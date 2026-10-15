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
#include <optional>
#include <string_view>
#include <system_error>
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

struct Handles {
  std::vector<HANDLE> values;
  ~Handles() {
    for (const HANDLE handle : values) CloseHandle(handle);
  }
};

std::wstring Normalized(const fs::path& path) { return fs::path(path).lexically_normal().make_preferred().native(); }

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
  HANDLE file = INVALID_HANDLE_VALUE;
  HANDLE event = nullptr;
  HANDLE wait = nullptr;
  OVERLAPPED overlapped = {};
  REQUEST_OPLOCK_INPUT_BUFFER input = {};
  REQUEST_OPLOCK_OUTPUT_BUFFER output = {};
  bool pending = false;
  std::atomic<bool> completed = false;

  // Nothing owning-thread-specific runs here, even during shutdown.
  static void CALLBACK Broken(void* context, BOOLEAN) {
    static_cast<Request*>(context)->completed = true;
  }

  ~Request() {
    if (wait) UnregisterWaitEx(wait, INVALID_HANDLE_VALUE);
    if (pending) {
      // Unbroken, cancellation releases the oplock. Broken, the request has
      // already completed, and only closing the last handle releases it.
      CancelIoEx(file, &overlapped);
      DWORD bytes = 0;
      GetOverlappedResult(file, &overlapped, &bytes, TRUE);
    }
    // Closing the last handle acknowledges a break and lets the opener on.
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (event) CloseHandle(event);
  }

  static Result<std::unique_ptr<Request>> Take(const fs::path& path) {
    auto request = std::make_unique<Request>();
    request->file =
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (request->file == INVALID_HANDLE_VALUE) {
      return Error{std::format("Couldn't open {}: {}", ToUtf8(path), DescribeWinError(GetLastError()))};
    }
    request->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!request->event) return Error{std::format("Couldn't create an event: {}", DescribeWinError(GetLastError()))};
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
    if (!RegisterWaitForSingleObject(&request->wait, request->event, &Request::Broken, request.get(), INFINITE,
                                     WT_EXECUTEONLYONCE)) {
      return Error{std::format("Couldn't wait for the oplock on {}: {}", ToUtf8(path), DescribeWinError(GetLastError()))};
    }
    return request;
  }
};

FileOplock::FileOplock(fs::path file) : file_(std::move(file)) {}

Result<std::unique_ptr<FileOplock>> FileOplock::Take(const fs::path& file) {
  auto request = Request::Take(file);
  if (!request) return Error{request.error()};
  std::unique_ptr<FileOplock> oplock(new FileOplock(file));
  oplock->request_ = std::move(*request);
  return oplock;
}

FileOplock::~FileOplock() = default;

bool FileOplock::wanted() {
  if (!request_) {
    auto request = Request::Take(file_);
    if (request) request_ = std::move(*request);
    return false;
  }
  if (!request_->completed) return false;
  const auto& output = request_->output;
  constexpr ACCESS_MASK modifying = FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE;
  if ((output.Flags & REQUEST_OPLOCK_OUTPUT_FLAG_MODES_PROVIDED) && !(output.AccessMode & modifying)) {
    // A reader with incompatible sharing needs only our handle closed.
    // Retry on a later poll, so the reader gets to finish opening first.
    request_.reset();
    return false;
  }
  // Without modes, keep the conservative hold for the core to unload.
  return true;
}

uintptr_t FileOplock::handle() const { return request_ ? reinterpret_cast<uintptr_t>(request_->file) : 0; }

Status FileOplock::PrepareHandoff() {
  if (!request_) return Ok{};  // A reader temporarily has the file.
  DWORD bytes = 0;
  if (!GetOverlappedResult(request_->file, &request_->overlapped, &bytes, FALSE) &&
      GetLastError() == ERROR_IO_INCOMPLETE) {
    // Even completion-port I/O is cancelled at process exit. Turn the
    // pending request into an unacknowledged break before handing it off.
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
  return Ok{};
}

Status StartWithFileOplocks(const fs::path& exe, std::wstring_view arguments, const std::vector<uintptr_t>& files) {
  if (files.empty()) return Error{"No waiting files were passed to the restart."};
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
  std::wstring command = std::format(L"\"{}\" {} --wait-for-files {}", exe.native(), arguments, handoff);
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
  CloseHandle(child.hThread);
  CloseHandle(child.hProcess);
  return Ok{};
}

Status WaitForFileOplocks(std::wstring_view handoff) {
  const std::string text = ToUtf8(handoff);
  std::string_view rest = text;
  // Every inherited handle is closed as this returns, even after a bad one,
  // so a handoff that doesn't check out can't keep an installer waiting.
  Handles inherited;
  std::optional<std::string> invalid;
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
  // Windows signals the process only once its mappings are gone. Closing
  // these duplicates then acknowledges the breaks and releases the writers.
  const DWORD waited = WaitForSingleObject(inherited.values.front(), 30000);
  if (waited == WAIT_TIMEOUT) return Error{"The old process didn't exit within 30 seconds."};
  if (waited != WAIT_OBJECT_0) {
    return Error{std::format("Couldn't wait for the old process to exit: {}", DescribeWinError(GetLastError()))};
  }
  return Ok{};
}

std::vector<fs::path> LoadedModules() {
  std::vector<HMODULE> modules(256);
  for (;;) {
    DWORD needed = 0;
    const DWORD size = static_cast<DWORD>(modules.size() * sizeof(HMODULE));
    if (!EnumProcessModulesEx(GetCurrentProcess(), modules.data(), size, &needed, LIST_MODULES_ALL)) return {};
    if (needed <= size) {
      modules.resize(needed / sizeof(HMODULE));
      break;
    }
    modules.resize(needed / sizeof(HMODULE) + 16);
  }
  std::vector<fs::path> files;
  files.reserve(modules.size());
  std::wstring path(MAX_PATH, L'\0');
  for (const HMODULE module : modules) {
    for (;;) {
      const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
      // Gone since the list was made.
      if (length == 0) break;
      if (length < path.size()) {
        files.emplace_back(std::wstring_view(path.data(), length));
        break;
      }
      path.resize(path.size() * 2);
    }
  }
  return files;
}

bool SamePath(const fs::path& a, const fs::path& b) { return SameText(Normalized(a), Normalized(b)); }

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
