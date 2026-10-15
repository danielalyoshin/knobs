<h1 align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/danielalyoshin/knobs-brand/main/logo/svg/knobs-lockup-horizontal-light.svg">
    <img src="https://raw.githubusercontent.com/danielalyoshin/knobs-brand/main/logo/svg/knobs-lockup-horizontal-dark.svg" alt="knobs" width="320">
  </picture>
</h1>

<div align="center">

**Your OBS mic chain, in every app.**

A Windows tray app that runs the mic filter chain you tuned in OBS Studio and sends it to a virtual audio cable.<br>
Your other apps get the same processed mic while OBS stays closed.

[![Release](https://img.shields.io/github/v/release/danielalyoshin/knobs?include_prereleases&sort=semver&label=release&color=c55149&labelColor=191b1d)](https://github.com/danielalyoshin/knobs/releases/latest)
[![CI](https://img.shields.io/github/actions/workflow/status/danielalyoshin/knobs/ci.yml?branch=main&label=CI&color=5c6169&labelColor=191b1d)](https://github.com/danielalyoshin/knobs/actions/workflows/ci.yml)
[![Windows x64](https://img.shields.io/badge/Windows-x64-5c6169?labelColor=191b1d)](#requirements)
[![OBS Studio 32.2 to 33](https://img.shields.io/badge/OBS%20Studio-32.2%20to%2033-5c6169?labelColor=191b1d)](#requirements)
[![License: GPL-2.0-or-later](https://img.shields.io/badge/license-GPL--2.0--or--later-5c6169?labelColor=191b1d)](LICENSE)

[Download](https://github.com/danielalyoshin/knobs/releases/latest) · [Setup](#setup) · [How it works](#how-it-works) · [Known limits](#known-limits) · [Design](docs/design.md) · [Issues](https://github.com/danielalyoshin/knobs/issues)

<sub>An independent project. Not affiliated with or endorsed by the OBS Project.</sub>

</div>

<br>

> [!NOTE]
> **knobs is new.** It works end to end and has been tested on real hardware with OBS 32.2.2, an Audient iD4 and VB-CABLE, but few people have run it yet. If something doesn't work, [open an issue](https://github.com/danielalyoshin/knobs/issues).

## Why knobs

If you tuned your mic in OBS and send it to a virtual cable, OBS has to stay open all day, even when you aren't streaming or recording. Since OBS 32.0 it's also harder to leave running unattended: OBS ignores `--disable-shutdown-check`, so its Safe Mode prompt can stop it from starting by itself ([obs-studio#12650](https://github.com/obsproject/obs-studio/issues/12650)).

Other tools don't sound the same. Equalizer APO, VoiceMeeter and NVIDIA Broadcast have their own audio processing (DSP), so a chain tuned in OBS has to be rebuilt by ear, and it can't match exactly. OBS plugins that send audio elsewhere still need OBS open.

knobs runs OBS's own filter code, from your OBS install, with your OBS settings. Nothing else touches the audio, apart from the VST plugins and NVIDIA noise removal your OBS chain already runs.

## Measured against OBS

**Same sound.** For the same input, knobs's output is bit-identical to OBS's: on test signals, on a chain of every audio filter in OBS's `obs-filters`, on VST plugins, and on a recording of real voice. `knobs-compare` checks this against OBS itself. Two cases vary in OBS too: NVIDIA's noise removal, whose output depends on when the mic started, and a stereo VST plugin on a mono profile ([Known limits](#known-limits)). NVIDIA's output is close to OBS's, but not exactly the same.

**Same delay.** From the mic to the cable takes 88.1 ms in knobs and 87.7 ms in OBS, on the same cable input.

**A fraction of the footprint.** knobs running a four-filter chain (3-Band Equalizer, Expander, Compressor, Limiter), against OBS minimized on the same PC:

| | knobs, running | OBS, minimized |
|:---|--:|--:|
| CPU, one core | 0.9% | 7.1% |
| Memory (working set) | 27 MB | 443–448 MB |
| Threads | 12–13 | 89–90 |
| GPU | None | 0.05% |

<sub>Measured twice over 60 s on one PC (Core Ultra 7 265K) with OBS 32.2.2, an Audient iD4 and VB-CABLE. [docs/design.md](docs/design.md#on-real-hardware) has the method.</sub>

## How it works

- **Your OBS does the processing.** knobs ships no libobs and no DSP of its own. It loads OBS's `obs.dll` and the modules your mic needs, `win-wasapi` for the mic and `obs-filters` for the filters, plus `obs-vst` for VST plugins and `nv-filters` for NVIDIA's noise removal when your mic has them, from a private copy of your OBS install in `%LocalAppData%\knobs`. The install itself stays unlocked, so a running knobs never blocks an OBS update.
- **OBS's loader rebuilds your mic.** knobs reads your active OBS profile and scene collection, and OBS's own loader recreates the mic from them: its filters in order, with their settings, and its Mono, balance and volume. knobs never writes to OBS's settings.
- **Out to a virtual cable.** libobs's audio monitoring, the same code OBS uses, sends the processed mic to the cable. There's no video, no window and no OBS process. Your other apps choose the cable's recording device, such as CABLE Output, as their mic.
- **Out of OBS's way.** knobs pauses while OBS is open, so the cable never gets your mic twice. When OBS closes, knobs reads its settings again and runs whatever you changed.
- **Invisible while it works, loud when it breaks.** knobs lives in the tray and starts with Windows: in testing, the mic was on the cable 9.5 s after signing in, with nothing clicked. It rebuilds the chain when the mic or the cable comes back, and a notification says when something needs you.

## Requirements

| Requirement | Supported |
|:---|:---|
| Windows | 10, version 1803 or later, or 11. 64-bit. |
| OBS Studio | 32.2 to 33.x, tested on 32.2.2 and 33.0.0-rc1. knobs finds an install from OBS's installer by itself; for a Steam or portable OBS, you choose its folder. |
| Virtual cable | A sound device that exists only in software, which your other apps choose as their mic. [VB-CABLE](https://vb-audio.com/Cable/), which knobs's installer can install, or another one: knobs recognizes VB-Audio's cables (VB-CABLE, VoiceMeeter, Hi-Fi Cable) and Virtual Audio Cable. |
| Mic filters | OBS's built-in audio filters: Noise Suppression (RNNoise or Speex), Noise Gate, Expander, Upward Compressor, Compressor, Limiter, Gain, 3-Band Equalizer and Invert Polarity. Also VST 2.x plugins, as OBS hosts them, and NVIDIA Audio Effects (NVIDIA's noise removal), with NVIDIA's Audio Effects SDK and an NVIDIA RTX GPU, as in OBS. [Known limits](#known-limits) has the exceptions. |

## Setup

### 1. Install OBS and a virtual cable

- [OBS Studio](https://obsproject.com/download) 32.2 to 33.x.
- A virtual cable such as [VB-CABLE](https://vb-audio.com/Cable/). knobs's installer can install VB-CABLE for you (step 3), so you can leave it until then.

### 2. Tune your mic in OBS

**If your mic is already set up in OBS,** there's nothing to do here. knobs uses the mic and filters in OBS's active profile and scene collection, and if OBS's **Monitoring Device** is a cable, setup suggests that one.

**If you're new to OBS,** you only need the mic, not a scene, a stream or a recording:

1. Open OBS.
2. In **Settings › Audio**, set **Mic/Auxiliary Audio** to your mic, if it isn't already.
3. In the **Audio Mixer**, click **Mic/Aux** and choose **Filters**.
4. Under **Audio Filters**, click **+** and add filters as needed. They run from the top of the list down.
5. Click each filter and adjust it by ear, starting from OBS's defaults.
6. Close OBS. knobs reads your filters when OBS closes.

To hear the filters while you adjust them, choose headphones as the **Monitoring Device** in **Settings › Audio**, then turn on the mic's **Audio Monitoring** in **Edit › Advanced Audio Properties**: check **Enable** in OBS 33, or choose **Monitor Only (mute output)** in OBS 32.2.

<details>
<summary><b>What each audio filter does</b></summary>
<br>

| Filter | What it does |
|:---|:---|
| [Noise Suppression](https://obsproject.com/kb/noise-suppression-filter) | Reduces steady background noise, such as a fan or hiss. RNNoise sounds better and uses more CPU than Speex. |
| [Noise Gate](https://obsproject.com/kb/noise-gate-filter) | Silences the mic while its level is below a threshold, such as between words. |
| [Expander](https://obsproject.com/kb/expander-filter) | Turns quiet sounds down, more gently than a gate. |
| Upward Compressor | Turns quiet sounds up. |
| [Compressor](https://obsproject.com/kb/compressor-filter) | Turns loud sounds down past a threshold, so the level stays more even. |
| [Limiter](https://obsproject.com/kb/limiter-filter) | Keeps the level from going over a threshold. |
| [Gain](https://obsproject.com/kb/gain-filter) | Turns the level up or down. |
| 3-Band Equalizer | Turns the low, mid and high frequencies up or down. |
| [Invert Polarity](https://obsproject.com/kb/invert-polarity-filter) | Flips the waveform upside down. |

OBS's [Filters Guide](https://obsproject.com/kb/filters-guide) has more on each.

</details>

### 3. Install and start knobs

Download knobs from the [latest release](https://github.com/danielalyoshin/knobs/releases/latest). There are two ways to run it:

- **The installer**, `knobs-<version>-setup.exe`. It installs knobs for your Windows account only, with no admin rights, adds it to the Start menu, and starts it at the end. With no virtual cable on your PC, it offers to install VB-CABLE, VB-Audio's virtual cable: VB-Audio's own installer opens, Windows asks to let it install its driver, and you click **Install Driver**. VB-Audio's website may then open in your browser. If Windows has to restart before the cable appears, the installer asks, and knobs opens again after the restart.
- **The portable zip**, `knobs-<version>-portable.zip`. Unzip it anywhere and run `knobs.exe`. It keeps its settings and its data in a `data` folder beside the exe, and nothing in your user folders.

knobs is signed by Daniel Alyoshin, through Azure Artifact Signing. Windows SmartScreen may still warn about a new release until enough people have run it. Choose **More info**, then **Run anyway**.

knobs then opens its setup window, which shows what it found in OBS and asks only what it needs to:

- **Which mic**, if OBS has several.
- **Which cable**, always, with OBS's **Monitoring Device** chosen to start with. Your choice stays yours if OBS's Monitoring Device changes later. Every other playback device is in a dropdown under the cables, for a cable knobs doesn't know by name. With no cable installed, it shows how to install VB-CABLE, step by step, and lets you choose it when it appears. It also says when a cable is turned off in Windows' Sound settings, waits for Windows to restart, or gets your PC's sound because Windows made it the output.
- **Filters it can't run**, if there are any. [Known limits](#known-limits) lists them.

If you choose **I'm new to OBS**, it shows the steps above instead, and carries on by itself when you close OBS. The last page shows your chain, with **Start with Windows** checked, and asks whether knobs should check for new versions, and update itself when there's one. Both are off until you turn them on. Closing the page is the same as **Done**.

knobs then lives in the tray. Windows 11 hides new tray icons, so click **^** (**Show hidden icons**) on the taskbar to find the knob.

### 4. Choose the cable in your apps

In your other apps, choose the cable's recording device as your mic. For VB-CABLE, that's **CABLE Output**.

> [!TIP]
> Apps can process your mic again on top of your chain, with noise suppression, echo cancellation or automatic gain control of their own. Turn those off in each app's audio settings to hear exactly what you tuned.

## Using knobs

**To change your sound,** open OBS, change the filters and close it. knobs pauses while OBS is open, runs the new chain when it closes, and says what changed in a notification.

**The tray menu** opens on a left or right click on the knob.

| Item | What it does |
|:---|:---|
| Status line | What knobs is doing, and the chain it runs, such as `Mic/Aux › 4 filters › CABLE In 16ch`. When something needs you, the fix follows it in bold. |
| Pause / Resume | Stops sending to the cable and lets go of the mic, or starts again. |
| Mic ▸ | Which of OBS's mics to run. **Same as OBS** follows OBS's only mic, even when it's renamed or replaced. |
| Cable ▸ | Where to send the mic. Your choice stays, whatever OBS's Monitoring Device is. Every other playback device is under **Other playback devices**. |
| Re-import from OBS | Reads OBS's settings now, rather than when OBS next closes. |
| Pause while OBS is open | On by default. If you turn it off while OBS also plays your mic into the same cable (its Audio Monitoring), the cable gets your mic twice. |
| Start with Windows | Starts knobs when you sign in. |
| Automatically check for new versions | Once a day, asks GitHub which version is the latest. Off until you turn it on ([Privacy](#privacy)). |
| Automatically update | Installs a new version by itself, when knobs isn't sending your mic anyway or as it starts, and says so if it can't. If a day goes by without such a moment, a notification offers to update now. Only for a copy the installer installed, and only with the check on. |
| Get knobs 0.3.0… | When a check found a newer version: opens its page, with what's new and the download. Once an update has downloaded, **Update to knobs 0.3.0** installs it now instead. |
| Setup… | Opens the setup window again. |
| Open log folder | Opens knobs's logs. |
| Quit | Closes knobs. While it finishes, the tray says **Closing…** and stays responsive. |

If quitting gets stuck, knobs ends the process after ten seconds, or sooner for a waiting installer.

**Notifications** say when:

- the mic or the cable has been missing for 5 seconds
- your chain changed in OBS, or has a filter knobs can't run
- knobs pauses while a plugin or NVIDIA's SDK is updated, or an installer may need to try again
- OBS was updated
- Windows plays your PC's sound into the cable
- with the update check on, a new version of knobs is out, or knobs updated itself, or couldn't

A click on one opens the fix. knobs restarts by itself, and says why, when OBS updates to a version it supports, when NVIDIA's SDK is installed or updated, when an installer updates a plugin file knobs can't unload, and to free NVIDIA's memory once your chain stops using it. It stays paused if you paused it. With **Do not disturb** on, the icon still shows the state: a pause badge while paused, and a **!** badge while the cable isn't getting your mic.

**knobs keeps its files** in two folders, and never writes to OBS's settings or its install. A portable copy keeps both in the `data` folder beside its `knobs.exe` instead.

| Folder | Holds |
|:---|:---|
| `%AppData%\knobs` | `settings.ini`: your choices in the menu and the setup window |
| `%LocalAppData%\knobs` | The private copy of OBS's files (about 80 MB), the logs, and an update's installer while it waits to install |

**To update,** install the new version over the old one, or turn on **Automatically update**. A portable copy says when a new version is out, with the update check on, and you unzip the new one in its place.

**To uninstall,** use **Settings › Apps › Installed apps** in Windows. Uninstalling quits knobs, turns off Start with Windows, and removes the copy of OBS's files and the logs. It then asks whether to delete your settings too. VB-CABLE stays installed, since other apps may use it: it's in Installed apps as **VBCABLE, The Virtual Audio Cable**. For a portable copy, turn off **Start with Windows** in its menu, quit it, and delete its folder.

## Known limits

- **One mic.** With several mics in OBS, knobs runs the one you choose.
- **VST plugins run without their windows.** knobs runs a VST filter as OBS does, with the settings OBS saved, but never opens the plugin's window, even when OBS's **Open interface when active** is on. A plugin that crashes takes knobs down, as it takes OBS down. When an installer needs its loaded DLL, knobs pauses, unloads the plugin and resumes after its folder has stopped changing for five seconds. If the plugin keeps a file loaded that knobs can't unload, knobs restarts instead. A plugin on a drive that's offline or slow to answer passes audio through, and knobs says so: choose **Re-import from OBS** once the drive is back. An installer that uses Windows' Restart Manager can also close knobs and open it again after the update. On a mono OBS profile, a stereo plugin can sound slightly different from run to run, in OBS too, since OBS's VST host feeds its second input from memory it never clears.
- **NVIDIA's noise removal is costly, and not bit-identical.** It runs on your GPU through NVIDIA's runtime, which takes about 800 MB of memory and a fifth of a CPU core while the mic runs. Once your chain stops using it, knobs restarts to free that memory. Its output is close to OBS's, but not exactly the same, since it depends on what it heard before, in OBS too. For its first moment after the mic starts, it lets the mic through untouched while it loads, as it does when OBS starts. Without NVIDIA's Audio Effects SDK or a GPU that runs it, the filter passes audio through, and knobs says why. Installing the SDK while knobs runs restarts knobs to load it, and updating the SDK restarts knobs to free its DLLs, then waits for its folder to stop changing for five seconds before loading it again. A copy you paused stays paused.
- **Some plugin updates need you to pause or quit.** If an installer says a file is in use, pause knobs for a VST update, or quit it for an update of NVIDIA's SDK or if pausing isn't enough, then retry the install. This can happen when an update takes long breaks between files, replaces files elsewhere, or another app uses them. Some drives don't support updating while the plugin runs. If a plugin gets stuck while unloading, or knobs can't restart promptly, it stops holding the installer and says it may need to retry. The file may still be in use: quit knobs, wait for its tray icon to disappear, then retry.
- **No sidechain.** With a sidechain source, OBS's compressor ducks the mic when that source plays. knobs runs no other sources, so it keeps only the compressor's output gain, which is how the compressor sounds in OBS while the sidechain is silent. It warns you.
- **Mute and push-to-talk don't silence the cable.** OBS's monitoring ignores them too. Setup says so.
- **The cable is quiet while OBS is open**, unless OBS also plays your mic into it (its Audio Monitoring). Turn off **Pause while OBS is open** to keep knobs running then.
- **A new sample rate or channel layout in OBS restarts knobs**, as it restarts OBS, with a moment of silence.
- **Windows only, and OBS 32.2 to 33.x only,** for now.

## Privacy

knobs collects nothing and has no telemetry and no account. It makes no network connections unless you turn on its update check: the links in its setup window open in your browser when you click them, and the installer carries VB-CABLE rather than downloading it. A VST plugin in your OBS chain runs inside knobs as it does inside OBS, though, so a plugin that goes online, to check its license, say, does so from knobs. Your mic's audio goes from the mic, through OBS's filters, to the virtual cable, and stays on your PC.

**The update check** is off until you turn it on, on the setup window's last page or in the tray menu. Then, once a day, knobs asks GitHub for `github.com/danielalyoshin/knobs/releases/latest`, which redirects to the latest release, and reads only which version that is. It sends nothing about you or your PC, though GitHub sees your IP address, as any website does. If you also turn on **Automatically update**, knobs downloads the new version's installer from that release on GitHub, checks that it's signed by Daniel Alyoshin as every release is, and runs it. Windows may ask Microsoft whether the installer's certificate was revoked, as it can for any signed program. If it can't tell, knobs keeps the download and checks again an hour later, repeating until Windows can finish the check. Network failures are retried hourly too. An installer with the wrong signature is never run, and knobs says so. A missing or oversized installer stops automatic attempts for that version after three tries, with a link to download the new version yourself.

knobs keeps its settings and logs on your PC, in the folders under [Using knobs](#using-knobs). The log names your audio devices, your OBS mic and its filters, and folders on your PC, which can include your Windows user name. Look it over before you attach it to an issue.

## Development

### Building

You need Visual Studio 2022 or newer with the **Desktop development with C++** workload, and CMake 3.25 or newer. From a Developer PowerShell for VS:

```powershell
git clone --recurse-submodules https://github.com/danielalyoshin/knobs.git
cd knobs
cmake --preset x64
cmake --build --preset release   # or: debug
ctest --preset release           # tests that need OBS skip if it isn't installed
```

`brand\` is [knobs-brand](https://github.com/danielalyoshin/knobs-brand), a git submodule with the brand's logo and design tokens; in a clone made without `--recurse-submodules`, run `git submodule update --init` first. The build puts `knobs.exe` and the dev tools in `build\x64\Release\` (or `Debug\`). No test opens an audio device or shows real tray icons or notifications. To run the tests that need OBS against another OBS, such as an unzipped release, configure with `-DKNOBS_TEST_OBS_DIR=<folder>`, and with `-DKNOBS_TEST_OBS_DIR=` to go back to the installed one.

### Packaging

`packaging\package.ps1` packages the Release build as the installer and the portable zip, with their checksums, in `build\package\`. The installer needs [Inno Setup 6](https://jrsoftware.org/isinfo.php), and carries VB-Audio's VB-CABLE driver pack, which the script downloads from vb-audio.com the first time and checks against a fixed SHA-256.

[CI](.github/workflows/ci.yml) builds knobs, runs every test with OBS Studio 33.0.0-rc1 installed, repeats the tests that need OBS against an unzipped 32.2.2, and packages the installer and portable zip on each push and pull request. Pushing a tag such as `v0.1.0` drafts a release with the packages ([release.yml](.github/workflows/release.yml)), signed with [Azure Artifact Signing](https://azure.microsoft.com/en-us/products/artifact-signing) (`packaging\sign.ps1`).

### Dev tools

Each one takes `--help`, and none of them changes OBS's settings.

| Tool | What it does |
|:---|:---|
| `knobs-smoke` | Checks that libobs loads from the private copy and shuts down cleanly |
| `knobs-import` | Imports the mic from OBS and reports each step and warning |
| `knobs-harness` | Runs audio through a chain offline, and checks runs are bit-identical |
| `knobs-compare` | Runs the same input through knobs and OBS, and measures the difference |
| `knobs-core` | Runs the tray app's core without the tray, printing each change of state |
| `knobs-tray` | Shows every menu state, first-run page and notification, on a fake core |
| `knobs-live` | Runs the live path into a cable, and measures latency and device clocks |
| `knobs-update` | Asks GitHub for the latest release, downloads an installer, and checks its signature, as the update check does |

Most of them open no audio device. `knobs-smoke --capture-seconds` opens the mic, and `knobs-core --live` the mic and the cable. `knobs-live` opens the cable, and the mic only with `--run`, `--measure-mic` or `--measure-drift --mic`. Only `knobs-update` uses the network.

### Layout

| Folder | Holds |
|:---|:---|
| `src/runtime` | Finding OBS, the private copy, loading `obs.dll` and running libobs |
| `src/import` | Reading OBS's settings, finding the mics, and the pre-flight checks |
| `src/audio` | Audio devices and the live chain |
| `src/core` | The always-on core: following OBS and the devices, and rebuilding the chain |
| `src/tray` | The tray app: menu, first run, notifications, settings and the update check's plan |
| `src/update` | The update check's requests to GitHub, and the check of an installer's signature |
| `tools/` | The dev tools |
| `packaging/` | The installer's script, the files that ship in each package, and the script that builds both |
| `docs/` | The design, and the evidence behind it |
| `tests/` | Unit tests, the import and core tests on made-up OBS settings, and a VST plugin of its own for the VST test |

[docs/design.md](docs/design.md) describes how knobs is designed and why, with the measurements behind it.

## Contributing

Issues and pull requests are welcome. A few rules keep knobs what it is:

- **Audio passes only through OBS's own filter code.** No DSP of knobs's own, and no "improvements".
- **OBS's settings and install are read-only.**
- **OBS's behavior is checked against the obs-studio source** at the tag of the tested version, not from memory.
- Commits follow [Conventional Commits](https://www.conventionalcommits.org/).

If knobs sounds different from OBS, include the output of `knobs-import` and the log (**Open log folder** in the menu).

| Problem | Where it belongs |
|:---|:---|
| Importing, the tray app, the path to the cable | This repository |
| A filter that sounds wrong in OBS too | [obsproject/obs-studio](https://github.com/obsproject/obs-studio) |
| The virtual cable itself | [VB-Audio](https://vb-audio.com/Cable/), or your cable's maker |

## License

knobs's code is licensed under **GPL-2.0-or-later**, the same as libobs. See [LICENSE](LICENSE). `third_party/libobs` holds headers from obs-studio under their own GPL-2.0-or-later notices. knobs ships no OBS binaries: it runs the OBS you installed.

The installer can install VB-CABLE, VB-Audio's virtual cable, from [www.vb-cable.com](https://vb-cable.com). It carries VB-Audio's driver pack unchanged and runs VB-Audio's own installer, as [VB-Audio's licensing](https://vb-audio.com/Services/licensing.htm) allows. VB-CABLE isn't covered by the GPL. In VB-Audio's words, "VB-CABLE is a donationware, all participations are welcome."

The knobs name and logo are trademarks of Daniel Alyoshin. The logo files, the masters in [knobs-brand](https://github.com/danielalyoshin/knobs-brand) and the icons rendered from them in [`assets/`](assets/), aren't under the GPL: you can share them unmodified and use them to refer to knobs, and a fork that you distribute needs its own name and icon. [TRADEMARKS.md](TRADEMARKS.md) has the details.

knobs is an independent project. It isn't affiliated with or endorsed by the OBS Project.
