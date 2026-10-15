// SPDX-License-Identifier: GPL-2.0-or-later
#include "util/files_in_use.h"

#include <windows.h>
#include <winioctl.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <format>
#include <future>
#include <mutex>
#include <optional>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "util/win_strings.h"

namespace knobs {
namespace {

namespace fs = std::filesystem;

// FolderFingerprint's limits, for each folder: an update writes a plugin's
// or NVIDIA's own folders, which are small, but a plugin can sit in a folder
// shared with many others.
constexpr int kFingerprintDepth = 2;
constexpr size_t kFingerprintFiles = 5000;
// Read by the shutdown watchdog without taking the file watchdog's mutex.
std::atomic<uint64_t> handoff_deadline = 0;

struct Handles {
  std::vector<HANDLE> values;
  ~Handles() {
    for (const HANDLE handle : values) CloseHandle(handle);
  }
};

std::wstring Normalized(const fs::path& path) {
  const fs::path normal = fs::path(path).lexically_normal().make_preferred();
  std::wstring text = normal.native();
  const size_t root = normal.root_path().native().size();
  while (text.size() > root && text.back() == L'\\') text.pop_back();
  return text;
}

bool SameText(std::wstring_view a, std::wstring_view b) {
  return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) ==
         CSTR_EQUAL;
}

// FNV-1a, so the same files sum to the same text from run to run.
void Mix(uint64_t& hash, const void* data, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    hash ^= static_cast<const unsigned char*>(data)[i];
    hash *= 0x100000001b3ull;
  }
}

}  // namespace

struct FileOplock::Request {
  fs::path path;
  HANDLE file = INVALID_HANDLE_VALUE;
  HANDLE event = nullptr;
  OVERLAPPED overlapped = {};
  REQUEST_OPLOCK_INPUT_BUFFER input = {};
  REQUEST_OPLOCK_OUTPUT_BUFFER output = {};
  bool pending = false;
  std::mutex mutex;
  bool retired = false;
  uint64_t since = 0;
  uint64_t limit = 10000, warning = 5000;
  bool expired = false;
  // Keep the break's access modes intact for the core to read later.
  OVERLAPPED ack = {};
  REQUEST_OPLOCK_INPUT_BUFFER ack_input = {REQUEST_OPLOCK_CURRENT_VERSION, sizeof(ack_input), 0,
                                         REQUEST_OPLOCK_INPUT_FLAG_ACK};
  REQUEST_OPLOCK_OUTPUT_BUFFER ack_output = {REQUEST_OPLOCK_CURRENT_VERSION, sizeof(ack_output)};
  bool ack_pending = false;
  DWORD ack_error = ERROR_SUCCESS;

  bool Broken() const { return pending && WaitForSingleObject(event, 0) == WAIT_OBJECT_0; }
  bool Reader() const {
    constexpr ACCESS_MASK modifying = FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE;
    return (output.Flags & REQUEST_OPLOCK_OUTPUT_FLAG_MODES_PROVIDED) && !(output.AccessMode & modifying);
  }

  void Release() {
    if (expired) return;
    // Acknowledge to None even if a restart has duplicated the handle.
    // Read handles can still cause a sharing violation, but no opener
    // waits for acknowledgment any more. Their owners close them.
    const BOOL done = DeviceIoControl(file, FSCTL_REQUEST_OPLOCK, &ack_input, sizeof(ack_input),
                                      &ack_output, sizeof(ack_output), nullptr, &ack);
    ack_error = done ? ERROR_SUCCESS : GetLastError();
    ack_pending = ack_error == ERROR_IO_PENDING;
    expired = true;
  }

