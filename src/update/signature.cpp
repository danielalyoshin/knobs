// SPDX-License-Identifier: GPL-2.0-or-later
#include "update/signature.h"

#include <windows.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app_info.h"
#include "util/win_strings.h"

namespace knobs::update {
namespace {

// How knobs's releases are signed, read from the v0.1.1 and v0.2.0
// installers, whose certificates differ: the signer's common name, the
// extended key usage Azure Artifact Signing puts on every certificate of
// Daniel Alyoshin's identity validation, and the SHA-1 thumbprint of the root
// the chain ends at, Microsoft Identity Verification Root Certificate
// Authority 2020. A new identity validation would get a new usage, and
// releases signed with it would need a knobs that knows it.
constexpr std::wstring_view kSignerName = L"Daniel Alyoshin";
constexpr std::string_view kSignerUsage = "1.3.6.1.4.1.311.97.240045311.89291031.361016744.918694411";
constexpr BYTE kRootThumbprint[20] = {0xF4, 0x00, 0x42, 0xE2, 0xE5, 0xF7, 0xE8, 0xEF, 0x81, 0x89,
                                      0xFE, 0xD1, 0x55, 0x19, 0xAE, 0xCE, 0x42, 0xC3, 0xBF, 0xA2};

std::string DescribeTrust(LONG status) {
  switch (static_cast<HRESULT>(status)) {
    case TRUST_E_NOSIGNATURE:
    case TRUST_E_SUBJECT_FORM_UNKNOWN:
    case TRUST_E_PROVIDER_UNKNOWN:
      return "it isn’t signed";
    case TRUST_E_BAD_DIGEST:
      return "it was changed after it was signed";
    case CERT_E_REVOKED:
      return "its certificate was revoked";
    case CERT_E_REVOCATION_FAILURE:
    case CRYPT_E_REVOCATION_OFFLINE:
      return "Windows couldn’t check whether its certificate was revoked";
    case HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION):
    case HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION):
      return "another program is using it";
    case CERT_E_UNTRUSTEDROOT:
    case CERT_E_CHAINING:
      return "its certificate doesn’t come from an authority Windows trusts";
    case CERT_E_EXPIRED:
      return "its certificate expired, and it has no time stamp from when it was signed";
    default:
      return std::format("Windows doesn’t trust its signature: {}", DescribeWinError(static_cast<unsigned long>(status)));
  }
}

// Whether the check may pass later: Windows couldn't reach the servers that
// say whether a certificate was revoked, or another program, such as an
// antivirus scanning it, has the file open.
bool MayPassLater(LONG status) {
  switch (static_cast<HRESULT>(status)) {
    case CERT_E_REVOCATION_FAILURE:
    case CRYPT_E_REVOCATION_OFFLINE:
    case HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION):
    case HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION):
      return true;
    default:
      return false;
  }
}

bool HasUsage(PCCERT_CONTEXT certificate, std::string_view usage) {
  DWORD size = 0;
  if (!CertGetEnhancedKeyUsage(certificate, CERT_FIND_EXT_ONLY_ENHKEY_USAGE_FLAG, nullptr, &size) || size == 0) {
    return false;
  }
  std::vector<BYTE> buffer(size);
  auto* usages = reinterpret_cast<CERT_ENHKEY_USAGE*>(buffer.data());
  if (!CertGetEnhancedKeyUsage(certificate, CERT_FIND_EXT_ONLY_ENHKEY_USAGE_FLAG, usages, &size)) return false;
  for (DWORD i = 0; i < usages->cUsageIdentifier; ++i) {
    if (usages->rgpszUsageIdentifier[i] && usage == usages->rgpszUsageIdentifier[i]) return true;
  }
  return false;
}

std::wstring CommonName(PCCERT_CONTEXT certificate) {
  wchar_t name[256] = {};
  CertGetNameStringW(certificate, CERT_NAME_ATTR_TYPE, 0, const_cast<char*>(szOID_COMMON_NAME), name,
                     static_cast<DWORD>(std::size(name)));
  return name;
}

bool IsRoot(PCCERT_CONTEXT certificate) {
  BYTE thumbprint[20] = {};
  DWORD size = sizeof(thumbprint);
  return CertGetCertificateContextProperty(certificate, CERT_SHA1_HASH_PROP_ID, thumbprint, &size) &&
         size == sizeof(thumbprint) && std::memcmp(thumbprint, kRootThumbprint, sizeof(thumbprint)) == 0;
}

}  // namespace

SignatureCheck CheckReleaseSignature(const std::filesystem::path& file, Revocation revocation) {
  WINTRUST_FILE_INFO file_info = {sizeof(file_info)};
  file_info.pcwszFilePath = file.c_str();
  WINTRUST_DATA data = {sizeof(data)};
  data.dwUIChoice = WTD_UI_NONE;
  if (revocation == Revocation::kOnline) {
    // As Windows checks a download before running it: the whole chain,
    // revoked certificates included.
    data.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    data.dwProvFlags = WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;
  } else {
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
  }
  data.dwUnionChoice = WTD_CHOICE_FILE;
  data.pFile = &file_info;
  data.dwStateAction = WTD_STATEACTION_VERIFY;
  GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  const HWND no_ui = static_cast<HWND>(INVALID_HANDLE_VALUE);
  const LONG trust = WinVerifyTrust(no_ui, &action, &data);
  using Verdict = SignatureCheck::Verdict;
  SignatureCheck checked{.verdict = Verdict::kRelease};
  const std::string name = ToUtf8(file.filename());
  const auto reject = [&checked](std::string error) {
    checked = {.verdict = Verdict::kRejected, .error = std::move(error)};
  };
  if (trust != ERROR_SUCCESS) {
    checked = {.verdict = MayPassLater(trust) ? Verdict::kUnknown : Verdict::kRejected,
               .error = std::format("{} can’t be installed, because {}.", name, DescribeTrust(trust))};
  } else {
    CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(data.hWVTStateData);
    CRYPT_PROVIDER_SGNR* signer = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
    if (!signer || signer->csCertChain == 0 || !signer->pasCertChain) {
      reject(std::format("{} can’t be installed, because its signer couldn’t be read.", name));
    } else {
      PCCERT_CONTEXT leaf = signer->pasCertChain[0].pCert;
      PCCERT_CONTEXT root = signer->pasCertChain[signer->csCertChain - 1].pCert;
      if (CommonName(leaf) != kSignerName) {
        reject(std::format("{} can’t be installed, because it was signed by {}, not {}.", name,
                           ToUtf8(CommonName(leaf)), ToUtf8(kSignerName)));
      } else if (!HasUsage(leaf, kSignerUsage) || !IsRoot(root)) {
        // The certificate profile's usage and root (kSignerUsage, IsRoot).
        reject(std::format("{} can’t be installed, because it isn’t signed the way {}’s releases are.", name,
                           kDisplayName));
      }
    }
  }
  data.dwStateAction = WTD_STATEACTION_CLOSE;
  WinVerifyTrust(no_ui, &action, &data);
  return checked;
}

}  // namespace knobs::update
