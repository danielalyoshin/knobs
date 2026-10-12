// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#ifndef RC_INVOKED
#include <string_view>
#endif

// The display name: "knobs", all lowercase (it was "knOBS" until 2026-10-05).
// This is its only definition, in case it changes again (docs/design.md,
// Overview). It also names knobs's %AppData% and %LocalAppData% folders;
// Windows paths are case-insensitive, so a rename that only changes case keeps
// them.
#define KNOBS_DISPLAY_NAME "knobs"

// The resource compiler reads this file too, for the name.
#ifndef RC_INVOKED

namespace knobs {

inline constexpr std::string_view kDisplayName = KNOBS_DISPLAY_NAME;
inline constexpr std::wstring_view kDisplayNameW = L"" KNOBS_DISPLAY_NAME;

// The setup guide: the README's Setup section, which the first run links to.
inline constexpr std::string_view kSetupGuideUrl = "https://github.com/danielalyoshin/knobs#setup";

// knobs's releases on GitHub, where updates come from (src/update).
inline constexpr std::string_view kReleasesUrl = "https://github.com/danielalyoshin/knobs/releases";

// The latest release, which the first run links to when OBS is newer than
// this version supports. GitHub redirects it to the release's own page, which
// is how the update check finds the latest version.
inline constexpr std::string_view kLatestReleaseUrl = "https://github.com/danielalyoshin/knobs/releases/latest";

// The installer's AppId (packaging/knobs.iss), which names its entry under
// HKCU's Uninstall key: a copy in the folder that entry gives was installed by
// the installer, and can update itself with the next one.
inline constexpr std::wstring_view kInstallerAppId = L"{1FE50EC5-3715-4EE5-BD51-381BF3374BA2}";

}  // namespace knobs

#endif  // RC_INVOKED
