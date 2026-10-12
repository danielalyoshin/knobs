// SPDX-License-Identifier: GPL-2.0-or-later
#include "update/github.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <format>
#include <functional>
#include <string>

#include "app_info.h"
#include "util/win_strings.h"

namespace knobs::update {
namespace {

// A slow network is fine, but not one that never answers.
constexpr int kResolveMs = 15'000;
constexpr int kConnectMs = 15'000;
constexpr int kSendMs = 15'000;
constexpr int kReceiveMs = 30'000;
constexpr DWORD kChunk = 64 * 1024;

// WinHTTP's messages are in winhttp.dll, not among the system's.
std::string DescribeHttpError(DWORD code) {
  if (code >= WINHTTP_ERROR_BASE && code <= WINHTTP_ERROR_LAST) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
        GetModuleHandleW(L"winhttp.dll"), code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text(buffer ? buffer : L"", length);
    LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' ')) text.pop_back();
    if (!text.empty()) return std::format("{} (WinHTTP error {})", ToUtf8(text), code);
  }
  return DescribeWinError(code);
}

Error Failed(std::string_view what, DWORD code, const Cancel& cancel) {
  if (cancel.stopped()) return Error{"The request was stopped."};
  return Error{std::format("Couldn't {}: {}", what, DescribeHttpError(code))};
}

// A WinHTTP handle that the Cancel can close.
class Handle {
 public:
  Handle(HINTERNET handle, Cancel& cancel) : handle_(handle), cancel_(cancel) {
    if (handle_ && !cancel_.Track(handle_)) {
      WinHttpCloseHandle(handle_);
      handle_ = nullptr;
      SetLastError(ERROR_WINHTTP_OPERATION_CANCELLED);
    }
  }
  ~Handle() {
    if (handle_ && cancel_.Untrack(handle_)) WinHttpCloseHandle(handle_);
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;

  HINTERNET get() const { return handle_; }
  explicit operator bool() const { return handle_ != nullptr; }

 private:
  HINTERNET handle_;
  Cancel& cancel_;
};

// Sends one request for `url` and waits for its response's headers, then
// lets `read` have it with its status code.
Status Exchange(const wchar_t* verb, std::string_view url, bool follow_redirects, Cancel& cancel,
                const std::function<Status(HINTERNET request, DWORD status)>& read) {
  const std::wstring wide = FromUtf8(url);
  URL_COMPONENTS parts = {sizeof(parts)};
  parts.dwHostNameLength = static_cast<DWORD>(-1);
  parts.dwUrlPathLength = static_cast<DWORD>(-1);
  parts.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
    return Error{std::format("{} isn't an HTTPS address.", url)};
  }
  const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
  const std::wstring path = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) +
                            std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength);

  // The system's proxy settings, as a browser has them.
  Handle session(WinHttpOpen(std::wstring(kDisplayNameW).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0),
                 cancel);
  if (!session) return Failed("start a connection", GetLastError(), cancel);
  WinHttpSetTimeouts(session.get(), kResolveMs, kConnectMs, kSendMs, kReceiveMs);
  Handle connect(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0), cancel);
  if (!connect) return Failed(std::format("connect to {}", ToUtf8(host)), GetLastError(), cancel);
  Handle request(WinHttpOpenRequest(connect.get(), verb, path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                    WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE),
                 cancel);
  if (!request) return Failed(std::format("connect to {}", ToUtf8(host)), GetLastError(), cancel);
  // Never from HTTPS to HTTP, when following.
  DWORD policy = follow_redirects ? WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP
                                  : WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
  WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
  if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request.get(), nullptr)) {
    return Failed(std::format("reach {}", ToUtf8(host)), GetLastError(), cancel);
  }
  DWORD status = 0;
  DWORD size = sizeof(status);
  if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
    return Failed(std::format("read {}'s answer", ToUtf8(host)), GetLastError(), cancel);
  }
  return read(request.get(), status);
}

}  // namespace

void Cancel::Stop() {
  std::lock_guard lock(mutex_);
  stopped_ = true;
  // The request first, then its connection and session.
  for (auto it = handles_.rbegin(); it != handles_.rend(); ++it) WinHttpCloseHandle(*it);
  handles_.clear();
}

bool Cancel::stopped() const {
  std::lock_guard lock(mutex_);
  return stopped_;
}

bool Cancel::Track(void* handle) {
  std::lock_guard lock(mutex_);
  if (stopped_) return false;
  handles_.push_back(handle);
  return true;
}

bool Cancel::Untrack(void* handle) {
  std::lock_guard lock(mutex_);
  const auto it = std::find(handles_.begin(), handles_.end(), handle);
  if (it == handles_.end()) return false;
  handles_.erase(it);
  return true;
}

Result<Version> FindLatestRelease(Cancel& cancel) {
  std::optional<Version> latest;
  const Status asked = Exchange(L"HEAD", kLatestReleaseUrl, false, cancel, [&](HINTERNET request, DWORD status) {
    if (status < 300 || status > 399) {
      return Status(Error{std::format("GitHub answered {} for the latest release, not where it is.", status)});
    }
    DWORD size = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &size,
                        WINHTTP_NO_HEADER_INDEX);
    std::wstring location(size / sizeof(wchar_t), L'\0');
    if (location.empty() || !WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                                 location.data(), &size, WINHTTP_NO_HEADER_INDEX)) {
      return Status(Error{"GitHub didn't say where the latest release is."});
    }
    location.resize(size / sizeof(wchar_t));
    latest = VersionFromReleaseUrl(ToUtf8(location));
    if (!latest) {
      return Status(Error{std::format("GitHub's latest release is at {}, which names no version.", ToUtf8(location))});
    }
    return Status(Ok{});
  });
  if (!asked) return Error{asked.error()};
  return *latest;
}

Status Download(std::string_view url, const std::filesystem::path& file, uint64_t max_bytes, Cancel& cancel) {
  return Exchange(L"GET", url, true, cancel, [&](HINTERNET request, DWORD status) {
    if (status != HTTP_STATUS_OK) return Status(Error{std::format("GitHub answered {} for {}.", status, url)});
    const HANDLE out = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                   nullptr);
    if (out == INVALID_HANDLE_VALUE) {
      return Status(Error{std::format("Couldn't create {}: {}", ToUtf8(file), DescribeWinError(GetLastError()))});
    }
    std::string buffer(kChunk, '\0');
    uint64_t total = 0;
    std::optional<Error> error;
    for (;;) {
      DWORD read = 0;
      if (!WinHttpReadData(request, buffer.data(), kChunk, &read)) {
        error = Failed(std::format("download {}", url), GetLastError(), cancel);
        break;
      }
      if (read == 0) break;
      total += read;
      if (total > max_bytes) {
        error = Error{std::format("{} is bigger than {} MB, which is too big for an installer.", url,
                                  max_bytes / (1024 * 1024))};
        break;
      }
      DWORD written = 0;
      if (!WriteFile(out, buffer.data(), read, &written, nullptr) || written != read) {
        error = Error{std::format("Couldn't write {}: {}", ToUtf8(file), DescribeWinError(GetLastError()))};
        break;
      }
    }
    CloseHandle(out);
    if (error) {
      DeleteFileW(file.c_str());
      return Status(std::move(*error));
    }
    return Status(Ok{});
  });
}

}  // namespace knobs::update