  void Retire() {
    {
      std::lock_guard lock(mutex);
      if (retired) return;
      retired = true;
    }
    // Cancellation waits outside the watchdog's mutex. The event and buffers
    // remain alive until both requests complete.
    if (pending) {
      // Unbroken, cancellation releases the oplock. Broken, the request has
      // already completed, and only closing the last handle releases it.
      CancelIoEx(file, &overlapped);
      DWORD bytes = 0;
      GetOverlappedResult(file, &overlapped, &bytes, TRUE);
    }
    if (ack_pending) {
      CancelIoEx(file, &ack);
      DWORD bytes = 0;
      GetOverlappedResult(file, &ack, &bytes, TRUE);
    }
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
  }

  ~Request() {
    Retire();
    if (event) CloseHandle(event);
    if (ack.hEvent) CloseHandle(ack.hEvent);
  }

  static Result<std::shared_ptr<Request>> Take(const fs::path& path) {
    auto request = std::make_shared<Request>();
    request->path = path;
    request->file =
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (request->file == INVALID_HANDLE_VALUE) {
      return Error{std::format("Couldn't open {}: {}", ToUtf8(path), DescribeWinError(GetLastError()))};
    }
    request->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!request->event) return Error{std::format("Couldn't create an event: {}", DescribeWinError(GetLastError()))};
    request->ack.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!request->ack.hEvent) return Error{std::format("Couldn't create an event: {}", DescribeWinError(GetLastError()))};
    request->overlapped.hEvent = request->event;
    request->input = {REQUEST_OPLOCK_CURRENT_VERSION, sizeof(request->input),
                      OPLOCK_LEVEL_CACHE_READ | OPLOCK_LEVEL_CACHE_HANDLE, REQUEST_OPLOCK_INPUT_FLAG_REQUEST};
    request->output = {REQUEST_OPLOCK_CURRENT_VERSION, sizeof(request->output)};
    // Granted, the request stays pending until the oplock breaks. Done at
    // once, it broke as it was granted.
    const BOOL done = DeviceIoControl(request->file, FSCTL_REQUEST_OPLOCK, &request->input, sizeof(request->input),
                                      &request->output, sizeof(request->output), nullptr, &request->overlapped);
    const DWORD error = done ? ERROR_SUCCESS : GetLastError();
    request->pending = done || error == ERROR_IO_PENDING;
    if (error != ERROR_IO_PENDING) {
      return Error{std::format("Windows didn't grant an oplock on {}: {}", ToUtf8(path),
                               done ? "another program is opening it" : DescribeWinError(error))};
    }
    return request;
  }
};

struct FileOplockWatchdog {
  using Request = FileOplock::Request;
  HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  std::mutex mutex;
  std::vector<std::weak_ptr<Request>> requests;
  FileHoldStatus status;
  HWND window = nullptr;
  UINT message = 0;
  std::atomic<bool> stop = false;
  std::thread thread;

