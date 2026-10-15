// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/runtime_copy.h"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <system_error>

#include "app_info.h"
#include "runtime/obs_layout.h"
#include "runtime/pe_imports.h"
#include "util/win_strings.h"

namespace knobs::runtime {
namespace {

namespace fs = std::filesystem;

// Written last, so its presence marks a complete copy. Records each file's
// size and the install file's timestamp, so a copy is only reused while the
// install still has exactly the files it was made from. OBS drops beta and RC
// suffixes from obs.dll's version resource, so the version alone can't tell
// 32.3.0-beta1 from 32.3.0.
constexpr wchar_t kManifestName[] = L"knobs-runtime.txt";
constexpr std::string_view kManifestHeader = "knobs-runtime 2";

std::string Describe(const std::error_code& ec) {
  return ec.category() == std::system_category() ? DescribeWinError(static_cast<unsigned long>(ec.value()))
                                                 : ec.message();
}

// The optional modules an install has (kOptionalObsModules).
std::vector<std::string_view> OptionalModulesIn(const fs::path& install_root, const ObsVersion& version) {
  std::vector<std::string_view> present;
  for (const OptionalObsModule& module : kOptionalObsModules) {
    std::error_code ec;
    if (fs::is_regular_file(PluginDll(install_root, module.name, version), ec)) present.push_back(module.name);
  }
  return present;
}

// The binaries the file set is built from. Recorded in the manifest so a
// knobs that loads more modules, or an install that has an optional module
// it didn't have, doesn't reuse a copy made without them. A copy that
// couldn't copy an optional module's files leaves it out of its line, so the
// next start copies it again.
std::string RootsLine(const fs::path& install_root, const ObsVersion& version,
                      const std::set<std::string, std::less<>>& left_out = {}) {
  std::string roots(kGraphicsModule);
  for (std::string_view module : kObsModules) roots += std::format(" {}", module);
  for (std::string_view module : OptionalModulesIn(install_root, version)) {
    if (!left_out.contains(module)) roots += std::format(" {}", module);
  }
  return roots;
}

int64_t WriteTime(const fs::path& file, std::error_code& ec) {
  return fs::last_write_time(file, ec).time_since_epoch().count();
}

// The copies are knobs's own. A read-only attribute, such as one carried over
// from the install, would block flushing a file, deleting it and replacing
// it, and makes a DLL look loaded to CheckFolder.
void ClearReadOnly(const fs::path& file) {
  const DWORD attributes = GetFileAttributesW(file.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY)) {
    SetFileAttributesW(file.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
  }
}

// Forces a file's data to disk, so the manifest can't reach the disk before
// the files it vouches for.
bool FlushToDisk(const fs::path& file) {
  HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  const bool flushed = FlushFileBuffers(handle);
  CloseHandle(handle);
  return flushed;
}

// DLLs in `dir`, keyed by lowercase file name.
std::map<std::string, fs::path> ListDlls(const fs::path& dir) {
  std::map<std::string, fs::path> dlls;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (entry.is_regular_file(ec) && AsciiLower(ToUtf8(entry.path().extension())) == ".dll") {
      dlls.emplace(AsciiLower(ToUtf8(entry.path().filename())), entry.path());
    }
  }
  return dlls;
}

// Whether knobs reads an optional module's data file. It starts libobs in
// en-US (ObsSession) and runs only audio filters, so a module's other
// locales, and the effects only its video filters draw with, stay behind.
bool KnobsReadsOptionalData(const fs::path& file) {
  const auto is = [](const fs::path& part, std::wstring_view name) {
    return CompareStringOrdinal(part.c_str(), -1, name.data(), static_cast<int>(name.size()), TRUE) == CSTR_EQUAL;
  };
  if (is(file.extension(), L".effect")) return false;
  return !is(file.parent_path().filename(), L"locale") || is(file.filename(), L"en-US.ini");
}

// Splits "<number> <rest>". Returns false if there's no valid number.
template <typename Number>
bool SplitNumber(std::string_view& text, Number& number) {
  const size_t space = text.find(' ');
  if (space == std::string_view::npos ||
      std::from_chars(text.data(), text.data() + space, number).ec != std::errc()) {
    return false;
  }
  text.remove_prefix(space + 1);
  return true;
}

struct ManifestSummary {
  size_t file_count = 0;
  uint64_t total_bytes = 0;
  std::vector<std::string> skipped;  // RuntimeCopy::skipped
};

// Returns the summary if `copy_root` holds a complete copy of `install`: made
// for its version and the current module list, every file still whole, and
// every install file unchanged since the copy was made, the main DLLs of the
// optional modules it left out included, so a module fixed since is copied.
std::optional<ManifestSummary> ReadIntactManifest(const fs::path& copy_root, const ObsInstall& install) {
  std::ifstream in(copy_root / kManifestName, std::ios::binary);
  std::string line;
  if (!in || !std::getline(in, line) || line != kManifestHeader) return std::nullopt;

  ManifestSummary summary;
  bool version_matches = false;
  bool roots_match = false;
  while (std::getline(in, line)) {
    std::string_view rest = line;
    if (rest.starts_with("obs-version ")) {
      version_matches = rest.substr(12) == install.version.ToString();
    } else if (rest.starts_with("roots ")) {
      roots_match = rest.substr(6) == RootsLine(install.root, install.version);
    } else if (rest.starts_with("skipped ")) {
      rest.remove_prefix(8);
      uint64_t size = 0;
      int64_t write_time = 0;
      if (!SplitNumber(rest, size) || !SplitNumber(rest, write_time) ||
          std::none_of(kOptionalObsModules.begin(), kOptionalObsModules.end(),
                       [&](const OptionalObsModule& module) { return module.name == rest; })) {
        return std::nullopt;
      }
      const fs::path dll = PluginDll(install.root, rest, install.version);
      std::error_code ec;
      if (fs::file_size(dll, ec) != size || ec) return std::nullopt;
      if (WriteTime(dll, ec) != write_time || ec) return std::nullopt;
      summary.skipped.push_back(std::format("{}: its files couldn't be read when the copy was made", rest));
    } else if (rest.starts_with("file ")) {
      rest.remove_prefix(5);
      uint64_t size = 0;
      int64_t write_time = 0;
      if (!SplitNumber(rest, size) || !SplitNumber(rest, write_time)) return std::nullopt;
      const fs::path file = FromUtf8(rest);
      std::error_code ec;
      if (fs::file_size(copy_root / file, ec) != size || ec) return std::nullopt;
      if (fs::file_size(install.root / file, ec) != size || ec) return std::nullopt;
      if (WriteTime(install.root / file, ec) != write_time || ec) return std::nullopt;
      ++summary.file_count;
      summary.total_bytes += size;
    }
  }
  if (!version_matches || !roots_match || summary.file_count == 0) return std::nullopt;
  return summary;
}

Status WriteManifest(const fs::path& copy_root, const ObsInstall& install, const RuntimeFileSet& file_set,
                     const std::set<std::string, std::less<>>& left_out) {
  const fs::path manifest = copy_root / kManifestName;
  std::ofstream out(manifest, std::ios::binary | std::ios::trunc);
  out << kManifestHeader << '\n'
      << "obs-version " << install.version.ToString() << '\n'
      << "roots " << RootsLine(install.root, install.version, left_out) << '\n'
      << "source " << ToObsPath(install.root) << '\n';
  for (const std::string_view module : OptionalModulesIn(install.root, install.version)) {
    const std::string prefix = std::format("{}: ", module);
    if (std::none_of(file_set.skipped.begin(), file_set.skipped.end(),
                     [&](const std::string& skipped) { return skipped.starts_with(prefix); })) {
      continue;
    }
    // Left out: copied again once its DLL changes.
    const fs::path dll = PluginDll(install.root, module, install.version);
    std::error_code size_error;
    std::error_code time_error;
    const uint64_t size = fs::file_size(dll, size_error);
    const int64_t write_time = WriteTime(dll, time_error);
    if (size_error || time_error) {
      return Error{std::format("Couldn't read {}: {}", ToUtf8(dll), Describe(size_error ? size_error : time_error))};
    }
    out << "skipped " << size << ' ' << write_time << ' ' << module << '\n';
  }
  for (const fs::path& file : file_set.files) {
    std::error_code size_error;
    std::error_code time_error;
    const uint64_t size = fs::file_size(install.root / file, size_error);
    const int64_t write_time = WriteTime(install.root / file, time_error);
    if (size_error || time_error) {
      return Error{std::format("Couldn't read {}: {}", ToUtf8(install.root / file),
                               Describe(size_error ? size_error : time_error))};
    }
    out << "file " << size << ' ' << write_time << ' ' << ToObsPath(file) << '\n';
  }
  out.close();
  if (!out || !FlushToDisk(manifest)) {
    return Error{std::format("Couldn't write {}.", ToUtf8(manifest))};
  }
  return Ok{};
}

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

// A folder in the runtime base that knobs made, known by its name.
struct CopyFolder {
  enum Kind { kCopy, kStaging, kRetired };

