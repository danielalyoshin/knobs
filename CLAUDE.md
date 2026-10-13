# knobs

Windows tray app that runs the user's OBS mic filter chain through their installed libobs and sends the result to a virtual audio cable, with no OBS process. The design and its rationale, with the evidence behind it, are in [docs/design.md](docs/design.md). Read it before non-trivial work, and update it when the design changes. It describes what's true now, in the present tense, with no dates, milestones or decision history: git history has how it got there.

Status: knobs works end to end against OBS 32.2.2. Its output is bit-identical to OBS's, on test signals and real voice, its mic-to-cable latency matches OBS's, and knobs.exe met its success criteria (PRODUCT.md) on real hardware (docs/design.md, Evidence). It also supports OBS 33.0, from knobs 0.2.0, tested against 33.0.0-rc1, where its output is bit-identical to OBS 33's too (docs/design.md, OBS 33.0). The core restarts libobs's monitor in silence so the delay to the cable can't creep up (docs/design.md, Keeping the delay down). knobs 0.2.0 is released, as an installer and a portable zip, signed with Azure Artifact Signing (`release.yml`). Open work is tracked in GitHub issues: OBS 33.0 support (#1, open until 33.0.0 is out and checked), VST filters (#3) and NVIDIA noise removal (#4). The setup window of knobs's own (#6), the opt-in update check with updating by itself (#2) and installing VB-Cable from the installer (#5) are on `main` for the next release, and their issues close with it. An hours-long latency test on real hardware is optional.

## Stack
- C++20, CMake, MSVC (VS 2022 or newer; the preset uses the newest installed Visual Studio), x64 only, static CRT.
- Plain Win32 for UI. No Qt or other UI frameworks.
- Windows-only in v1. Keep OS-specific code behind thin seams so a port stays possible.

## Build & test
Run these from a Developer PowerShell for VS, which puts `cmake` and `ctest` on PATH:

```powershell
cmake --preset x64            # configure
cmake --build --preset debug  # or: release
ctest --preset debug          # unit tests, then smoke, harness, import and core tests (these skip if OBS isn't installed)
```

To run the smoke, harness, import and core tests against another OBS, such as an unzipped release, configure with `-DKNOBS_TEST_OBS_DIR=<folder>`. CMake keeps it for later configures, so go back to the installed OBS with `-DKNOBS_TEST_OBS_DIR=` (empty). CI tests the newest tested OBS installed and the oldest supported one unzipped (`build.yml`).

The build puts the tray app, `knobs.exe`, in `build\x64\<config>\`. It runs the real core: unless OBS is open or something needs setting up, it opens the mic. Use `knobs-tray` to look at the tray and the first run instead.

`packaging\package.ps1` packages the Release build as an installer (Inno Setup 6) and a portable zip in `build\package`. The installer carries VB-Audio's VB-Cable driver pack, which the script downloads into `build\vb-cable` the first time and checks against a fixed SHA-256 (`-VbCablePack` takes a copy instead). Don't test an uninstall of the real installer on this PC: it deletes `%LocalAppData%\knobs`. Build one with another `AppName` instead, such as `/DAppName=knobs-test` to Inno Setup's compiler. Without `package.ps1`'s `/DKnobsAppId`, it gets an AppId of its own, so it can't install over knobs or uninstall it.

The dev tools in `build\x64\<config>\` each take `--help`:
- `knobs-smoke`: the runtime bootstrap check. Options include `--video dummy`, `--list-files`, `--obs-dir`, and opt-in mic capture (`--capture-seconds`).
- `knobs-import`: imports the mic from OBS's active profile and scene collection and reports each step: settings found, mics, pre-flight warnings, the chain libobs loads. `--obs-config` points it at another OBS settings folder, `--pick` chooses among several mics, `--save` writes the source object. It opens no audio devices.
- `knobs-harness`: pushes a WAV (or a built-in test signal) through a filter chain offline and checks that runs are bit-identical. `--import` uses the imported mic's chain; `--source <json>` takes an OBS source object; `--out` writes the result. It opens no audio devices.
- `knobs-compare`: runs the same input through the imported chain in knobs and in OBS itself, and measures the difference. OBS runs from a portable copy of the install in `%LocalAppData%\knobs\compare`, minimized to the tray, with settings of its own. It opens no audio device and leaves `%AppData%\obs-studio` alone. `--obs-wav` compares with an existing OBS recording instead.
- `knobs-core`: the always-on core with no tray. It prints each change of state, and takes keys to pause, resume and re-import. By default it opens no audio device: the chain loads as a push source and isn't monitored (`--stall` fakes a stalled mic). `--live` opens the mic and the cable. `--ignore-obs` stops it pausing for OBS. `--restart-monitor` shortens the time between the monitor's restarts in silence. `--prune-runtime` prunes the real runtime folder, as knobs.exe does.
- `knobs-tray`: the tray app on a fake core: the real core over a made-up OBS and devices, and a made-up GitHub for the update check. `--state` picks the state (every one the menu can show), `--theme light|dark` the menu's theme, and `--screenshot` saves a picture of the menu (`--open` opens a submenu or a dialog for it). `--first-run <page>` opens any page of the first run on those fake findings (`--walk-at` holds a guide's walkthrough at a moment), `--obs-config tests\fixtures\obs-config` imports a real OBS settings folder with the installed OBS's libobs instead, and `--press` scripts keys that open and close OBS, here or in another Windows account, plug the mics and cables in and out, stall the chain, change the chain or add filters in OBS, update OBS, and click the notification and the first run's buttons. `--open notification` takes a picture of the notification the keys bring up, `--restarted-for` starts as the copy a restart starts (`--paused` as one a paused copy started), `--windows10` words Windows' Sound settings as Windows 10 does, and `--icons` saves the icon with each badge. `--updates check|install` turns the update check on, `--latest` sets the made-up GitHub's latest release, `--signature` and `--recheck` how its installers' signatures check out, when downloaded and just before they run, `--downloaded` starts with an update downloaded before, `--remind-after` shortens the day an update waits before a notification offers it, `--portable` runs as a copy that can't update itself, and `--updated` starts as the copy an update started. It opens no audio device, makes no network connection, never starts OBS, and reads nothing of OBS's but the install and a settings folder it's given.
- `knobs-update`: the update check's parts against the real GitHub, one at a time: `--latest` asks for the latest release, `--download <version>` downloads its installer and checks its signature, and `--verify <file>` checks a file's. It installs nothing. It's the only tool that uses the network.
- `knobs-live`: the live path into a virtual cable. `--import` runs the imported mic, monitored to the profile's monitoring device unless that's `default` (then `--output` is required). `--list-devices`, `--measure-output`, `--measure-cable`, `--measure-restart` and `--measure-drift` don't use the mic, unless `--measure-drift` is given `--mic`. `--measure-output`, `--measure-cable` and `--measure-restart` play test clicks into VB-Cable and refuse to run while another app is using the cable; `--measure-restart` also restarts libobs's monitor on a schedule and measures the gaps it leaves, and `--push-ppm` runs the push source fast or slow, as a mic whose clock drifts. `--measure-drift` times a playback device's clock against QueryPerformanceCounter, playing silence, or with `--mic` a recording device's, dropping what it records unread. `--run`, `--measure-mic` and `--measure-drift --mic` open the mic.

Don't open the mic without the user's go-ahead.

## Layout
- `src/runtime/`: finding OBS, the runtime copy and its pruning, loading `obs.dll` and its function table, the libobs session, logging, and `ObsHost`, which runs all of those in order.
- `src/import/`: reading OBS's settings (an INI reader that matches libobs's parser, the active profile and scene collection), finding the mics in a collection, and the pre-flight checks.
- `src/audio/`: the live path. Device lists (libobs's, and Windows' own with `DeviceWatch` for devices coming and going), and `LiveChain`, which loads a source through OBS's loader and monitors it.
- `src/core/`: the always-on core the tray runs. `Controller` makes the decisions with no threads or clock of its own, on a `Backend` (`ObsBackend` is the real one). `Core` runs it on its own thread with libobs, and `ObsWatch` tells whether OBS is running. The tray hears state through `Observer`.
- `src/update/`: the update check's requests to GitHub over WinHTTP (`github.h`), its releases' versions and addresses (`release.h`), and the check of an installer's signature (`signature.h`).
- `src/tray/`: the tray app. `TrayApp` is the icon and the menu; `BuildMenu` turns a snapshot into the menu as data, which the unit tests check for every state. `FirstRun` is the first run as data in the same way (pages, the band's stages, buttons, what each click does), and `SetupWindow` shows it in a window of its own, drawn with Direct2D and DirectWrite: `setup_scene` lays out and paints a page, `setup_uia` is its UI Automation, `walkthrough` is a guide's walkthrough as data, and `pictures/` has its screenshots, with how they're taken. `Notifier` (`notices.h`) is the notifications as data too, with the icon's badge, which `badge.h` draws. `Updater` (`updates.h`) is the update check's plan: when to check, download and install, with no threads or clock of its own. Also dark menus, the settings file, the Run entry, the single-instance lock and starting OBS. `src/main.cpp` is `knobs.exe`'s entry point.
- `src/util/`: `Result`, UTF-8, JSON, text file and Win32 helpers, knobs's app folders.
- `tools/common/`: code shared by the dev tools (console output, WAV files, the push source and offline runs, the test signal, the tools' start-up and import steps, aligning and diffing audio, energy envelopes and peaks, a seeded RNG). It isn't part of the app.
- `tools/smoke/`, `tools/import/`, `tools/harness/`, `tools/compare/`, `tools/live/`, `tools/core/`, `tools/tray/`, `tools/update/`: the dev tools above. `tools/tray/` also has the fake backend and the screenshots. `tools/vendor-libobs-headers.ps1` refreshes `third_party/libobs`, `tools/render-icon.ps1` renders `assets/knobs.ico`, and `tools/render-installer-images.ps1` the installer's images in `assets/installer/`.
- `tests/`: unit tests, which don't need OBS (`core_tests.cpp` runs the core on a fake backend, `tray_tests.cpp` checks the menu, the settings file, the Run and RunOnce entries on a scratch key, the lock, and starting OBS with a stand-in, `first_run_tests.cpp` the first run's pages and moves, `notices_tests.cpp` the notifications and badges on knobs-tray's fake core, and `update_tests.cpp` the update check's plan, its releases' addresses and the signature check, offline), and the import and core tests, which run `knobs-import` and `knobs-core` on the made-up OBS settings in `tests/fixtures/obs-config`.
- `packaging/`: the installer's Inno Setup script (with VB-Cable's task and the silent update knobs runs), the text files that ship in each package, `package.ps1`, which builds both packages, and `sign.ps1`, which signs with Azure Artifact Signing (set up in the `release` environment that `release.yml` uses).
- `.github/workflows/`: CI (`ci.yml`, which calls `build.yml`: build, every test against an installed OBS, packages) and `release.yml`, which drafts a release from a `v<version>` tag.
- `docs/design.md`: how knobs is designed and why, with the evidence behind it.
- `third_party/libobs/`: vendored libobs headers (declarations only). Don't edit them.
- `assets/`: the logo. The SVGs are the masters, and `knobs.ico` is the app icon, built into `knobs.exe`: the knob alone, with no tile. `assets/README.md` has the colors and usage rules.
- `PRODUCT.md`: who knobs is for, its voice and brand commitments. Read it before UI or copy work.

## Invariants
- **Fidelity is the product.** Audio passes only through OBS's own filter code. No custom DSP and no "improvements".
- **`%AppData%\obs-studio` is read-only**, as is a portable OBS's `config` folder. knobs keeps its own state in `%AppData%\knobs` and `%LocalAppData%\knobs`, or in the `data` folder of a portable copy (`AppDirs`); either way it reads OBS's settings from `%AppData%`. Some libobs helpers write: `obs_data_create_from_json_file_safe` renames a backup over a broken file, so read files yourself and parse them with `obs_data_create_from_json`.
- **Never load from the OBS install dir.** Load only from the shadow copy in `%LocalAppData%\knobs\runtime\<obs-version>\` (`AppDirs::RuntimeBase`). Hold `RuntimeCopyLock` from `EnsureRuntimeCopy` until `obs.dll` has loaded from the copy (`ObsHost` does). Only knobs.exe prunes the runtime folder; tests and tools don't, apart from `knobs-core --prune-runtime`.
- **No link-time libobs dependency.** Resolve exports into the function table (`KNOBS_OBS_API` in `src/runtime/obs_api.h`) with `GetProcAddress`. Add new libobs functions there. A missing export fails gracefully with a clear message.
- **Load only `win-wasapi` and `obs-filters`.** No `obs-vst` in v1.
- **No network until the user asks.** The update check is off until it's turned on, and is knobs's only connection. Updating by itself runs only an installer `update::CheckReleaseSignature` accepts, checked again just before it runs.
- **Never ship libobs.** A from-source build is only for local debugging.
- Import uses `obs_load_source()`, in its private form `obs_load_private_source()`. Manual replay is the fallback, not the default.
- Check OBS behavior (config keys, scene JSON, libobs semantics) against the obs-studio source at the tag matching the installed version. Don't rely on memory.
- When the tested OBS versions change, re-vendor the headers at the newest one's tag, revisit the supported range in `src/runtime/obs_version.h`, and update the versions in `build.yml`. An export older supported versions lack goes in `KNOBS_OBS_API_OPTIONAL`, with a fallback.

## Conventions
- Every source file starts with `// SPDX-License-Identifier: GPL-2.0-or-later` (`;` or `#` comments in scripts).
- The logo files in `assets/` aren't GPL: they're all rights reserved, apart from what `TRADEMARKS.md` allows. Don't add SPDX lines to them or move them out of `assets/`.
- The name is "knobs", all lowercase, even at the start of a sentence. It was "knOBS" until 2026-10-05. Keep it in one constant (`KNOBS_DISPLAY_NAME` in `src/app_info.h`), and don't write it out in user-facing strings; tests shouldn't depend on it either.

## Git
- Commit straight to `main`.
- Conventional Commits: `type(scope): subject`. Types: `feat`, `fix`, `refactor`, `perf`, `test`, `docs`, `build`, `chore`. Optional scopes: `runtime`, `import`, `audio`, `harness`, `tray`.
- Imperative, lowercase subject, no trailing period, 72 chars max. Add a body only when the why isn't obvious.