  FileOplockWatchdog() {
    std::promise<void> ready;
    auto started = ready.get_future();
    thread = std::thread([this, &ready] { ready.set_value(); Run(); });
    started.wait();
  }
  ~FileOplockWatchdog() {
    stop = true;
    SetEvent(wake);
    thread.join();
    CloseHandle(wake);
  }
  static FileOplockWatchdog& Get() {
    static FileOplockWatchdog watchdog;
    return watchdog;
  }
  void Add(const std::shared_ptr<Request>& request, std::chrono::milliseconds limit,
           std::chrono::milliseconds warning) {
    request->limit = static_cast<uint64_t>(std::max<int64_t>(1, limit.count()));
    request->warning = static_cast<uint64_t>(std::max<int64_t>(0, warning.count()));
    {
      std::lock_guard lock(mutex);
      requests.push_back(request);
    }
    SetEvent(wake);
  }
  void Run() {
    while (!stop) {
      std::vector<std::shared_ptr<Request>> active;
      {
        std::lock_guard lock(mutex);
        std::erase_if(requests, [](const auto& request) { return request.expired(); });
        for (const auto& request : requests) if (auto owned = request.lock()) active.push_back(std::move(owned));
      }
      std::vector<HANDLE> events{wake};
      DWORD delay = INFINITE;
      FileHoldStatus next;
      const uint64_t now = GetTickCount64();
      for (const auto& request : active) {
        std::lock_guard lock(request->mutex);
        if (request->retired) continue;
        if (!request->Broken()) {
          if (events.size() < MAXIMUM_WAIT_OBJECTS) events.push_back(request->event);
          else delay = std::min<DWORD>(delay, 50);  // More DLLs than one Windows wait can hold.
          continue;
        }
        if (!request->since) request->since = now;
        const uint64_t due = request->since + request->limit;
        if (now >= due) request->Release();
        if (request->ack_pending) {
          if (WaitForSingleObject(request->ack.hEvent, 0) == WAIT_OBJECT_0) {
            DWORD bytes = 0;
            request->ack_error = GetOverlappedResult(request->file, &request->ack, &bytes, FALSE)
                                     ? ERROR_SUCCESS : GetLastError();
            request->ack_pending = false;
          } else if (events.size() < MAXIMUM_WAIT_OBJECTS) {
            events.push_back(request->ack.hEvent);
          } else {
            delay = std::min<DWORD>(delay, 50);
          }
        }
        if (!request->Reader()) {
          if (request->expired && request->ack_error == ERROR_SUCCESS) next.released.push_back(request->path);
          else if (now >= request->since + request->warning) next.waiting = true;
        }
        if (request->expired && !request->ack_pending && request->ack_error != ERROR_SUCCESS) {
          next.errors.push_back(std::format("Couldn't release the file hold for {}: {}", ToUtf8(request->path),
                                            DescribeWinError(request->ack_error)));
        }
        if (!request->expired) {
          uint64_t next_due = due;
          if (now < request->since + request->warning) next_due = std::min(next_due, request->since + request->warning);
          delay = std::min(delay, static_cast<DWORD>(std::min<uint64_t>(next_due - now, INFINITE - 1)));
        }
      }
      {
        std::lock_guard lock(mutex);
        if (status != next) {
          status = std::move(next);
          if (window) PostMessageW(window, message, 0, 0);
        }
      }
      // Keep waited events alive. Retiring a request wakes this wait.
      WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), FALSE, delay);
    }
  }
  uint64_t Deadline(const std::vector<uintptr_t>& files) {
    uint64_t deadline = GetTickCount64() + 10000;
    std::lock_guard lock(mutex);
    for (const uintptr_t file : files) {
      bool found = false;
      for (const auto& weak : requests) if (auto request = weak.lock()) {
        if (reinterpret_cast<uintptr_t>(request->file) != file) continue;
        std::lock_guard hold(request->mutex);
        if (request->retired) continue;
        if (request->expired) return 0;
        if (!request->since) request->since = GetTickCount64();
        deadline = std::min(deadline, request->since + request->limit);
        found = true;
        break;
      }
      if (!found) return 0;
    }
    SetEvent(wake);
    return deadline;
  }
  void Release(const std::vector<uintptr_t>& files) {
    std::lock_guard lock(mutex);
    for (const auto& weak : requests) if (auto request = weak.lock()) {
      if (std::find(files.begin(), files.end(), reinterpret_cast<uintptr_t>(request->file)) == files.end()) continue;
      std::lock_guard hold(request->mutex);
      if (!request->retired) request->Release();
    }
    SetEvent(wake);
  }
};

FileHoldStatus FileHolds() {
  auto& watchdog = FileOplockWatchdog::Get();
  std::lock_guard lock(watchdog.mutex);
  return watchdog.status;
}

uint64_t FileHandoffDeadline() {
  return handoff_deadline.load();
}

void WatchFileHolds(uintptr_t window, unsigned message) {
  auto& watchdog = FileOplockWatchdog::Get();
  std::lock_guard lock(watchdog.mutex);
  watchdog.window = reinterpret_cast<HWND>(window);
  watchdog.message = message;
  PostMessageW(watchdog.window, message, 0, 0);
}