  fs::path path;
  Kind kind = kCopy;
  std::string version;  // "32.2.2"
};

// "32.2.2" is a copy, "32.2.2.partial" a copy being made, and
// "32.2.2.old-<n>" a copy set aside. Any other name isn't knobs's.
std::optional<CopyFolder> ParseCopyFolder(const fs::path& path) {
  const std::string name = AsciiLower(ToUtf8(path.filename()));
  size_t end = 0;
  for (int part = 0; part < 3; ++part) {
    if (part > 0) {
      if (end == name.size() || name[end] != '.') return std::nullopt;
      ++end;
    }
    const size_t digits = end;
    while (end < name.size() && IsDigit(name[end])) ++end;
    if (end == digits) return std::nullopt;
  }
  CopyFolder folder{path, CopyFolder::kCopy, name.substr(0, end)};
  constexpr std::string_view kRetiredMark = ".old-";
  const std::string_view rest = std::string_view(name).substr(end);
  if (rest == ".partial") {
    folder.kind = CopyFolder::kStaging;
  } else if (rest.starts_with(kRetiredMark) && rest.size() > kRetiredMark.size() &&
             std::all_of(rest.begin() + kRetiredMark.size(), rest.end(), IsDigit)) {
    folder.kind = CopyFolder::kRetired;
  } else if (!rest.empty()) {
    return std::nullopt;
  }
  return folder;
}

// The folders knobs made in the runtime base. Links are left out: knobs
// makes none, and one could lead anywhere.
std::vector<CopyFolder> ListCopyFolders(const fs::path& runtime_base) {
  std::vector<CopyFolder> folders;
  std::error_code ec;
  for (auto it = fs::directory_iterator(runtime_base, ec); !ec && it != fs::directory_iterator();
       it.increment(ec)) {
    std::error_code status_error;
    if (it->symlink_status(status_error).type() != fs::file_type::directory) continue;
    if (auto folder = ParseCopyFolder(it->path())) folders.push_back(std::move(*folder));
  }
  return folders;
}

// An unused name to set a copy of `version` aside under: <version>.old-<n>.
fs::path RetiredPath(const fs::path& runtime_base, const std::string& version) {
  std::error_code ec;
  for (uint64_t n = GetTickCount64();; ++n) {
    fs::path path = runtime_base / FromUtf8(std::format("{}.old-{}", version, n));
    if (!fs::exists(path, ec)) return path;
  }
}

// Whether pruning has been asked to stop (PruneOptions::stop_requested).
bool Stopped(const std::function<bool()>& stop_requested) { return stop_requested && stop_requested(); }

// Waits for `time` unless asked to stop first. Returns false if it was.
bool WaitUnlessStopped(std::chrono::milliseconds time, const std::function<bool()>& stop_requested) {
  constexpr std::chrono::milliseconds kSlice(10);
  const auto end = std::chrono::steady_clock::now() + time;
  while (!Stopped(stop_requested)) {
    const auto left = std::chrono::ceil<std::chrono::milliseconds>(end - std::chrono::steady_clock::now());
    if (left <= std::chrono::milliseconds::zero()) return true;
    Sleep(static_cast<DWORD>(std::min(left, kSlice).count()));
  }
  return false;
}

// How many times setting a folder aside is tried before it's left for next
// time, waiting 100 ms longer after each try: about a second in all.
constexpr int kSetAsideAttempts = 5;

struct FolderCheck {
  uint64_t bytes = 0;
  std::string in_use;  // Why the folder can't go now, if it can't.
};

// Looks through a folder before it's removed: adds up its files, makes them
// writable, and looks for a DLL a program has loaded from it. A loaded DLL
// doesn't keep its folder from being renamed, but Windows won't open it for
// writing, which the check tries without writing anything. Asked to stop, it
// stops looking, and the caller sees that.
FolderCheck CheckFolder(const fs::path& folder, const std::function<bool()>& stop_requested) {
  FolderCheck check;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(folder, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    if (Stopped(stop_requested)) return check;
    // Links aren't followed, and only files are looked at.
    const fs::file_type type = it->symlink_status(ec).type();
    if (ec) break;
    if (type != fs::file_type::regular) continue;
    const uintmax_t size = it->file_size(ec);
    if (ec) break;
    check.bytes += size;
    const fs::path& file = it->path();
    ClearReadOnly(file);
    if (AsciiLower(ToUtf8(file.extension())) != ".dll") continue;
    HANDLE handle = CreateFileW(file.c_str(), FILE_WRITE_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      check.in_use = std::format("{} is in use: {}", ToObsPath(file.lexically_relative(folder)),
                                 DescribeWinError(GetLastError()));
      return check;
    }
    CloseHandle(handle);
  }
  if (ec) check.in_use = std::format("couldn't look through it: {}", Describe(ec));
  return check;
}

// Deletes a folder and what's in it, as fs::remove_all does, links included
// but not followed. Asked to stop, it stops between files. Returns whether
// the folder is gone; if it isn't and it wasn't asked to stop, `ec` says why.
bool RemoveAll(const fs::path& folder, const std::function<bool()>& stop_requested, std::error_code& ec) {
  for (auto it = fs::directory_iterator(folder, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
    if (Stopped(stop_requested)) return false;
    const fs::file_type type = it->symlink_status(ec).type();
    if (ec) return false;
    if (type == fs::file_type::directory) {
      if (!RemoveAll(it->path(), stop_requested, ec)) return false;
    } else {
      fs::remove(it->path(), ec);  // A link goes, not what it leads to.
      if (ec) return false;
    }
  }
  if (ec) return false;
  fs::remove(folder, ec);
  return !ec;
}

// Removes a runtime copy, or a folder a copy left behind, unless a program
// uses it. It's renamed aside first, which fails as a whole while a process
// has a file open in it or works in it, and only what was renamed is
// deleted, so a copy in use is never left half deleted. Says why the folder
// stays, or nothing once it's gone. Call with the RuntimeCopyLock held, so
// no process is between making a copy and loading it.
std::optional<PrunedCopies::Left> RemoveCopyFolder(const fs::path& runtime_base, const CopyFolder& folder,
                                                   const std::function<bool()>& stop_requested, uint64_t& bytes) {
  const FolderCheck check = CheckFolder(folder.path, stop_requested);
  if (!check.in_use.empty()) return PrunedCopies::Left{folder.path, check.in_use};
  if (Stopped(stop_requested)) return PrunedCopies::Left{folder.path, "pruning stopped before it was set aside"};
  const fs::path retired = RetiredPath(runtime_base, folder.version);
  // Antivirus and the search indexer briefly hold handles to files, which
  // makes renaming their folder fail for a moment, as in EnsureRuntimeCopy. A
  // program that uses the copy holds its handles for longer, so the tries end
  // after about a second.
  std::error_code ec;
  for (int attempt = 1;; ++attempt) {
    fs::rename(folder.path, retired, ec);
    if (!ec || attempt == kSetAsideAttempts ||
        !WaitUnlessStopped(std::chrono::milliseconds(100 * attempt), stop_requested)) {
      break;
    }
  }
  if (ec) {
    return PrunedCopies::Left{
        folder.path, std::format("couldn't set it aside, so a program may be using it: {}", Describe(ec))};
  }
  // What doesn't go now is a <version>.old-<n> folder, for next time.
  if (!RemoveAll(retired, stop_requested, ec)) {
    return PrunedCopies::Left{retired, ec ? std::format("couldn't delete it: {}", Describe(ec))
                                          : std::string("pruning stopped while it was being deleted")};
  }
  bytes = check.bytes;
  return std::nullopt;
}

// Whether `name`, from an import table, names a file in a folder and nothing
// more. One with a path in it would reach outside the folder.
bool IsPlainFileName(std::string_view name) {
  return !name.empty() && name != "." && name != ".." && name.find_first_of("\\/:") == std::string_view::npos;
}

// Copies `from` to `to`, which isn't there, whole: into a file beside it,
// flushed to disk, then renamed into place, without replacing a file that's
// there by then.
Status CopyWhole(const fs::path& from, const fs::path& to) {
  fs::path part = to;
  part += L".partial";
  std::error_code ec;
  fs::copy_file(from, part, fs::copy_options::overwrite_existing, ec);
  if (ec) return Error{std::format("Couldn't copy {} to {}: {}", ToUtf8(from), ToUtf8(part), Describe(ec))};
  ClearReadOnly(part);
  std::string error;
  if (!FlushToDisk(part)) {
    error = std::format("Couldn't write {}: {}", ToUtf8(part), DescribeWinError(GetLastError()));
  } else if (!MoveFileExW(part.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) {
    error = std::format("Couldn't move {} to {}: {}", ToUtf8(part), ToUtf8(to), DescribeWinError(GetLastError()));
  }
  if (error.empty()) return Ok{};
  fs::remove(part, ec);
  return Error{error};
}

// Deletes <version>.old-<n> folders left by earlier swaps. Ones still in use
// by a running knobs stay until later.
void RemoveRetiredCopies(const fs::path& runtime_base, const std::string& version) {
  for (const CopyFolder& folder : ListCopyFolders(runtime_base)) {
    if (folder.kind != CopyFolder::kRetired || folder.version != version) continue;
    uint64_t bytes = 0;
    RemoveCopyFolder(runtime_base, folder, {}, bytes);
  }
}

}  // namespace

std::wstring RuntimeCopyLockName() { return std::format(L"Local\\{}-runtime-copy", kDisplayNameW); }

RuntimeCopyLock::RuntimeCopyLock(bool wait, const std::wstring& name) {
  mutex_ = CreateMutexW(nullptr, FALSE, name.c_str());
  if (!mutex_) return;
  const DWORD result = WaitForSingleObject(mutex_, wait ? INFINITE : 0);
  locked_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
}

RuntimeCopyLock::~RuntimeCopyLock() {
  if (locked_) ReleaseMutex(mutex_);
  if (mutex_) CloseHandle(mutex_);
}

Result<RuntimeFileSet> PlanRuntimeCopy(const fs::path& install_root, const ObsVersion& version) {
  const fs::path bin = BinDir(install_root);
  const std::vector<std::string_view> optional = OptionalModulesIn(install_root, version);
  std::map<fs::path, std::map<std::string, fs::path>> dlls_by_dir = {{bin, ListDlls(bin)}};
  const auto list_dir = [&](std::string_view module) {
    const fs::path dir = PluginBinDir(install_root, module, version);
    if (!dlls_by_dir.contains(dir)) dlls_by_dir.emplace(dir, ListDlls(dir));
  };
  for (std::string_view module : kObsModules) list_dir(module);
  for (std::string_view module : optional) list_dir(module);
  const auto find_dll = [&](const fs::path& dir, const std::string& lower_name) -> const fs::path* {
    const auto dir_it = dlls_by_dir.find(dir);
    if (dir_it == dlls_by_dir.end()) return nullptr;
    const auto it = dir_it->second.find(lower_name);
    return it == dir_it->second.end() ? nullptr : &it->second;
  };

  // What one set of roots adds: the DLLs in their import closure that aren't
  // in `known` yet, the PDBs next to them, and the files in `data_dirs`, for
  // an optional module only those knobs reads.
  struct Part {
    std::set<fs::path> binaries;
    std::vector<fs::path> files;
    uint64_t bytes = 0;
    std::map<std::string, std::string> system_dlls;  // Lowercase name -> as spelled.
  };
  const auto plan_part = [&](std::vector<fs::path> pending, const std::vector<fs::path>& data_dirs,
                             const std::set<fs::path>& known, bool optional) -> Result<Part> {
    Part part;
    while (!pending.empty()) {
      const fs::path binary = std::move(pending.back());
      pending.pop_back();
      if (known.contains(binary) || !part.binaries.insert(binary).second) continue;

      auto imports = ReadDllImports(binary);
      if (!imports) return Error{imports.error()};
      for (const std::string& name : *imports) {
        const std::string lower = AsciiLower(name);
        if (IsApiSet(lower)) continue;
        const fs::path* local = find_dll(binary.parent_path(), lower);
        if (!local) local = find_dll(bin, lower);
        if (local) {
          pending.push_back(*local);
        } else {
          part.system_dlls.emplace(lower, name);
        }
      }
    }
    std::error_code ec;
    const auto add = [&](const fs::path& file) -> Status {
      const uint64_t size = fs::file_size(file, ec);
      if (ec) return Error{std::format("Couldn't read {}: {}", ToUtf8(file), Describe(ec))};
      part.files.push_back(file.lexically_relative(install_root));
      part.bytes += size;
      return Ok{};
    };
    for (const fs::path& binary : part.binaries) {
      if (auto added = add(binary); !added) return Error{added.error()};
      // OBS ships PDBs; keeping them next to the copies makes crashes debuggable.
      fs::path pdb = binary;
      pdb.replace_extension(L".pdb");
      if (fs::exists(pdb, ec)) {
        if (auto added = add(pdb); !added) return Error{added.error()};
      }
    }
    for (const fs::path& dir : data_dirs) {
      for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
           it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (optional && !KnobsReadsOptionalData(it->path())) continue;
        if (auto added = add(it->path()); !added) return Error{added.error()};
      }
      if (ec) return Error{std::format("Couldn't read {}: {}", ToUtf8(dir), Describe(ec))};
    }
    return part;
  };

  RuntimeFileSet file_set;
  std::set<fs::path> known;
  std::map<std::string, std::string> system_dlls;
  const auto take = [&](Part& part) {
    known.merge(part.binaries);
    file_set.files.insert(file_set.files.end(), part.files.begin(), part.files.end());
    file_set.total_bytes += part.bytes;
    system_dlls.merge(part.system_dlls);
  };

  std::vector<fs::path> roots = {ObsDll(install_root), bin / (fs::path(kGraphicsModule) += L".dll")};
  std::vector<fs::path> data_dirs = {LibobsDataDir(install_root)};
  for (std::string_view module : kObsModules) {
    roots.push_back(PluginDll(install_root, module, version));
    data_dirs.push_back(PluginDataDir(install_root, module, version));
  }
  auto base = plan_part(std::move(roots), data_dirs, known, false);
  if (!base) return Error{base.error()};
  take(*base);

  // An optional module that can't be read is left out, so a chain that
  // doesn't need it still runs. One with no data folder needs none.
  for (std::string_view module : optional) {
    const fs::path data = PluginDataDir(install_root, module, version);
    std::error_code data_ec;
    std::vector<fs::path> module_data;
    if (fs::is_directory(data, data_ec)) module_data.push_back(data);
    auto part = plan_part({PluginDll(install_root, module, version)}, module_data, known, true);
    if (!part) {
      file_set.skipped.push_back(std::format("{}: {}", module, part.error()));
      continue;
    }
    file_set.optional_files.emplace(module, part->files);
    take(*part);
  }

  std::sort(file_set.files.begin(), file_set.files.end());
  for (auto& [lower, name] : system_dlls) file_set.system_dlls.push_back(std::move(name));
  return file_set;
}

Result<RuntimeCopy> EnsureRuntimeCopy(const ObsInstall& install, const fs::path& runtime_base,
                                      bool force) {
  const std::string version = install.version.ToString();
  if (!IsSupportedObsVersion(install.version)) return Error{UnsupportedObsMessage(version)};

  const RuntimeCopyLock lock;
  const fs::path target = runtime_base / FromUtf8(version);
  if (!force) {
    if (auto intact = ReadIntactManifest(target, install)) {
      return RuntimeCopy{target, true, intact->file_count, intact->total_bytes, std::move(intact->skipped)};
    }
  }

  auto file_set = PlanRuntimeCopy(install.root, install.version);
  if (!file_set) return Error{file_set.error()};

  const fs::path staging = runtime_base / FromUtf8(version + ".partial");
  std::error_code ec;
  fs::remove_all(staging, ec);
  if (ec) return Error{std::format("Couldn't clear {}: {}", ToUtf8(staging), Describe(ec))};

  // An optional module's file that won't copy leaves that module out, as
  // one that can't be read does, so a chain that doesn't need it still runs.
  std::map<fs::path, std::string> module_of;
  for (const auto& [module, files] : file_set->optional_files) {
    for (const fs::path& file : files) module_of.emplace(file, module);
  }
  std::set<std::string, std::less<>> left_out;
  std::vector<std::string> skipped = file_set->skipped;
  for (const fs::path& file : file_set->files) {
    const auto module = module_of.find(file);
    if (module != module_of.end() && left_out.contains(module->second)) continue;
    const fs::path from = install.root / file;
    const fs::path to = staging / file;
    std::string error;
    fs::create_directories(to.parent_path(), ec);
    if (!ec) fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      error = std::format("Couldn't copy {} to {}: {}", ToUtf8(from), ToUtf8(to), Describe(ec));
    } else {
      ClearReadOnly(to);
      if (!FlushToDisk(to)) error = std::format("Couldn't write {}: {}", ToUtf8(to), DescribeWinError(GetLastError()));
    }
    if (error.empty()) continue;
    if (module == module_of.end()) return Error{error};
    left_out.insert(module->second);
    skipped.push_back(std::format("{}: {}", module->second, error));
    ec.clear();
  }
  if (!left_out.empty()) {
    std::erase_if(file_set->files, [&](const fs::path& file) {
      const auto module = module_of.find(file);
      if (module == module_of.end() || !left_out.contains(module->second)) return false;
      std::error_code size_ec;
      const uint64_t size = fs::file_size(install.root / file, size_ec);
      if (!size_ec) file_set->total_bytes -= std::min(size, file_set->total_bytes);
      std::error_code ignored;
      fs::remove(staging / file, ignored);
      return true;
    });
  }
  if (auto written = WriteManifest(staging, install, *file_set, left_out); !written) {
    return Error{written.error()};
  }

