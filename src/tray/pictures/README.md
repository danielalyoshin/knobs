# The setup's screenshots

Real screenshots for the setup window's guides, built into knobs.exe and knobs-tray.exe as `RCDATA` resources: `pictures.rc2` lists them, and `knobs.rc` and `knobs-tray.rc` both take it in. A picture's name, `"obs-main"`, is its resource's, `OBS_MAIN`.

## The OBS walkthrough

Setting up a mic in OBS has a walkthrough (`walkthrough.cpp`), five screenshots of OBS 32.2.2 at twice its size, maximized on a 2560 by 1440 screen whose taskbar leaves 2560 by 1392:

- `obs-main.png`: the main window, whole.
- `obs-mixer-menu.png`: Mic/Aux's menu open in the Audio Mixer.
- `obs-mixer-filters.png`: the same, with Filters highlighted.
- `obs-filters-window.png`: the Filters window over the main window, its Audio Filters list empty.
- `obs-add-filter.png`: the menu that + under Audio Filters opens.

All but the first are cropped to what the camera sees of them, and `walkthrough.cpp` has each crop, in the whole screen's pixels. A unit test checks that the camera never looks past a crop (`WalkthroughCameraStaysInsideTheShots`), so a change to the walkthrough's timing or zoom may need a wider crop: find what the camera sees of a shot from `WalkAt` and `WalkView`, and crop the screenshot again with 16 pixels to spare.

### Taking them

From a portable copy of OBS, such as the one `knobs-compare` makes in `%LocalAppData%\knobs\compare`, with settings of its own in its `config` folder, so `%AppData%\obs-studio` stays as it is. Its scene collection has no sources and one Mic/Aux in Settings > Audio (`AuxAudioDevice1`), an Audio Input Capture on a device that doesn't exist, with no filters, so OBS opens no mic: its log says the device failed to start, and Windows lists no mic use for the copy.

1. Start `bin\64bit\obs64.exe --portable --multi --disable-updater --disable-missing-files-check` from `bin\64bit` with `QT_SCALE_FACTOR=2`, on a 100% screen, and maximize it there. OBS must fit the screen: OBS 32.2.2 crashes opening the mixer's menu if Mic/Aux's button is off it (`VolumeControl::showVolumeControlMenu` looks up the screen under it).
2. The main window.
3. Click Mic/Aux's name in the Audio Mixer (UI Automation's Invoke works): its menu.
4. Up twice from the bottom: Filters highlighted.
5. Click Filters, and size the Filters window to 1400 by 980 pixels, centred: its default size is taller than the screen at 2x.
6. Click + under Audio Filters with the mouse, since its menu opens where the pointer is, and capture it. Esc closes it.

Capture the screen, not the window, so the menus show, and with no other window over OBS. The pointer is drawn by the setup window, so it doesn't matter where the real one is, except over a control it lights up.

After copying a picture in, touch it: a copy keeps its old time, and the build only compiles the resources again for a newer picture.