void StopWatchingFileHolds(uintptr_t window) {
  auto& watchdog = FileOplockWatchdog::Get();
  std::lock_guard lock(watchdog.mutex);
  if (watchdog.window == reinterpret_cast<HWND>(window)) watchdog.window = nullptr;
}

FileOplock::FileOplock(fs::path file) : file_(std::move(file)) {}

Result<std::unique_ptr<FileOplock>> FileOplock::Take(const fs::path& file, std::chrono::milliseconds limit,
                                                std::chrono::milliseconds warning) {
  auto request = Request::Take(file);
  if (!request) return Error{request.error()};
  std::unique_ptr<FileOplock> oplock(new FileOplock(file));
  oplock->request_ = std::move(*request);
  oplock->limit_ = limit;
  oplock->warning_ = warning;
  FileOplockWatchdog::Get().Add(oplock->request_, limit, warning);
  return oplock;
}

FileOplock::~FileOplock() {
  if (request_) request_->Retire();
  // Drop the owner before waking the watchdog. Its next wait must not keep
  // a retired request's events alive until another watch changes.
  request_.reset();
  SetEvent(FileOplockWatchdog::Get().wake);
}

bool FileOplock::wanted() {
  if (!request_) {
    auto request = Request::Take(file_);
    if (request) {
      request_ = std::move(*request);
      FileOplockWatchdog::Get().Add(request_, limit_, warning_);
    }
    return false;
  }
  if (!request_->Broken()) return false;
  if (request_->Reader()) {
    // A reader with incompatible sharing needs only our handle closed.
    // Retry on a later poll, so the reader gets to finish opening first.
    request_->Retire();
    request_.reset();
    SetEvent(FileOplockWatchdog::Get().wake);
    return false;
  }
  // Without modes, keep the conservative hold for the core to unload.
  return true;
}

uintptr_t FileOplock::handle() const { return request_ ? reinterpret_cast<uintptr_t>(request_->file) : 0; }

Status FileOplock::PrepareHandoff() {
  if (!request_) return Ok{};  // A reader temporarily has the file.
  {
    std::lock_guard lock(request_->mutex);
    if (request_->expired) return Error{"The file hold expired before it could be handed over."};
  }
  DWORD bytes = 0;
  if (!GetOverlappedResult(request_->file, &request_->overlapped, &bytes, FALSE) &&
      GetLastError() == ERROR_IO_INCOMPLETE) {
    // Even completion-port I/O is cancelled at process exit. Turn the
    // pending request into a break awaiting acknowledgment before handing it off.
    // An exclusive byte-range lock breaks RH to None and needs an ack, but
    // the lock operation itself doesn't wait for that ack (IRP_MJ_LOCK_CONTROL).
    // Use a separate open, with a different oplock key, and read access only.
    const HANDLE probe = CreateFileW(file_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, 0, nullptr);
    if (probe == INVALID_HANDLE_VALUE) {
      return Error{std::format("Couldn't prepare {} for handoff: {}", ToUtf8(file_), DescribeWinError(GetLastError()))};
    }
    OVERLAPPED range = {};  // The first byte, which a loaded DLL always has.
    const BOOL locked = LockFileEx(probe, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &range);
    const DWORD error = locked ? ERROR_SUCCESS : GetLastError();
    CloseHandle(probe);  // Releases the byte-range lock, not the oplock.
    if (!locked) {
      return Error{std::format("Couldn't prepare {} for handoff: {}", ToUtf8(file_), DescribeWinError(error))};
    }
  }
  // Don't hand off a request that could still be cancelled at exit, or a
  // break that no longer holds an opener. No waiting on the core thread.
  if (!GetOverlappedResult(request_->file, &request_->overlapped, &bytes, FALSE) ||
      !(request_->output.Flags & REQUEST_OPLOCK_OUTPUT_FLAG_ACK_REQUIRED)) {
    return Error{std::format("Windows didn't preserve the oplock on {} for handoff.", ToUtf8(file_))};
  }
  {
    std::lock_guard lock(request_->mutex);
    if (request_->expired) return Error{"The file hold expired before it could be handed over."};
    if (!request_->since) request_->since = GetTickCount64();
  }
  SetEvent(FileOplockWatchdog::Get().wake);
  return Ok{};
}

