// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

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

// A read-handle oplock on a file the process has loaded. Windows can't
// replace or delete a loaded DLL, and fails the program that tries, as an
// installer updating it does. With the oplock, Windows tells this process
// first and holds that program's open until the oplock goes, so the process
// can unload the file and let it through. The oplock's own handle shares only
// reading, so that a program opening the file to write, rename or delete it
// conflicts with it, which is what breaks the oplock.
class FileOplock {
 public:
  // Calls `wanted` once, on a thread-pool thread, when another program goes to
  // write, rename or delete `file`. Fails where the file system has no
  // oplocks, and on a file another program has open to write.
  static Result<std::unique_ptr<FileOplock>> Take(const std::filesystem::path& file, std::function<void()> wanted);
  // Lets go, and the program waiting, if any, goes on. Unload the file first,
  // or that program fails as it would have without the oplock.
  ~FileOplock();
  FileOplock(const FileOplock&) = delete;
  FileOplock& operator=(const FileOplock&) = delete;

  const std::filesystem::path& file() const { return file_; }
  // Whether a program has gone for the file.
  bool wanted() const;
  // Borrowed native handle, for StartWithFileOplocks. Valid until destruction.
  uintptr_t handle() const;

 private:
  struct Request;

  explicit FileOplock(std::filesystem::path file);

  std::filesystem::path file_;
  std::unique_ptr<Request> request_;
};

// Starts a replacement process with duplicates of the broken oplocks and a
// handle to this process. The child must call WaitForFileOplocks with the
// --wait-for-files argument this adds, before loading any plugins. Closing
// the old process's handles at exit happens before its DLLs are unmapped;
// the inherited duplicates keep the writer waiting through that interval.
Status StartWithFileOplocks(const std::filesystem::path& exe, std::wstring_view arguments,
                           const std::vector<uintptr_t>& files);
// Waits for the old process to finish, then closes the inherited file handles
// to let its installer through. No audio or runtime has started in this copy.
Status WaitForFileOplocks(std::wstring_view handoff);

// The files of the modules loaded in this process, the program's own
// included, by their full paths.
std::vector<std::filesystem::path> LoadedModules();

// Whether two paths name the same file as Windows compares them, ignoring
// case and the difference between / and \.
bool SamePath(const std::filesystem::path& a, const std::filesystem::path& b);
// Whether `file` is in `folder` or a folder below it, compared the same way.
bool InFolder(const std::filesystem::path& file, const std::filesystem::path& folder);

// A summary of the files in `folders` and the two levels of folders below
// them: their names, sizes and times. It changes when a file is added,
// removed, resized or rewritten, and says so when a folder is missing. Reads
// at most a few thousand files.
std::string FolderFingerprint(const std::vector<std::filesystem::path>& folders);

}  // namespace knobs
