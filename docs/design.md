# knobs design

How knobs is designed and why. The [README](../README.md) has the short version. OBS's behavior is checked against the obs-studio source at the tags of the tested versions, OBS 32.2.2 and 33.0.0-rc1. The measurements below use 32.2.2 unless they say otherwise.

## Overview

knobs is a Windows tray app that runs the mic filter chain a user tuned in OBS Studio and sends the result to a virtual audio cable, with no OBS process. Other apps choose the cable's recording side, such as VB-Cable's CABLE Output, as their mic.

Without knobs, that sound needs OBS open all day, and since OBS 32.0 the Safe Mode prompt can stop OBS from running unattended ([obs-studio#12650](https://github.com/obsproject/obs-studio/issues/12650)). Other tools, such as Equalizer APO, VoiceMeeter and NVIDIA Broadcast, have their own DSP, and OBS's gate, expander and compressor are custom code in its `obs-filters` module.

So knobs doesn't reimplement OBS's filters: it runs them. It loads libobs (`obs.dll`) and two of its modules from the user's own OBS install, and recreates the mic with OBS's own loader from the user's own settings. The audio passes through the same compiled code as in OBS, with the same settings in the same order, so for the same input, knobs's output is bit-identical to OBS's.

The name is "knobs", all lowercase, as in the audio kind. It leaves "OBS" out, as the OBS Project's [Forum Resource and IP Policy](https://obsproject.com/forum/threads/forum-resource-and-ip-policy.178569/) asks of third-party tools, and it's defined once (`KNOBS_DISPLAY_NAME`) in case the OBS team asks for a change.

## Principles

- **Fidelity is the product.** Audio passes only through OBS's own code: the same filters, settings and order, and the same Mono, balance and volume. knobs has no DSP of its own.
- **Leave OBS alone.** knobs never writes to OBS's settings or install. It loads OBS from a private copy, so the install stays unlocked, and it pauses while OBS runs.
- **Invisible while it works.** Tray only, with no window, no video and no GPU, and it starts with Windows. The target is under 1% of a core.
- **Loud when it breaks.** A missing mic, cable or OBS gets a notification that says what to do, and a badge on the icon.

## Architecture

```mermaid
flowchart TB
  install[("OBS Studio install")] -- "copied once per OBS version" --> copy[("Private runtime copy<br>%LocalAppData%\knobs\runtime")]
  obsconfig[("OBS's settings<br>%AppData%\obs-studio<br>read-only")]

  subgraph app["knobs.exe"]
    direction TB
    tray["Tray app<br>menu, first run, notifications"]
    core["Always-on core<br>follows OBS and the devices"]
    loader["Runtime loader<br>obs.dll, win-wasapi, obs-filters"]
    importer["Importer<br>active profile and scene collection"]
    subgraph libobs["libobs: audio only, no video"]
      direction TB
      source["Mic source<br>wasapi_input_capture<br>Mono, balance, volume"]
      filters["The mic's filters, in OBS's order"]
      monitor["Audio monitoring"]
      source --> filters --> monitor
    end
  end

  tray -. "commands" .-> core
  core -. "state" .-> tray
  core --> loader
  core --> importer
  copy --> loader
  obsconfig --> importer
  importer -- "obs_load_private_source" --> source
  mic(["Mic"]) --> source
  monitor --> cable(["Virtual cable<br>such as CABLE In 16ch"])
  cable --> apps(["Other apps<br>record from CABLE Output"])
```

- **Runtime loader** (`src/runtime/`): finds OBS, keeps the private copy of its files, loads `obs.dll` and runs libobs (`ObsHost`).
- **Importer** (`src/import/`): reads OBS's settings, lists the mics in the active scene collection, and checks the chosen one before it loads.
- **Live chain** (`src/audio/`): `LiveChain` loads the mic through OBS's loader and monitors it to the cable. `DeviceWatch` hears audio devices come and go.
- **Always-on core** (`src/core/`): `Controller` decides what runs, `Core` runs it and libobs on one thread, and `ObsWatch` tells whether OBS is running.
- **Tray app** (`src/tray/`, with `src/main.cpp` as the entry point): the icon, menu, first run and notifications, each built as data from a snapshot of the core so that unit tests cover every state.
- **Update check** (`src/update/`, run by the tray): off until the user turns it on. It asks GitHub for the latest release, and can download its installer, check its signature and run it (Updates).

The dev tools in `tools/` run each layer on its own, from `knobs-smoke` (the runtime) to `knobs-tray` (the tray on a fake core).

## Runtime

### Finding and copying OBS

knobs ships no libobs. It finds OBS through the installer's registry key (`HKLM\SOFTWARE\OBS Studio`), then `C:\Program Files\obs-studio`, and the user picks the folder of a Steam or portable OBS. The version comes from `obs.dll`'s version resource, so nothing in the install is loaded.

knobs loads OBS from a copy in `%LocalAppData%\knobs\runtime\<obs-version>\`, never from the install, so a running knobs never blocks an OBS update. The copy mirrors the install's layout, so libobs's relative data lookups still work. What's copied comes from the PE import tables: the import closure of `obs.dll`, `libobs-d3d11.dll` and the two modules, limited to DLLs the install ships, plus their data folders. For 32.2.2 that's 200 files and 53.6 MB, and for 33.0.0-rc1 201 files and 53.6 MB, mostly FFmpeg, which `obs.dll` imports.

Where the modules are depends on the version. OBS 33 gives each module a folder of its own, `core\<module>`, with its data in `core\<module>\data`. Earlier versions keep them together in `obs-plugins\64bit`, with their data in `data\obs-plugins\<module>`. So knobs reads `obs.dll`'s version first, then looks for the rest where that version keeps it (`HasCoreModules`). An OBS it doesn't support is reported as unsupported, not as an incomplete install, since its files may be somewhere knobs doesn't know.

A copy is built in a staging folder and finished by a manifest written last, so an interrupted copy is never used. The manifest records each file's size and timestamp, and a copy is reused only while the install still matches, since OBS drops beta and RC suffixes from its version resource. A mutex (`RuntimeCopyLock`), held until `obs.dll` has loaded, serializes making, loading and pruning copies across processes. A copy being replaced is first renamed aside, which fails while libobs works in it, so a copy in use is never half replaced. Once libobs runs, knobs.exe prunes copies of other versions and leftovers in the background.

A new OBS version needs a new process, because libobs never unloads module DLLs (`free_module`, `obs-module.c`), and they keep `obs.dll` loaded. When OBS is updated, knobs restarts itself and copies the new version as it starts.

### Loading libobs

knobs resolves the libobs functions it uses with `GetProcAddress` into a function table (`KNOBS_OBS_API` in `src/runtime/obs_api.h`), typed from the vendored headers in `third_party/libobs`. With no import library, a missing export fails with a clear message instead of at process load. An export only some supported versions have, such as 33.0's `obs_source_set_monitoring_enabled`, is optional instead (`KNOBS_OBS_API_OPTIONAL`): it's null where libobs lacks it, and knobs falls back. `obs.dll`'s dependencies resolve from the copy and the system, never from PATH or the install. libobs finds its data relative to the working directory (`find_libobs_data_file`, `obs-windows.c`), so that's the copy's `bin\64bit` for as long as libobs runs.

knobs checks the OBS version before copying and again after loading, and refuses an unsupported one (Compatibility). It opens only two modules, by absolute path:

| Module | Provides |
|---|---|
| `win-wasapi` | `wasapi_input_capture`, the mic source |
| `obs-filters` | the audio filters, from noise suppression to the limiter |

It doesn't load `obs-vst`, or `nv-filters`, where OBS 32 keeps NVIDIA's noise removal. Since knobs never calls `obs_load_all_modules()`, or 33.0's `obs_load_core_modules()`, OBS's safe-mode and disabled-module lists don't apply.

### The libobs session

1. `base_set_log_handler()`, to knobs's log in `%LocalAppData%\knobs\logs`, so that startup is logged.
2. `obs_startup()` with knobs's own module config folder, `%AppData%\knobs\module-config`, never OBS's. It initializes COM as a single-threaded apartment on its thread, so `obs_shutdown()` must run on the same thread.
3. `obs_reset_audio()` with the OBS profile's sample rate and channel layout.
4. No `obs_reset_video()` (Audio without video).
5. `obs_open_module()` and `obs_init_module()` for the two modules, then `obs_post_load_modules()`.
6. `obs_load_private_source()` with the mic's saved source (Loading the mic).
7. Monitoring on, with `obs_source_set_monitoring_enabled()` on OBS 33 and `obs_source_set_monitoring_type()` to Monitor Only on 32.2, and `obs_source_inc_active()`, since the monitor plays only while the source is active.
8. `obs_set_audio_monitoring_device()` with the cable.
9. At shutdown, release the sources and drain libobs's destroy queue as `obs_wait_for_destroy_queue` does, then `obs_shutdown()`, which waits for that queue only when there's a video thread. Otherwise a source could be destroyed after its module has unloaded.

A start that fails before any module loads can be tried again in the same process. A loaded module never unloads, so after that, trying again restarts knobs. Once the copy exists, libobs starts and imports the mic in about 30 ms.

## Importing the mic

The importer reads OBS's settings as OBS's frontend does, which is the same in 32.2.2 and 33.0, and writes nothing. `knobs-import` runs it and reports each step.

### OBS's settings

OBS keeps its settings in `%AppData%\obs-studio`, or for a portable OBS (one with a marker file such as `portable_mode.txt` beside its `bin` folder) in `<install>\config\obs-studio`. OBS's `--portable` flag can't be seen from outside, so the user can also pick the folder. knobs reads the files itself, because some libobs helpers write: `obs_data_create_from_json_file_safe` renames a backup over a broken file.

- The active profile and scene collection come from `user.ini` (OBS 31 and later) or `global.ini`. Both are found by name, as in OBS 32: the name saved in each profile's `basic.ini` and each collection's `.json`.
- INI files are read by a parser that matches libobs's (`util/config-file.c`), since the profile sets libobs's audio format and is read before libobs starts.
- Scene collections are parsed with `obs_data_create_from_json`, falling back to the `.bak` file as OBS does.
- The profile gives the sample rate, the channel layout and the monitoring device. A profile that monitors to `default`, usually speakers, needs the user to choose a cable.

### Mic candidates

A mic is any `wasapi_input_capture` source among the collection's global audio devices and in its `sources`. With several, the user picks one by name. With one, knobs follows it, even when it's renamed or replaced. A mic set to "default" records from the default communications device, not the default recording device (win-wasapi's `InitDevice` asks for `eCommunications`), and the tray says so.

### Pre-flight

Before loading, the importer checks the mic's saved source:

- VST filters are removed, with a warning.
- Filters libobs doesn't know, such as NVIDIA's noise removal, stay in as placeholders that pass audio through, with a warning.
- A compressor with a sidechain becomes a Gain filter at its output gain, with a warning. knobs runs no other sources, so the sidechain would be silent, and while its sidechain is below the threshold, the compressor multiplies each sample by exactly its output gain (`compressor_filter_audio`), as the Gain filter does. Left as saved, it would compress the mic by its own level with settings tuned for ducking. `knobs-harness` confirms the swap is bit-identical.
- Filters that are off stay off. Settings the monitor ignores, such as mute and push-to-talk, get a note.

### Loading the mic

`obs_load_private_source()` loads the checked source. It's the private form of `obs_load_source()`: the same loader, but the source stays out of the global list and registers no hotkeys. It recreates the source, its filters in order with their settings, and its volume, balance, mute, Mono and sync offset. knobs turns the saved monitoring off first (`LoadSourceJson`), so no device opens during the load. Like OBS, it runs the `load` callbacks (`obs_source_load2`) for a mic from `sources`, but not for a global audio device.

The loader is OBS's on purpose. OBS loads the collection with it, from JSON the same OBS version wrote, so there's no hand-written replayer to drift from OBS. Replaying the settings by hand (`obs_source_create`, `obs_source_filter_add`) is only the fallback.

## The live path

### Audio without video

knobs never calls `obs_reset_video()`. Audio doesn't need it: capture callbacks run on the source's own thread, after the filters (`obs_source_output_audio`). Even a dummy canvas would cost about 70 MB of private memory and 70 threads, and its D3D11 device can wake a discrete GPU on a hybrid laptop.

The video tick drives more than video, though. Without it, sources never get their `activate` callback, which is where win-wasapi creates its reconnect thread. So a mic source never restarts by itself when its device is missing at load or goes away, or when a "default" mic's default device changes. Instead, the core rebuilds the source with OBS's loader when its devices come back (Devices and the watchdog). That re-runs OBS's own code, so fidelity is unaffected, and the filters start fresh, as when OBS starts.

### Monitoring into the cable

libobs's monitor (`audio-monitoring/win32/wasapi-output.c`) is an audio capture callback on the source. It takes the filtered audio, applies the source's volume, resamples it to the device's format, and writes it into a shared-mode WASAPI stream with a 1 s buffer. It bypasses libobs's audio thread and output mix, so OBS's audio buffering settings don't affect the cable.

The cable is the profile's monitoring device unless the user picks another. An OBS setup can use a second input of the same cable, such as VB-Cable's CABLE In 16ch, so knobs doesn't assume CABLE Input.

If the cable is missing when monitoring starts, libobs creates no monitor and never tries again (`audio_monitor_create`). If it goes away later, the monitor tries to reopen it on every packet, 100 times a second. So knobs runs the chain only while the cable is there.

### Mute and push-to-talk

From OBS 32.2.0, the monitor ignores mute ([PR #13378](https://github.com/obsproject/obs-studio/pull/13378)), so mute, push-to-talk and push-to-mute don't silence the cable, in OBS or in knobs. A disabled source counts as muted, so disabling it doesn't either, and the monitor ignores an audio-only source's sync offset. In earlier versions, mute or push-to-talk silenced the monitor, but knobs supports only 32.2 and later, and 33.0 keeps the new behavior. The first run warns about these settings.

## The always-on core

### States

The core is always in one state: starting, running, paused (by the user, or while OBS is open), needs setup, mic missing, cable missing, OBS missing, OBS unsupported, restart needed, or failed. After every event, the controller works out the state from what it knows, and the first match wins:

1. a problem with OBS, its settings or the import;
2. a chain that failed to start;
3. paused by the user;
4. paused while OBS is open;
5. the cable missing;
6. the mic missing.

The cable comes before the mic because a missing cable needs the user, while a mic usually comes back by itself. The chain runs only in the running state. Every other state releases it, which frees the mic and the cable and costs no CPU.

### Following OBS

knobs pauses while OBS runs, so the cable never gets the mic twice, and imports again when OBS exits. It pauses even when OBS doesn't monitor the mic, since OBS's saved settings lag behind what OBS is doing. Turning off Pause while OBS is open keeps it running.

A re-import reloads the chain only if the mic, the chain or the cable changed. Chains are compared without what doesn't reach the cable (`ImportedMic::chain_key`), such as mute, names and UUIDs, so renaming a filter in OBS leaves the chain running. An OBS update, or a new sample rate or channel layout, needs a restart, as in OBS.

`ObsWatch` sees OBS by two signals, in knobs's own Windows session only, so another user's OBS doesn't pause knobs:

- **OBS's named mutex.** OBS creates it before its window or any module, and holds it until it has saved its settings (`CheckIfAlreadyRunning`, `obs-main.cpp`). knobs checks for it every second, which takes 0.3 µs.
- **A process scan** for `obs64.exe`, every 2 s and when the mutex appears. It finds a portable OBS, whose mutex is named after its settings folder, and notes OBS in other sessions for the tray to report. The whole watch costs 0.07% of a core.

A one-second check is soon enough. On the test PC, OBS opened its monitor 1.4–2.4 s after creating its mutex, and knobs leaves the cable within 60 ms of seeing OBS, so in the worst case knobs is off the cable about 0.4 s before OBS starts sending. When OBS exits, knobs waits for its process to end, not just for its mutex.

### Devices and the watchdog

`DeviceWatch` hears devices come and go. Notifications come in bursts, such as while an interface powers up, so the devices are listed again once they've been quiet for 500 ms, or 2 s after the first. A mic or cable that goes away releases the chain, and the chain is rebuilt when both are back.

A watchdog catches what notifications miss, such as a device whose format changed. win-wasapi delivers silence as packets of zeros, so a working mic always sends packets. If the chain sees none for 3 s, it has stalled: the state is mic missing, and the chain is rebuilt after 2, 5, 15 and 30 s, then every 60 s. 30 s of audio starts the waits over.

### Keeping the delay down

libobs's monitor doesn't correct its delay for a mic. Its one correction, `process_audio_delay`, runs only for sources with video, and nothing upstream corrects clock drift. So the monitor's queue follows the difference between the mic's clock and the cable's, in OBS as in knobs:

- A backlog from a hiccup or a fast mic stays, and the delay creeps up until the 1 s buffer overflows. Then the monitor reopens its stream, losing what was queued.
- A slow mic drains the queue. When it's empty, Windows' audio engine plays a period of silence, and the delay steps back up.

So the core restarts the monitor (`obs_reset_audio_monitoring`, `Controller::FollowLevel`) at most every 10 minutes, once the output has been silent for 3 s. A restart opens a fresh stream, so the delay starts again as at any start, and leaves a gap of about one packet, in the silence. A mic 31.5 ppm faster than its cable (Clock drift) gains 19 ms in 10 minutes, two engine periods. Restarting more often buys little, since where a restart lands varies by about one period.

Silence is judged on the samples the monitor gets. Each second counts by its level with its loudest tenth left out, so a click or a breath doesn't spoil it. A second is silent at or below −50 dBFS, which a gate or expander reaches between words. A mic without one only gets down to its noise floor, so a second within 6 dB of the quietest in the last 5 minutes counts too, up to −30 dBFS. The last 200 ms before a restart must be silent as well, so a restart never cuts into a word. A mic that's never that quiet isn't restarted, since a gap in that much noise would be heard, and libobs's own reset at a full buffer is its backstop.

Restarts don't remove a slow mic's short dropouts, or delay steps downstream of the monitor, in Windows' audio engine or the cable. OBS has both too.

### One thread, testable

`core::Core` runs the controller and libobs on one thread, which pumps messages, as a COM single-threaded apartment must. Device notifications and the tray's commands are posted to it, and the tray hears each change of state through `core::Observer`. The controller has no threads or clock of its own: it drives a `Backend` interface (`ObsBackend` is the real one) and is told what happened and when, so the unit tests run it on a fake. knobs's own settings, such as a picked mic or cable, override what it imports and survive re-imports.

## The tray app

The tray app shows the core's state and sends it commands. The menu and notifications are Windows' own, and the setup window is knobs's own. `knobs-tray` runs it on a made-up OBS and devices, and takes pictures of every state.

### Status line and menu

The status line names the state and the chain in short form, such as `Mic/Aux › 4 filters › CABLE In 16ch` or `Mic missing: …`, counting only the filters that run. The icon's tooltip repeats it, sent with every change to the icon, since Windows 11 names the icon for screen readers from the last change, and the first run and notifications give the chain in full.

The menu is native (`TrackPopupMenuEx`): the status line, Pause/Resume, Mic ▸, Cable ▸, Re-import from OBS, Pause while OBS is open, Start with Windows, Automatically check for new versions, Automatically update, Setup…, Open log folder, About and Quit.

- When something needs the user, the item after the status line is the fix, in bold, such as Choose a cable…, or Find a supported OBS…, which opens the first run's page for an OBS knobs doesn't support.
- Mic ▸ and Cable ▸ start with "Same as OBS" when OBS has one mic or monitors to a cable. Any other pick is knobs's own and survives re-imports.
- Cables are recognized by name: VB-Audio's (VB-Cable, VoiceMeeter, Hi-Fi Cable) and Virtual Audio Cable. A VB-Audio cable's recording side follows from its name, so CABLE In 16ch records from CABLE Output.
- When a check has found a newer version, the item after the status lines offers it: Get knobs 0.3.0… opens its page, or once an update has downloaded, Update to knobs 0.3.0 installs it now (Updates).
- Menus follow the taskbar's dark mode, through uxtheme's `SetPreferredAppMode`.

Windows keeps a user's programs running when another user signs in, and the cable mixes every program's audio. So when OBS runs in another Windows account, knobs says under the status line that it may also send audio to the cable, and runs on.

### First run

The first run shows its pages in the setup window (below). It offers two doors, "I set up my mic in OBS" and "I'm new to OBS", each noting what knobs found, and the one that fits is the default.

- The first door asks only what it needs: anything that blocks the rest, such as OBS not found, then which mic, when there are several, which cable, when OBS doesn't monitor to a connected one, and the warnings that change what to expect. With no cable installed, it shows how to install VB-Cable, step by step, and moves on when one appears. With a cable that's turned off in Windows' Sound settings, it says how to turn it on, and opens the settings. With VB-Cable installed but none of its devices there, it says to restart Windows, and has Windows open knobs again after the restart, with `--resume-setup` in HKCU's RunOnce key, as the installer does (The installer and VB-Cable). The entry goes once the cable appears without a restart, the page moves on, or the first run is finished. If a copy is already running at sign-in, as one Start with Windows started, the copy the entry starts has it open the first run.
- The second door lists five steps in OBS, in OBS's own words, which are the same in 32.2.2 and 33.0, and suggests no filters or settings, since OBS is the editor. When OBS closes with filters on the mic, the first run carries on with the first door.
- The last page shows the chain, which device other apps should use and where the tray icon is, with Start with Windows checked. It also asks whether knobs should automatically check for new versions, and automatically update when one is out, which is only offered in a copy the installer installed and needs the check: without it, it's disabled, and keeps its state for when the check is on again. Both are off until the user turns them on.

A choice takes effect when the core's next snapshot carries it, and the page waits for that. knobs reopens an unfinished first run where it stopped, but not when Windows starts it at sign-in, unless it's after a restart the first run asked for: then it opens the first run even if it was finished since.

### The setup window

`SetupWindow` is a window of knobs's own, drawn with Direct2D and DirectWrite, which ship with Windows, so the tray stays plain Win32 with no UI framework. The pages stay data (`FirstRun`'s `PageView`, with the band's stages from `StageViews`), and the window lays out whichever page the data gives it (`setup_scene`). Its look is getknobs.app's graphite in Windows' own type.

```mermaid
flowchart TB
    band["Band: your mic › OBS's filters › virtual cable › your other apps"]
    panel["Panel: the page, its caret pointing up at the stage it's about"]
    foot["Foot: the page's note on the left, its buttons on the right"]
    band --> panel --> foot
```

- **The band** draws the mic's path as the site's How it works does: each stage an icon on a 1px wire, with its name and what knobs knows of it. A stage that's set, warns or stops knobs wears a badge on its icon's corner, in the tray icon's language: a green disc with a check, an amber triangle with "!", or the red disc with "!". The wire is ink as far as the signal has got, up to the stage the page is about.
- **The panel** holds the heading, the text, and the page's doors, choices, steps, notes or chain, and scrolls if they don't fit. Its caret points at the page's stage, and pages about no stage, such as Looking for OBS, have none.
- **Guides** are numbered steps with a picture beside them, and the step it shows has its number on an ink disc, as the picture's corner has.
  - Both guides have a walkthrough (`walkthrough.h`): real screenshots, with a camera that follows a drawn pointer, resting after each click so the user sees what it opened. Setting up a mic in OBS goes from OBS's main window to Mic/Aux in the Audio Mixer, Filters in its menu and **+** under Audio Filters, in 13.6 s. Installing VB-Cable goes from its driver pack in Downloads through **Extract All**, **Extract**, **Run as administrator** on `VBCABLE_Setup_x64` and **Install Driver**, in 19.1 s. Windows asks for admin on a desktop of its own, which can't be captured, so VB-Cable's window fades in where the prompt would be, and the step says to click **Yes**. The step a walkthrough is at lights up in the list. The scripts are data, and the unit tests check that the pointer rests on what it clicks, the steps light up in order, and the camera moves without jumps.
  - The screenshots are in `src/tray/pictures`, taken at twice the apps' size at 100%: OBS with `QT_SCALE_FACTOR=2`, and Windows at 200% in Windows Sandbox, where VB-Cable isn't installed. So the camera, in close to their own size, stays sharp at 200%. Each is cropped to what the camera sees of it, which the tests check too, so the sixteen take 940 KB, with VB-Cable's photographic banner as a JPEG. The pointer, the camera and the click's ring are drawn over them, so they stay sharp at any scale.
  - The frame is a toggle button: a click, Space, Enter or Alt+P plays and pauses it, and screen readers hear "Play the walkthrough" and its state. With Windows' animations off it holds still on the moment that matters most, the pointer on **Filters** in Mic/Aux's menu or on **Run as administrator**, both highlighted, until it's played. Playing, it waits where it is while the window is minimized, on another desktop, or behind the window the user is using, such as OBS, and only its frame is drawn again as it moves. Its screenshots are decoded when its page opens, not as they come in.
- **The look:** 760 by 520 DIPs. Windows' app mode picks the site's dark graphite or the same greys in light, and high contrast uses the user's colours only. Segoe UI Variable (Segoe UI on Windows 10), Consolas for file names, 4px controls and an 8px panel, hairlines on whole device pixels, and on Windows 11 a title bar in the window's own ground.
- **Motion** uses the site's one curve: the caret slides and the wire inks over 320 ms, a new page fades in over 200 ms, and a waiting page's dot breathes. A guide's walkthrough eases gently in and out instead, so the camera never whips. With Windows' animations off, nothing moves until the user plays a walkthrough.
- **Keyboard and screen readers.** Tab moves through the controls in reading order, the arrows through a group of choices, Enter clicks the focused button or the page's default, and Esc closes. Every button, door and the check box has an access key, marked "&" in the page's data and underlined while Alt is held, as in Windows' dialogs. UI Automation (`setup_uia`) gives each part of the page a provider, with the patterns for its controls: the stages as a list with their state, links, doors and buttons to invoke, radios to select, the check box to toggle, and what a page waits for as a live region. A new page announces its heading.
- **Scale.** It opens on the pointer's monitor at that monitor's scale, and Windows' text size scales the whole window, as far as the screen allows.

### Notifications and badges

Notifications are `Shell_NotifyIcon` balloons that say what happened, then what knobs does or what to do, and a click opens the fix. They cover a mic or cable missing for 5 s (so a power cycle stays quiet), a chain that stopped sending audio, a chain changed in OBS, a filter knobs can't run, an OBS update, OBS open in another Windows account, and any other problem that stops knobs. With the update check on, they also say when a new version is out and when knobs updated itself, or couldn't, and a click opens the version's page. While the first run is open, these wait for it to close. A normal start gets none, and neither does OBS opening and closing without changes. Problems found at start do, since a knobs started at sign-in would otherwise stop silently. A problem's notification is taken down when the problem ends.

Do Not Disturb hides notifications, so the icon shows the state too: a pause badge while paused, and a "!" badge while the cable gets no mic for a reason worth a notification. The badges are discs in the logo's colors, a white "!" on red (`#E5484D`) and white bars on graphite (`#4A4C52`), drawn at run time at 7/16 of the icon's size, with whole-pixel strokes so they stay crisp.

### Restarts, settings and startup

A restart the core needs, for an OBS update or a new audio format, happens at once, and the new copy says why. OBS asks first, because a restart would interrupt a stream, but restarting knobs costs only a moment of silence.

Settings live in `%AppData%\knobs\settings.ini`, written whole through a temporary file. Pause isn't saved, so knobs runs after a cold boot. A picked OBS install or settings folder that stops working stays picked: knobs shows the error and offers the way back, but changes nothing by itself.

Start with Windows is a value under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` that starts `knobs.exe --startup`. One copy runs per Windows session, holding the mutex `Local\knobs.tray`, and starting another opens the running one's menu, unless Windows or an installer started it.

### Portable mode

A `portable_mode.txt` file beside `knobs.exe`, as with OBS, makes knobs keep all of its state, the runtime copy included, in a `data` folder beside the exe (`AppDirs`). It still reads OBS's settings from `%AppData%`. The portable zip ships with the file, and the installer installs for the current user only, with no admin rights. A portable copy gets the update check, but not updating by itself, since it has no installer: it says where to get the new version.

### Updates

knobs makes no network connection until the user turns on the update check, on the first run's last page or in the menu. `Updater` (`src/tray/updates.h`) decides what's due, with no threads or clock of its own, as the controller does, so the unit tests run it on made-up times. The tray runs what it asks for on a thread of its own, which closes its WinHTTP handles to stop at once when knobs quits. A signature check can't be stopped, and may wait on servers it can't reach, so knobs waits a second for it, after letting go of the mic and the cable, then quits without it.

- **The check** asks GitHub for `github.com/danielalyoshin/knobs/releases/latest` with redirects off, and reads the version from where it redirects, `…/releases/tag/v0.3.0`: one request, with no API, no token and nothing about the PC. GitHub's latest release is never a draft or a pre-release. The first check waits a minute after knobs starts, since the network may not be up at sign-in, then they come once a day, and an hour after one that got no answer. The settings file keeps when the last one was answered and what it found, so a restart doesn't check again, and a version is news only once: a notification, and the menu's item, which opens the version's page.
- **Updating by itself** is only for a copy the installer installed: one whose folder is the `InstallLocation` of the installer's entry in HKCU's Uninstall key. It downloads the release's installer, as release.yml names it, into `%LocalAppData%\knobs\updates`, and checks its signature before keeping it, and again before it runs (`CheckReleaseSignature`). The signature must be one Windows trusts, revocation included, by a certificate whose common name is Daniel Alyoshin, chaining to Microsoft Identity Verification Root Certificate Authority 2020, with the extended key usage Azure Artifact Signing gives every certificate of this identity validation. Its certificates last three days, so no one certificate can be pinned, but that usage is the same on all of them, as in the v0.1.1 and v0.2.0 installers. A download that isn't signed so is never run, and the user hears that the update didn't happen. When Windows can't finish the check, as when it can't reach the servers that say whether a certificate was revoked, or another program has the file open, the download, or the installer kept for the next start, is tried again an hour later. A version whose download or install fails three times in a row, as one whose installer GitHub doesn't have, isn't tried again by itself, and the user hears that too, with its page to get it from. Downloads wait a minute after knobs starts, as checks do, but they don't wait for each other: a check that got no answer doesn't hold up a download, or the other way round.
- **Installing** restarts knobs, so it waits for a moment when knobs isn't sending the mic anyway: while OBS is open, the mic or cable is missing, or a problem stops knobs, or when knobs next starts with it downloaded: then the core waits, up to 5 s, for the signature check before it, so the mic doesn't start only to stop again. An update ready for a day with no such moment, as for someone who never opens OBS, gets a notification, once a version, whose click installs it. Not while the user has paused it, which a new copy wouldn't be, nor while the menu, the setup window or a dialog is open, nor once updating by itself is turned off, which knobs checks again after the signature. The menu's Update to knobs 0.3.0 installs it at once, or once a check or download that's running is done, and says why if it can't. knobs runs the installer very silent with `/update=1`, and quits. The installer closes any copy left, installs, and starts knobs again with `--updated` as it ends, whether the update worked or not, and the new copy says which. A version that was tried isn't tried again by itself.

`knobs-update` runs each part on its own, against GitHub: the check, a download with its signature check, and the signature check of any file.

### The installer and VB-Cable

With no virtual cable on the PC, the installer offers to install VB-Cable, VB-Audio's donationware cable, as a task that's checked. A virtual cable is recognized as the tray recognizes one, from Windows' list of playback devices in the registry, on or turned off. The installer carries VB-Audio's driver pack unchanged, which `package.ps1` downloads and checks against a fixed SHA-256, unzips it with Windows' own `tar`, and runs VB-Audio's own setup in its window, so the user sees it's VB-Audio's software: Windows asks to let it install its driver, and the user clicks Install Driver. VB-Audio allows this, as long as the user can tell VB-Cable is theirs, donationware, from www.vb-cable.com, which the tasks page and `NOTICE.txt` say. An update, or any other silent install, never runs it.

VB-Audio's setup says to restart Windows, but its devices don't wait for one: in Windows Sandbox, CABLE Input, CABLE In 16ch and CABLE Output were active within 5 s of the setup closing. So the installer waits up to 15 s for a cable, and then starts knobs as usual, whose first run finds it. If VB-Cable's driver is installed but no cable appears, it asks to restart Windows, and has knobs open after it to carry on, with a RunOnce entry; if the user restarts later, knobs starts now, and its first run says to restart. If the user declined Windows' prompt, or closed the setup without installing, it says VB-Cable wasn't installed, and knobs's first run shows how to install it by hand. Uninstalling knobs leaves VB-Cable, which other apps may use, and removes knobs's RunOnce entry if one is waiting. The portable zip doesn't carry it.

## Compatibility

knobs supports OBS 32.2.0 up to, but not including, 34.0.0 (`src/runtime/obs_version.h`). The vendored headers are 33.0's. Every export knobs requires has the same declaration back to 32.2.0, and the one 33.0 adds is optional, so the floor stays at 32.2. Patch releases don't change the libobs API. The ceiling is the next major, where libobs makes breaking changes. 32.2.2 and 33.0.0-rc1 are tested, and CI runs every test against both: the tests that need OBS take another OBS folder through `KNOBS_TEST_OBS_DIR`. When the tested versions change, the headers are re-vendored at the newest one's tag and the range is revisited.

### OBS 33.0

Checked against 33.0.0-rc1. Neither `win-wasapi` nor the audio filters changed from 32.2.2, and knobs's output is bit-identical to OBS 33's (Same output as OBS).

- **The modules moved** to folders of their own, `core\<module>`, with no `obs-plugins` folder. knobs looks for them where the version keeps them (Finding and copying OBS).
- **Monitoring is on or off.** `obs_source_set_monitoring_enabled` replaces `obs_source_set_monitoring_type`, which is deprecated and logs a warning on each call, so knobs uses the new export where it's there. Monitor Only is gone, and with it the shortcut that kept a monitored source out of libobs's output buffers (`source_output_audio_data`, `obs-source.c`). So libobs's audio thread processes the mic each tick, though nothing mixes it, since it's on no output channel. The monitor takes the audio before that, so the cable gets the same audio. Pushing audio through the chain and the monitor for 30 s cost 0.7–0.9% of a core on either version (`knobs-live --measure-output`, Release).
- **Sources saved by 33.0 store a `monitoring_enabled` bool**, which the loader reads instead of `monitoring_type`. knobs overrides both when loading (`LoadSourceJson`) and reads both on import.
- **Mute and push-to-talk still don't reach the monitor.** 33.0's new monitoring hotkeys don't apply, since knobs's private source registers none.
- **Advanced Audio Properties has an Enable check box for monitoring** in place of the list with Monitor Only. The settings the first run's steps name keep their names.

## Evidence

### Same output as OBS

`knobs-compare` runs the same input through the imported chain in knobs, offline, and in OBS itself: a private portable copy of the install, scripted to play the input as a Media Source carrying the mic's filters and record it to a 32-bit float WAV, with no audio device and without touching the user's OBS settings. knobs plays the input in OBS's packet size for a WAV, 4096 frames; other sizes change the output only at rounding level, about 120 dB below the signal. knobs's side also gets what OBS's recording mix does and the monitor doesn't: −0.0 becomes 0.0, NaN becomes 0, and samples are clamped to ±1.

knobs's output is bit-identical to OBS's on test signals and synthetic speech, for a real mic's chain (3-band EQ, expander, compressor and limiter, with Mono on), for every `obs-filters` audio filter in one chain, for a chain that clips, and for a 20.9 s recording of real voice. With OBS 33.0.0-rc1, it's bit-identical for the real mic's chain on the test signal. `knobs-harness` shows that knobs is deterministic too: its output is bit-identical across runs, processes, and Debug and Release builds.

### Mic-to-cable latency

`knobs-live --measure-mic` compares the mic with what arrives on the cable's recording side. With the same mic, an Audient iD4, into the same cable input, the mic reaches the cable in 88.1 ms in knobs and 87.7 ms in OBS.

The cable sets most of that, and libobs's monitor adds nothing measurable. From a packet's timestamp to the cable's recording side takes 84–94 ms through libobs's monitor and 81–98 ms through a bare WASAPI stream set up like it. Each run holds within ±0.2 ms, but runs vary with how the cable's buffers line up at the start, so only runs into the same cable input are compared.

### On real hardware

knobs.exe (Release) ran on one PC (Core Ultra 7 265K) with an Audient iD4, VB-Cable and OBS 32.2.2, running a four-filter chain (3-Band EQ, Expander, Compressor, Limiter). A watcher listed the audio sessions on the cable every 50 ms.

The idle footprint was measured over 60 s with nothing clicked. CPU is the change in processor time, and the cycle counts (`QueryProcessCycleTime`) are exact.

| | knobs running (OBS closed) | knobs paused (OBS open) | OBS minimized |
|---|---|---|---|
| CPU, one core | 0.72% | 0.44% | 5.64% |
| CPU by cycles | 1.02% | 0.39% | 7.52% |
| Working set | 36.2 MB | 30.7 MB | 468.9 MB |
| Private | 21.0 MB | 17.1 MB | 801.5 MB |
| Threads | 7–9 | 6–8 | 90–98 |
| Handles | 687 | 392 | 2205 |
| GPU | none | none | 0.05% |

- **Cold boot.** With nothing clicked, Windows started knobs 9.26 s after sign-in, and the chain was running 0.22 s later.
- **Making way for OBS.** Over six opens and closes of OBS, knobs and OBS were never active on the cable at once, and no echo was heard. knobs was off the cable 0.76–1.45 s before OBS started sending.
- **Losing the mic.** knobs listed the devices 0.51 s after the unplugged mic's stream failed, and stopped its own stream 13 ms later. The notification came about 5 s after the unplug. Plugged back in, the chain loaded 10 ms after the devices were listed.
- **Same sound.** In a voice app's mic test, knobs and OBS sounded the same, and the app read the level knobs sent within 0.1 dB.

### Clock drift

`knobs-live --measure-drift` times a device's clock against `QueryPerformanceCounter` over 10 minutes.

| Device | Against QPC | Per hour |
|---|---|---|
| Audient iD4 | −31.6 ppm | −114 ms |
| VB-Cable, CABLE In 16ch | −0.1 ppm | −0.5 ms |

VB-Cable runs on the system clock. The iD4's recording side measures the same as its playback side, so this mic delivers 31.5 ppm less audio than the cable plays. A push source run off real time (`knobs-live --measure-restart --push-ppm`) shows what that does:

- At the iD4's drift, the monitor's queue runs dry, and the cable gets a gap of 8–10 ms about every 5.3 minutes, in OBS too.
- A mic 31.5 ppm fast would gain 113 ms an hour and overflow the monitor's 1 s buffer after about 9 hours. Crystal clocks are commonly rated to ±20–50 ppm, so a fast mic is plausible, and restarting the monitor brings its delay back down (Keeping the delay down).

## Non-goals

- Video, recording, streaming or encoding.
- Editing filters. Users tune in OBS.
- VST filters. `obs-vst` drives its plugins through Qt, and knobs has no Qt application.
- NVIDIA noise removal, which is in `nv-filters`.
- DSP of its own, including following the cable's clock: resampling the mic would make the output no longer OBS's.
- More than one mic.
- Platforms other than Windows. OS-specific code stays behind thin seams, so a port stays possible.
- Shipping libobs.