Status StartWithFileOplocks(const fs::path& exe, std::wstring_view arguments, const std::vector<uintptr_t>& files) {
  if (files.empty()) return Error{"No waiting files were passed to the restart."};
  const uint64_t deadline = FileOplockWatchdog::Get().Deadline(files);
  if (!deadline) return Error{"A file hold expired before the replacement could start."};
  Handles inherited;
  HANDLE process = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &process,
                       SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, 0)) {
    return Error{std::format("Couldn't pass this process to its replacement: {}", DescribeWinError(GetLastError()))};
  }
  inherited.values.push_back(process);
  for (const uintptr_t file : files) {
    HANDLE duplicate = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(file), GetCurrentProcess(), &duplicate,
                         0, TRUE, DUPLICATE_SAME_ACCESS)) {
      return Error{std::format("Couldn't pass a waiting file to the new process: {}", DescribeWinError(GetLastError()))};
    }
    inherited.values.push_back(duplicate);
  }
  std::wstring handoff;
  for (const HANDLE handle : inherited.values) {
    handoff += std::format(L"{}{:x}", handoff.empty() ? L"" : L",", reinterpret_cast<uintptr_t>(handle));
  }
  SECURITY_ATTRIBUTES security = {sizeof(security), nullptr, TRUE};
  const HANDLE ready = CreateEventW(&security, TRUE, FALSE, nullptr);
  if (!ready) return Error{std::format("Couldn't prepare the replacement's wait: {}", DescribeWinError(GetLastError()))};
  inherited.values.push_back(ready);
  std::wstring command = std::format(L"\"{}\" {} --wait-for-files {:x}:{:x}:{}", exe.native(), arguments,
                                      deadline, reinterpret_cast<uintptr_t>(ready), handoff);
  // An explicit list avoids inheriting any unrelated handles in the app.
  SIZE_T size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
  std::vector<unsigned char> storage(size);
  auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
  if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) {
    return Error{std::format("Couldn't prepare file handoff: {}", DescribeWinError(GetLastError()))};
  }
  STARTUPINFOEXW startup = {};
  startup.StartupInfo.cb = sizeof(startup);
  startup.lpAttributeList = attributes;
  PROCESS_INFORMATION child = {};
  const bool started =
      UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.values.data(),
                                inherited.values.size() * sizeof(HANDLE), nullptr, nullptr) &&
      CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT,
                     nullptr, exe.parent_path().c_str(), &startup.StartupInfo, &child);
  const DWORD error = started ? ERROR_SUCCESS : GetLastError();
  DeleteProcThreadAttributeList(attributes);
  if (!started) return Error{std::format("Couldn't restart with the waiting files: {}", DescribeWinError(error))};
  Handles child_handles{{child.hThread, child.hProcess}};
  const HANDLE events[] = {ready, child.hProcess};
  const uint64_t now = GetTickCount64();
  const DWORD left = deadline > now ? static_cast<DWORD>(std::min<uint64_t>(deadline - now, 1000)) : 0;
  if (WaitForMultipleObjects(2, events, FALSE, left) != WAIT_OBJECT_0 || GetTickCount64() >= deadline) {
    // Do not leave a replacement that never reached its bounded wait holding
    // duplicate files. Acknowledge before stopping it, even if its exit stalls.
    FileOplockWatchdog::Get().Release(files);
    TerminateProcess(child.hProcess, ERROR_TIMEOUT);
    return Error{"The replacement didn't start waiting for the files in time."};
  }
  uint64_t previous = handoff_deadline.load();
  while ((!previous || deadline < previous) && !handoff_deadline.compare_exchange_weak(previous, deadline)) {}
  return Ok{};
}