  // Move the old copy aside rather than deleting it: the rename fails as a
  // whole while a running knobs has it loaded, instead of half-deleting it.
  // Its loaded DLLs alone wouldn't stop the rename, but libobs works in the
  // copy's bin\64bit (ObsRuntime::Load), and that does.
  RemoveRetiredCopies(runtime_base, version);
  const bool replacing = fs::exists(target, ec);
  const fs::path retired = RetiredPath(runtime_base, version);
  if (replacing) {
    fs::rename(target, retired, ec);
    if (ec) {
      return Error{std::format(
          "Couldn't replace the OBS runtime copy in {}. If {} is running, close it and try again. ({})",
          ToUtf8(target), kDisplayName, Describe(ec))};
    }
  }
  // Antivirus and the search indexer briefly hold handles to freshly written
  // files, which makes renaming their folder fail for a moment.
  for (int attempt = 1; attempt <= 5; ++attempt) {
    fs::rename(staging, target, ec);
    if (!ec) break;
    Sleep(200 * attempt);
  }
  if (ec) {
    std::error_code ignored;
    if (replacing) fs::rename(retired, target, ignored);
    return Error{std::format("Couldn't move {} into place: {}", ToUtf8(staging), Describe(ec))};
  }
  RemoveRetiredCopies(runtime_base, version);
  return RuntimeCopy{target, false, file_set->files.size(), file_set->total_bytes, std::move(skipped)};
}

