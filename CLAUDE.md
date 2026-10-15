# knobs

Windows tray app that runs the user's OBS mic filter chain through their installed libobs and sends the result to a virtual cable, with no OBS process. The design and its rationale, with the evidence behind it, are in [docs/design.md](docs/design.md). Read it before non-trivial work, and update it when the design changes. It describes what's true now, in the present tense, with no dates, milestones or decision history: git history has how it got there.

Status: knobs 1.0.0 is released, as an installer and a portable zip signed through `release.yml`. It supports OBS 32.2 to 33.x (`src/runtime/obs_version.h`), tested on 32.2.2 and 33.0.0-rc1 (`build.yml`). Its output is bit-identical to OBS's, its latency matches and it met its success criteria on real hardware with a four-filter chain (docs/design.md, Evidence). It also runs VST filters and NVIDIA's noise removal (docs/design.md, Optional modules), checked against OBS but not yet run live on a mic. Open work is in GitHub issues: OBS 33.0.0 final (#1).

## Stack
- C++20, CMake, MSVC (VS 2022 or newer; the preset uses the newest installed Visual Studio), x64 only, static CRT.
- Plain Win32 for UI. No Qt or other UI frameworks.
- Windows only for now. Keep OS-specific code behind thin seams so a port stays possible.

## Build & test
Run these from a Developer PowerShell for VS, which puts `cmake` and `ctest` on PATH:

```powershell
cmake --preset x64            # configure
cmake --build --preset debug  # or: release
ctest --preset debug          # unit and brand assets tests, then smoke, harness, import, core and vst tests (these skip if OBS isn't installed)
```

`brand/` is a git submodule (knobs-brand): clone with `--recurse-submodules`, or run `git submodule update --init`. Configuring stops without it.

To run the smoke, harness, import, core and vst tests against another OBS, such as an unzipped release, configure with `-DKNOBS_TEST_OBS_DIR=<folder>`. CMake keeps it for later configures, so go back to the installed OBS with `-DKNOBS_TEST_OBS_DIR=` (empty). CI tests the newest tested OBS installed and the oldest tested one unzipped (`build.yml`).

Tests open no audio devices and show no real tray icons or notifications. A test that creates `TrayApp` replaces `TrayOptions::notify_icon` with a recorder, including for icon removal. The recorder must be safe to call from the shutdown watchdog's worker too.

The build puts the tray app, `knobs.exe`, in `build\x64\<config>\`. It runs the real core: unless OBS is open or something needs setting up, it opens the mic. Use `knobs-tray` to look at the tray and the first run instead.

`packaging\package.ps1` packages the Release build as an installer (Inno Setup 6) and a portable zip in `build\package` (`-VbCablePack` takes a copy of VB-Audio's driver pack instead of downloading it). Don't test an uninstall of the real installer on this PC: it deletes `%LocalAppData%\knobs`. Build one with another `AppName` instead: `/DAppName=knobs-test` to Inno Setup's compiler, with the other defines `package.ps1` passes and a `knobs-test.exe` in the stage. Without `package.ps1`'s `/DKnobsAppId`, it gets an AppId of its own, so it can't install over knobs or uninstall it.

The dev tools in `build\x64\<config>\` each take `--help`, which lists their options:
- `knobs-smoke`: the runtime bootstrap check. `--capture-seconds` opens the mic.
- `knobs-import`: imports the mic from OBS's active profile and scene collection and reports each step. `--obs-config` points it at another OBS settings folder. It opens no audio devices.
- `knobs-harness`: pushes a WAV or a test signal through a filter chain offline and checks that runs are bit-identical. A chain with NVIDIA's noise removal waits at least 5 s for it to start, here and in `knobs-compare`. It opens no audio devices.
- `knobs-compare`: runs the same input through the imported chain in knobs and in OBS itself, from a portable copy of the install in `%LocalAppData%\knobs\compare` with settings of its own. It opens no audio device and leaves `%AppData%\obs-studio` alone.
- `knobs-core`: the always-on core with no tray, printing each change of state. By default it opens no audio device; `--live` opens the mic and the cable. `--prune-runtime` prunes the real runtime folder, as knobs.exe does.
- `knobs-tray`: the tray app on a fake core, over a made-up OBS, devices and GitHub: every state of the menu, every page of the first run, notifications and the update check, with `--screenshot` for pictures. It opens no audio device, makes no network connection, never starts OBS, and reads nothing of OBS's but the install and a settings folder it's given. Problem states pop real notifications.
- `knobs-update`: the update check's parts against the real GitHub, one at a time. It installs nothing. It's the only tool that uses the network.
- `knobs-live`: the live path into a virtual cable, and measurements of latency, monitor restarts and device clocks. `--run`, `--measure-mic` and `--measure-drift --mic` open the mic. Every mode but `--list-devices` and `--measure-drift` refuses to run while another app plays into the cable, and the modes that play test clicks also while one records from it (`--force` overrides).

Don't open the mic without the user's go-ahead.

## Layout
- `src/runtime/`: finding OBS, the runtime copy and its pruning, loading `obs.dll` and its function table, the libobs session and its optional modules, the Qt application `obs-vst` needs (`QtApp`), logging, and `ObsHost`, which finds, copies and loads OBS and starts the session, in order. Pruning runs from `ObsBackend`.
- `src/import/`: reading OBS's settings (an INI reader that matches libobs's parser, the active profile and scene collection), finding the mics in a collection, and the pre-flight checks.
- `src/audio/`: the live path. Device lists (libobs's, and Windows' own with `DeviceWatch` for devices coming and going), and `LiveChain`, which loads a source through OBS's loader and monitors it.
- `src/core/`: the always-on core the tray runs. `Controller` makes the decisions with no threads of its own, on a `Backend` (`ObsBackend` is the real one), with event times and an injected clock for work that takes time. `Core` runs it on its own thread with libobs, and `ObsWatch` tells whether OBS is running. The tray hears state through `Observer`.
- `src/update/`: the update check's requests to GitHub (`github.h`), its releases' versions and addresses (`release.h`), and the check of an installer's signature (`signature.h`).
- `src/tray/`: the tray app. `TrayApp` is the icon and the menu; `BuildMenu` turns a snapshot into the menu as data, which the unit tests check for every state. `FirstRun` is the first run as data in the same way, and `SetupWindow` shows it in a window of its own (`setup_scene` lays out and paints a page, `setup_uia` is its UI Automation, `walkthrough` a guide's walkthrough, `pictures/` its screenshots). `Notifier` (`notices.h`) is the notifications and the icon's badge as data, and `Updater` (`updates.h`) the update check's plan. Also the settings file, the Run entry, the single-instance lock, starting OBS, and `ShutdownWatchdog`, which ends a quit that runs past its deadline. `src/main.cpp` is `knobs.exe`'s entry point.
- `src/util/`: `Result`, UTF-8, JSON, text file and Win32 helpers, knobs's app folders, file checks on a worker with a time limit (`AsyncProbe`), and files-in-use helpers: DLL oplocks, the watchdog that bounds their holds, and their restart handoff, loaded modules and folder fingerprints.
- `tools/`: the dev tools above, one folder each, with the code they share in `tools/common/` (not part of the app) and knobs-tray's fake backend in `tools/tray/`. `tools/vendor-libobs-headers.ps1` refreshes `third_party/libobs`, and `tools/render-brand-assets.ps1` renders `assets/` from `brand/`'s logo files ([assets/README.md](assets/README.md)).
- `tests/`: the unit tests (`knobs-tests`, no OBS needed: the core on a fake backend, the menu, the first run, notifications, walkthroughs, updates offline, the runtime layer, and quitting in child processes), the import and core tests on the made-up OBS settings in `tests/fixtures/obs-config`, and the vst test, which runs `tests/fixtures/vst`, a VST 2 plugin of knobs's own, through `obs-vst`. `tests/fixtures/dep` builds DLLs for the unit tests of `UnresolvedDllImports` and the shutdown tests.
- `packaging/`: the installer's Inno Setup script, the text files that ship in each package, `package.ps1`, which builds both packages, and `sign.ps1`, which signs with Azure Artifact Signing (set up in the `release` environment that `release.yml` uses).
- `.github/workflows/`: CI (`ci.yml`, which calls `build.yml`) and `release.yml`, which drafts a release from a `v<version>` tag.
- `docs/design.md`: how knobs is designed and why, with the evidence behind it. `DESIGN.md`: the setup window's design system, mirrored from the brand's tokens.
- `third_party/libobs/`: vendored libobs headers (declarations only). Don't edit them.
- `brand/`: knobs-brand, a git submodule: the brand's rules (`BRAND.md`), the logo's masters, and the design tokens, whose generated `dist/knobs_tokens.h` the setup window and the tray's badges are drawn from. Don't edit it here: change knobs-brand, run its `node tokens/build.mjs`, push, then move the submodule and render `assets/` again.
- `assets/`: the icons and installer images rendered from `brand/` ([assets/README.md](assets/README.md)).
- `PRODUCT.md`: who knobs is for, its voice and brand commitments. Read it before UI or copy work.

## Invariants
- **Fidelity is the product.** Audio passes only through OBS's own filter code, and the VST plugins and NVIDIA runtime it hosts. No custom DSP and no "improvements".
- **`%AppData%\obs-studio` is read-only**, as is a portable OBS's `config` folder. knobs keeps its own state in `%AppData%\knobs` and `%LocalAppData%\knobs`, or in the `data` folder of a portable copy (`AppDirs`); either way it reads OBS's settings from `%AppData%`, or a portable OBS's `config` folder. Some libobs helpers write: `obs_data_create_from_json_file_safe` renames a backup over a broken file, so read files yourself and parse them with `obs_data_create_from_json`.
- **Never load from the OBS install dir.** Load only from the shadow copy in `AppDirs::RuntimeBase()\<obs-version>\` (`%LocalAppData%\knobs\runtime`, or `data\runtime` in a portable copy). Hold `RuntimeCopyLock` from `EnsureRuntimeCopy` until `obs.dll` has loaded from the copy (`ObsHost` does). Only knobs.exe prunes the runtime folder; tests and tools don't, apart from `knobs-core --prune-runtime`.
- **No link-time libobs dependency.** Resolve exports into the function table (`KNOBS_OBS_API` in `src/runtime/obs_api.h`) with `GetProcAddress`. Add new libobs functions there. A missing export fails gracefully with a clear message.
- **Load `win-wasapi` and `obs-filters`, and the optional modules only for the picked mic's chain.** `obs-vst` and `nv-filters` (`kOptionalObsModules`) load when that chain has their filter, and `nv-filters` alone costs hundreds of MB. Never open a VST plugin's window: every source loads with `obs-vst`'s Open interface when active off (`LoadSourceJson`), since a QWidget with no QApplication aborts the process.
- **No network until the user asks.** The update check is off until it's turned on, and is knobs's only connection of its own; a VST plugin in the chain runs inside knobs and can go online, as it would in OBS. Updating by itself runs only an installer `update::CheckReleaseSignature` accepts, checked again just before it runs.
- **Never ship libobs.** A from-source build is only for local debugging.
- Import uses `obs_load_source()`, in its private form `obs_load_private_source()`, never a hand-written replay of the settings (`obs_source_create`, `obs_source_filter_add`).
- Check OBS behaviour (config keys, scene JSON, libobs semantics) against the obs-studio source at the tag matching the installed version. Don't rely on memory.
- When the tested OBS versions change, re-vendor the headers at the newest one's tag, revisit the supported range in `src/runtime/obs_version.h`, and update the versions in `build.yml`. An export older supported versions lack goes in `KNOBS_OBS_API_OPTIONAL`, with a fallback.

## Conventions
- Every source file starts with `// SPDX-License-Identifier: GPL-2.0-or-later` (`;` or `#` comments in scripts). A `.manifest` has it on its second line, after the XML declaration, and JSON, which has no comments, has none.
- The logo files in `assets/` and in `brand/logo/` and `brand/web/` aren't GPL: they're all rights reserved, apart from what `TRADEMARKS.md` allows. Don't add SPDX lines to them or move them.
- The UI's colours, shapes, radius, focus ring and motion come from the brand's tokens (`knobs::tokens` in `knobs_tokens.h`), never literals. The app keeps Windows' own type (Segoe UI Variable, Consolas), not the brand's.
- The name is "knobs", all lowercase, even at the start of a sentence. Keep it in one constant (`KNOBS_DISPLAY_NAME` in `src/app_info.h`), and don't write it out in user-facing strings; tests shouldn't depend on it either.

## Git
- Commit straight to `main`.
- Conventional Commits: `type(scope): subject`. Types: `feat`, `fix`, `refactor`, `perf`, `test`, `docs`, `build`, `chore`. Optional scopes: `runtime`, `import`, `audio`, `harness`, `tray`.
- Imperative, lowercase subject, no trailing period, 72 chars max. Add a body only when the why isn't obvious.