Status WaitForFileOplocks(std::wstring_view handoff) {
  const std::string text = ToUtf8(handoff);
  std::string_view rest = text;
  // Every inherited handle is closed as this returns, even after a bad one,
  // so a handoff that doesn't check out can't keep an installer waiting.
  Handles inherited;
  Handles readiness;
  std::optional<std::string> invalid;
  uint64_t deadline = GetTickCount64() + 30000;
  if (const size_t colon = rest.find(':'); colon != std::string_view::npos) {
    const auto parsed = std::from_chars(rest.data(), rest.data() + colon, deadline, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != rest.data() + colon) invalid = "The restart's file deadline is invalid.";
    rest.remove_prefix(colon + 1);
    if (const size_t ready_end = rest.find(':'); ready_end != std::string_view::npos) {
      uintptr_t value = 0;
      const auto event = std::from_chars(rest.data(), rest.data() + ready_end, value, 16);
      const HANDLE handle = reinterpret_cast<HANDLE>(value);
      DWORD flags = 0;
      if (event.ec != std::errc{} || event.ptr != rest.data() + ready_end ||
          !GetHandleInformation(handle, &flags) || !(flags & HANDLE_FLAG_INHERIT)) {
        invalid = "The restart's readiness event is invalid.";
      } else {
        readiness.values.push_back(handle);
        SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);
      }
      rest.remove_prefix(ready_end + 1);
    }
  }
  while (!rest.empty()) {
    const size_t comma = rest.find(',');
    const std::string_view part = rest.substr(0, comma);
    uintptr_t value = 0;
    const auto parsed = std::from_chars(part.data(), part.data() + part.size(), value, 16);
    const HANDLE handle = reinterpret_cast<HANDLE>(value);
    DWORD flags = 0;
    if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size() ||
        !GetHandleInformation(handle, &flags) || !(flags & HANDLE_FLAG_INHERIT) ||
        std::find(inherited.values.begin(), inherited.values.end(), handle) != inherited.values.end()) {
      if (!invalid) invalid = "The restart's file handoff is invalid.";
    } else {
      // Only the inherited process and disk files expected by this protocol.
      if (inherited.values.empty()) {
        const DWORD pid = GetProcessId(handle);
        if ((!pid || pid == GetCurrentProcessId()) && !invalid) invalid = "The restart's old process handle is invalid.";
      } else if (GetFileType(handle) != FILE_TYPE_DISK && !invalid) {
        invalid = "The restart's waiting file handle is invalid.";
      }
      inherited.values.push_back(handle);
      SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);
    }
    if (comma == std::string_view::npos) break;
    rest.remove_prefix(comma + 1);
  }
  if (invalid) return Error{*invalid};
  if (inherited.values.size() < 2) return Error{"The restart didn't inherit its waiting files."};
  if (!readiness.values.empty() && !SetEvent(readiness.values.front())) {
    return Error{"The restart's readiness event is invalid."};
  }
  // Windows signals the process only once its mappings are gone. Closing
  // these duplicates then acknowledges the breaks and releases the writers.
  const uint64_t now = GetTickCount64();
  const DWORD left = deadline > now ? static_cast<DWORD>(std::min<uint64_t>(deadline - now, 30000)) : 0;
  const DWORD waited = WaitForSingleObject(inherited.values.front(), left);
  if (waited == WAIT_TIMEOUT) return Error{"The file hold expired before the old process exited."};
  if (waited != WAIT_OBJECT_0) {
    return Error{std::format("Couldn't wait for the old process to exit: {}", DescribeWinError(GetLastError()))};
  }
  return Ok{};
}

