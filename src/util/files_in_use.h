// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "util/result.h"

// Files the process has loaded, and hearing when another program goes to
// replace one (docs/design.md, Updates while knobs runs).
namespace knobs {

// Published independently of the core. A released hold may still leave a
// loaded DLL in use: Windows can report that to the installer instead of
// keeping its open waiting. The tray logs released files and warns the user.
struct FileHoldStatus {
  bool waiting = false;
  std::vector<std::filesystem::path> released;
  std::vector<std::string> errors;
  bool operator==(const FileHoldStatus&) const = default;
};
FileHoldStatus FileHolds();
// Posts `message` when FileHolds changes. Register before starting the core
// so the watchdog's thread is already running when plugins load.
void WatchFileHolds(uintptr_t window, unsigned message);
void StopWatchingFileHolds(uintptr_t window);
// Earliest successful handoff's deadline, in GetTickCount64 milliseconds,
// or zero. Shutdown leaves time for the replacement to release its copies.
uint64_t FileHandoffDeadline();

// A read-handle oplock on a file the process has loaded. Windows can't
// replace or delete a loaded DLL, and fails the program that tries, as an
// installer updating it does. With the oplock, Windows tells this process
// first and holds that program's open until the oplock goes, so the process
// can unload the file and let it through. The oplock's own handle shares only
// reading, so that a program opening the file to write, rename or delete it
// conflicts with it, which is what breaks the oplock.
class FileOplock {
 public:
  // Fails where the file system has no oplocks, and on a file another
  // program has open to write. A break is acknowledged after `limit` even
  // if the owner cannot poll it. `warning` controls the tray's notice.
  static Result<std::unique_ptr<FileOplock>> Take(
      const std::filesystem::path& file,
      std::chrono::milliseconds limit = std::chrono::seconds(10),
      std::chrono::milliseconds warning = std::chrono::seconds(5));
  // Lets go, and the program waiting, if any, goes on. Unload the file first,
  // or that program fails as it would have without the oplock.
  ~FileOplock();
  FileOplock(const FileOplock&) = delete;
  FileOplock& operator=(const FileOplock&) = delete;

  const std::filesystem::path& file() const { return file_; }
  // Poll on the owning thread. True for a writer, renamer or deleter. A
  // reader whose sharing conflicts releases just the watch; later polls
  // take it again once the reader closes, without unloading the DLL.
  bool wanted();
  // Borrowed native handle, for StartWithFileOplocks. Zero while a reader
  // has the file. Valid until the next poll or destruction.
  uintptr_t handle() const;
  // Before handing handle() to StartWithFileOplocks, put an unbroken oplock
  // into a break awaiting acknowledgment. Its request then completes, so
  // neither destruction nor thread/process exit can cancel the hold. The
  // replacement acknowledges it by closing the last duplicate.
  Status PrepareHandoff();

 private:
  struct Request;
  friend struct FileOplockWatchdog;

  explicit FileOplock(std::filesystem::path file);

  std::filesystem::path file_;
  std::shared_ptr<Request> request_;
  std::chrono::milliseconds limit_, warning_;
};

// Starts a replacement process with duplicates of the oplocks and a
// handle to this process. The child must call WaitForFileOplocks with the
// --wait-for-files argument this adds, before loading any plugins, and
// acknowledge that within one second and the remaining hold budget. Closing
// the old process's handles at exit happens before its DLLs are unmapped;
// the inherited duplicates keep the writer waiting through that interval.
// Call PrepareHandoff on every watch first; retain the watches until this
// succeeds, then destroy them without polling them again.
Status StartWithFileOplocks(const std::filesystem::path& exe, std::wstring_view arguments,
                           const std::vector<uintptr_t>& files);
// Waits for the old process to finish, then closes the inherited file handles
// to let its installer through. No audio or runtime has started in this copy.
// A handoff that doesn't check out, or an old process that hasn't ended
// before the original hold deadline (at most 30 s), fails, saying why, with
// the handles closed all the same.
Status WaitForFileOplocks(std::wstring_view handoff);

// The files of the modules loaded in this process, the program's own
// included, by their full paths. `complete` is false if enumeration or any
// path lookup fails; callers must not infer that omitted DLLs unloaded.
std::vector<std::filesystem::path> LoadedModules(const std::function<void(std::string_view)>& log = {},
                                                bool* complete = nullptr);

// Whether two paths name the same file as Windows compares them, ignoring
// case, trailing separators and the difference between / and \.
// An empty path matches nothing.
bool SamePath(const std::filesystem::path& a, const std::filesystem::path& b);
// Whether `file` is in `folder` or a folder below it, compared the same way.
bool InFolder(const std::filesystem::path& file, const std::filesystem::path& folder);

// A summary of the files in `folders` and the two levels of folders below
// them: their names, sizes and times. It changes when a file is added,
// removed, resized or rewritten, and says so when a folder is missing. Reads
// at most a few thousand files in each.
std::string FolderFingerprint(const std::vector<std::filesystem::path>& folders);

}  // namespace knobs
