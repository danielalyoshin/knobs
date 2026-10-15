// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/pe_imports.h"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include "util/win_strings.h"

namespace knobs::runtime {
namespace {

struct HandleCloser {
  void operator()(HANDLE handle) const { CloseHandle(handle); }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

// A file read a few bytes at a time, where the parse needs them, rather than
// mapped: while a mapping exists, Windows refuses to truncate the file, as an
// installer that overwrites a plugin does (CREATE_ALWAYS), and an open handle
// that shares every kind of access doesn't stop that.
class FileReader {
 public:
  static Result<FileReader> Open(const std::filesystem::path& file) {
    const std::string name = ToUtf8(file);
    FileReader reader;
    HANDLE raw = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
      return Error{std::format("Couldn’t open {}: {}", name, DescribeWinError(GetLastError()))};
    }
    reader.file_.reset(raw);
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(reader.file_.get(), &size) || size.QuadPart == 0) {
      return Error{std::format("{} is empty or unreadable.", name)};
    }
    reader.size_ = static_cast<uint64_t>(size.QuadPart);
    return reader;
  }

  uint64_t size() const { return size_; }

  // `count` bytes at `offset`, kept until the reader goes. Null past the end,
  // or if they can't be read, as when another program truncated the file
  // meanwhile.
  const std::byte* Read(uint64_t offset, size_t count) {
    if (offset > size_ || size_ - offset < count || count > MAXDWORD) return nullptr;
    auto bytes = std::make_unique_for_overwrite<std::byte[]>(count);
    OVERLAPPED at = {};
    at.Offset = static_cast<DWORD>(offset);
    at.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD read = 0;
    if (!ReadFile(file_.get(), bytes.get(), static_cast<DWORD>(count), &read, &at) || read != count) return nullptr;
    reads_.push_back(std::move(bytes));
    return reads_.back().get();
  }

 private:
  UniqueHandle file_;
  uint64_t size_ = 0;
  std::vector<std::unique_ptr<std::byte[]>> reads_;
};

// Bounds-checked view of a PE file's bytes.
class PeFile {
 public:
  explicit PeFile(FileReader& file) : file_(file) {}

  // Reads the headers. Returns false if this isn't a 64-bit PE file.
  bool Parse() {
    const auto* dos = At<IMAGE_DOS_HEADER>(0);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) return false;
    nt_ = At<IMAGE_NT_HEADERS64>(static_cast<uint64_t>(dos->e_lfanew));
    if (!nt_ || nt_->Signature != IMAGE_NT_SIGNATURE ||
        nt_->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
      return false;
    }
    const uint64_t sections_offset = static_cast<uint64_t>(dos->e_lfanew) +
                                     offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
                                     nt_->FileHeader.SizeOfOptionalHeader;
    const size_t count = nt_->FileHeader.NumberOfSections;
    const std::byte* sections = file_.Read(sections_offset, count * sizeof(IMAGE_SECTION_HEADER));
    if (!sections) return false;
    sections_ = {reinterpret_cast<const IMAGE_SECTION_HEADER*>(sections), count};
    return true;
  }

  const IMAGE_DATA_DIRECTORY* Directory(unsigned index) const {
    const auto& optional = nt_->OptionalHeader;
    if (index >= optional.NumberOfRvaAndSizes) return nullptr;
    const IMAGE_DATA_DIRECTORY& dir = optional.DataDirectory[index];
    return dir.VirtualAddress != 0 && dir.Size != 0 ? &dir : nullptr;
  }

  template <typename T>
  const T* AtRva(uint32_t rva) const {
    const auto offset = RvaToOffset(rva);
    return offset ? At<T>(*offset) : nullptr;
  }

  std::optional<std::string_view> StringAtRva(uint32_t rva) const {
    const auto offset = RvaToOffset(rva);
    if (!offset || *offset >= file_.size()) return std::nullopt;
    const size_t limit = static_cast<size_t>(std::min<uint64_t>(file_.size() - *offset, MAX_PATH));
    const auto* begin = reinterpret_cast<const char*>(file_.Read(*offset, limit));
    if (!begin) return std::nullopt;
    const size_t length = strnlen(begin, limit);
    if (length == 0 || length == limit) return std::nullopt;
    return std::string_view(begin, length);
  }

 private:
  template <typename T>
  const T* At(uint64_t offset) const {
    return reinterpret_cast<const T*>(file_.Read(offset, sizeof(T)));
  }

  std::optional<uint64_t> RvaToOffset(uint32_t rva) const {
    for (const IMAGE_SECTION_HEADER& section : sections_) {
      const uint32_t start = section.VirtualAddress;
      const uint32_t extent = std::max(section.Misc.VirtualSize, section.SizeOfRawData);
      if (rva >= start && rva - start < extent) {
        const uint32_t delta = rva - start;
        if (delta >= section.SizeOfRawData) return std::nullopt;  // Zero-fill, not in the file.
        return uint64_t{section.PointerToRawData} + delta;
      }
    }
    if (rva < nt_->OptionalHeader.SizeOfHeaders) return rva;
    return std::nullopt;
  }