std::vector<fs::path> LoadedModules(const std::function<void(std::string_view)>& log, bool* complete) {
  if (complete) *complete = false;
  std::vector<HMODULE> modules(256);
  DWORD error = ERROR_SUCCESS;
  bool listed = false;
  for (int attempt = 0; attempt < 4; ++attempt) {
    DWORD needed = 0;
    const DWORD size = static_cast<DWORD>(modules.size() * sizeof(HMODULE));
    if (!EnumProcessModulesEx(GetCurrentProcess(), modules.data(), size, &needed, LIST_MODULES_ALL)) {
      error = GetLastError();
      continue;  // Module loads and unloads can race the scan.
    }
    if (needed <= size) {
      modules.resize(needed / sizeof(HMODULE));
      listed = true;
      break;
    }
    error = ERROR_MORE_DATA;
    modules.resize(needed / sizeof(HMODULE) + 16);
  }
  if (!listed) {
    if (log) log(std::format("Couldn't list loaded DLLs after four tries: {}", DescribeWinError(error)));
    return {};
  }
  bool paths_complete = true;
  std::vector<fs::path> files;
  files.reserve(modules.size());
  std::wstring path(MAX_PATH, L'\0');
  for (const HMODULE module : modules) {
    for (;;) {
      const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
      if (length == 0) {
        // It may have unloaded since enumeration. Callers deciding which
        // watches to release must not treat this as a complete module list.
        if (paths_complete && log) {
          log(std::format("Couldn't read a loaded DLL's path: {}", DescribeWinError(GetLastError())));
        }
        paths_complete = false;
        break;
      }
      if (length < path.size()) {
        files.emplace_back(std::wstring_view(path.data(), length));
        break;
      }
      path.resize(path.size() * 2);
    }
  }
  if (complete) *complete = paths_complete;
  return files;
}

bool SamePath(const fs::path& a, const fs::path& b) {
  return !a.empty() && !b.empty() && SameText(Normalized(a), Normalized(b));
}

bool InFolder(const fs::path& file, const fs::path& folder) {
  std::wstring inside = Normalized(folder);
  if (inside.empty()) return false;
  if (inside.back() != L'\\') inside += L'\\';
  const std::wstring path = Normalized(file);
  return path.size() > inside.size() && SameText(std::wstring_view(path).substr(0, inside.size()), inside);
}

std::string FolderFingerprint(const std::vector<fs::path>& folders) {
  uint64_t hash = 0xcbf29ce484222325ull;
  struct Entry {
    std::wstring name;
    uint64_t size = 0;
    FILETIME written = {};
    DWORD error = ERROR_SUCCESS;
  };
  std::vector<Entry> entries;
  std::string missing;
  for (const fs::path& folder : folders) {
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) {
      missing += std::format(" missing {}", ToUtf8(folder));
      continue;
    }
    // A big shared folder mustn't hide the plugins' folders after it.
    const size_t first = entries.size();
    fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator() && entries.size() - first < kFingerprintFiles;
         it.increment(ec)) {
      if (it.depth() >= kFingerprintDepth) it.disable_recursion_pending();
      Entry entry{it->path().native()};
      // NTFS directory entries can still have the size and time from before
      // a completed write. Ask the file itself, sharing every kind of access
      // so this read doesn't hold up the installer.
      const HANDLE file = CreateFileW(entry.name.c_str(), FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
      BY_HANDLE_FILE_INFORMATION info = {};
      if (file == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file, &info)) {
        entry.error = GetLastError();
      } else {
        entry.size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        entry.written = info.ftLastWriteTime;
      }
      if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
      entries.push_back(std::move(entry));
    }
    if (ec) missing += std::format(" unreadable {}: {}", ToUtf8(folder), ec.value());
  }
  // Directory enumeration order isn't a promise of the filesystem.
  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
  for (const Entry& entry : entries) {
    Mix(hash, entry.name.data(), entry.name.size() * sizeof(wchar_t));
    Mix(hash, &entry.size, sizeof(entry.size));
    Mix(hash, &entry.written, sizeof(entry.written));
    Mix(hash, &entry.error, sizeof(entry.error));
  }
  return std::format("{:016x} {} files{}", hash, entries.size(), missing);
}

}  // namespace knobs