AddedDlls AddInstallDlls(const fs::path& install_root, const fs::path& copy_root, const std::vector<std::string>& names,
                         const std::vector<fs::path>& folders, const std::wstring& lock) {
  const fs::path install_bin = BinDir(install_root);
  const fs::path copy_bin = BinDir(copy_root);
  // Where the loader finds a DLL without the install: an API set, one
  // already loaded, or a file in a folder it searches, the copy's bin\64bit
  // included. As UnresolvedDllImports looks, but for side-by-side
  // assemblies, which the DLLs OBS ships don't use.
  std::vector<fs::path> search = folders;
  search.push_back(copy_bin);
  const auto found = [&search](const std::string& name) {
    if (IsApiSet(AsciiLower(name))) return true;
    const std::wstring wide = FromUtf8(name);
    if (GetModuleHandleW(wide.c_str())) return true;
    return std::any_of(search.begin(), search.end(), [&wide](const fs::path& folder) {
      std::error_code ec;
      return !folder.empty() && fs::is_regular_file(folder / wide, ec);
    });
  };

  // Each name's DLLs to copy: itself, and those it imports, directly or not,
  // that only the install has. None if one of them imports a DLL found
  // nowhere, which OBS couldn't load either: the name stays missing.
  std::vector<std::string> to_copy;
  std::set<std::string, std::less<>> planned;  // Lowercase.
  for (const std::string& name : names) {
    if (found(name)) continue;
    std::vector<std::string> closure;
    std::set<std::string, std::less<>> visited;  // Lowercase.
    std::vector<std::string> pending = {name};
    bool complete = true;
    while (complete && !pending.empty()) {
      const std::string dll = std::move(pending.back());
      pending.pop_back();
      if (!visited.insert(AsciiLower(dll)).second || (dll != name && found(dll))) continue;
      const fs::path from = install_bin / FromUtf8(dll);
      std::error_code ec;
      // Read, not loaded, as the copy is planned (PlanRuntimeCopy).
      auto imports = IsPlainFileName(dll) && fs::is_regular_file(from, ec)
                         ? ReadDllImports(from, false)
                         : Result<std::vector<std::string>>(Error{"not in the install"});
      complete = imports.ok();
      if (!complete) break;
      closure.push_back(dll);
      pending.insert(pending.end(), imports->begin(), imports->end());
    }
    if (!complete) continue;
    for (std::string& dll : closure) {
      if (planned.insert(AsciiLower(dll)).second) to_copy.push_back(std::move(dll));
    }
  }

  AddedDlls result;
  if (to_copy.empty()) return result;
  const RuntimeCopyLock held(true, lock);
  for (const std::string& dll : to_copy) {
    const fs::path from = install_bin / FromUtf8(dll);
    const fs::path to = copy_bin / FromUtf8(dll);
    std::error_code ec;
    if (fs::exists(to, ec)) continue;  // Another process added it meanwhile.
    if (const Status copied = CopyWhole(from, to); !copied) {
      result.errors.push_back(copied.error());
    } else {
      result.added.push_back({from, to});
    }
  }
  return result;
}

PrunedCopies PruneRuntimeCopies(const fs::path& runtime_base, const fs::path& keep, const PruneOptions& options) {
  PrunedCopies pruned;
  // Held by a process making a copy, which uses its staging folder, or about
  // to load one.
  const RuntimeCopyLock lock(false, options.lock);
  if (!lock.locked()) {
    pruned.busy = true;
    return pruned;
  }
  const std::string kept = AsciiLower(ToUtf8(keep.filename()));
  for (const CopyFolder& folder : ListCopyFolders(runtime_base)) {
    if (folder.kind == CopyFolder::kCopy && folder.version == kept) continue;
    if (Stopped(options.stop_requested)) {
      pruned.stopped = true;
      break;
    }
    uint64_t bytes = 0;
    if (auto left = RemoveCopyFolder(runtime_base, folder, options.stop_requested, bytes)) {
      pruned.left.push_back(std::move(*left));
    } else {
      pruned.removed.push_back(folder.path);
      pruned.removed_bytes += bytes;
    }
  }
  return pruned;
}

}  // namespace knobs::runtime