  FileReader& file_;
  const IMAGE_NT_HEADERS64* nt_ = nullptr;
  std::span<const IMAGE_SECTION_HEADER> sections_;
};

// Descriptor tables end with an all-zero entry; the cap guards against
// malformed files that don't.
constexpr size_t kMaxDescriptors = 4096;

template <typename Descriptor, typename NameRva>
bool ReadDescriptorNames(const PeFile& pe, unsigned directory, NameRva name_rva,
                         std::vector<std::string>& names) {
  const IMAGE_DATA_DIRECTORY* dir = pe.Directory(directory);
  if (!dir) return true;
  for (size_t i = 0; i < kMaxDescriptors; ++i) {
    const auto* descriptor =
        pe.AtRva<Descriptor>(dir->VirtualAddress + static_cast<uint32_t>(i * sizeof(Descriptor)));
    if (!descriptor) return false;
    const uint32_t rva = name_rva(*descriptor);
    if (rva == 0) return true;
    const auto name = pe.StringAtRva(rva);
    if (!name) return false;
    names.emplace_back(*name);
  }
  return false;
}

}  // namespace

Result<bool> IsWindows64Binary(const std::filesystem::path& file) {
  auto reader = FileReader::Open(file);
  if (!reader) return Error{reader.error()};
  return PeFile(*reader).Parse();
}

Result<std::vector<std::string>> ReadDllImports(const std::filesystem::path& file, bool delay_loaded) {
  const std::string name = ToUtf8(file);
  auto reader = FileReader::Open(file);
  if (!reader) return Error{reader.error()};
  PeFile pe(*reader);
  if (!pe.Parse()) return Error{std::format("{} isn’t a 64-bit Windows binary.", name)};

  std::vector<std::string> names;
  const bool ok =
      ReadDescriptorNames<IMAGE_IMPORT_DESCRIPTOR>(
          pe, IMAGE_DIRECTORY_ENTRY_IMPORT,
          [](const IMAGE_IMPORT_DESCRIPTOR& d) { return d.Name; }, names) &&
      (!delay_loaded || ReadDescriptorNames<IMAGE_DELAYLOAD_DESCRIPTOR>(
                            pe, IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT,
                            [](const IMAGE_DELAYLOAD_DESCRIPTOR& d) { return d.DllNameRVA; }, names));
  if (!ok) return Error{std::format("{} has a malformed import table.", name)};
  return names;
}

Result<std::vector<std::string>> UnresolvedDllImports(const std::filesystem::path& file,
                                                      const std::vector<std::filesystem::path>& folders) {
  auto imports = ReadDllImports(file, false);
  if (!imports) return Error{imports.error()};
  return UnresolvedDllImports(file, *imports, folders);
}

std::vector<std::string> UnresolvedDllImports(const std::filesystem::path& file,
                                              const std::vector<std::string>& imports,
                                              const std::vector<std::filesystem::path>& folders) {
  std::vector<std::string> unresolved;
  for (const std::string& name : imports) {
    if (IsApiSet(AsciiLower(name))) continue;
    const std::wstring wide = FromUtf8(name);
    if (GetModuleHandleW(wide.c_str())) continue;
    const bool found = std::any_of(folders.begin(), folders.end(), [&](const std::filesystem::path& folder) {
      std::error_code ec;
      return !folder.empty() && std::filesystem::is_regular_file(folder / wide, ec);
    });
    if (!found) unresolved.push_back(name);
  }
  if (unresolved.empty()) return unresolved;
  // The loader also finds a DLL's imports through the side-by-side
  // assemblies its own manifest names, as a DLL built with Visual C++ 2005 or
  // 2008 finds its runtime in WinSxS. Reading the manifest runs nothing.
  ACTCTXW request = {sizeof(request)};
  request.dwFlags = ACTCTX_FLAG_RESOURCE_NAME_VALID;
  request.lpSource = file.c_str();
  request.lpResourceName = MAKEINTRESOURCEW(2);  // ISOLATIONAWARE_MANIFEST_RESOURCE_ID
  const HANDLE context = CreateActCtxW(&request);
  if (context == INVALID_HANDLE_VALUE) return unresolved;
  ULONG_PTR cookie = 0;
  if (ActivateActCtx(context, &cookie)) {
    std::erase_if(unresolved, [](const std::string& name) {
      ACTCTX_SECTION_KEYED_DATA data = {sizeof(data)};
      return FindActCtxSectionStringW(0, nullptr, ACTIVATION_CONTEXT_SECTION_DLL_REDIRECTION, FromUtf8(name).c_str(),
                                      &data) != FALSE;
    });
    DeactivateActCtx(0, cookie);
  }
  ReleaseActCtx(context);
  return unresolved;
}

}  // namespace knobs::runtime
