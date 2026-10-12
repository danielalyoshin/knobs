# The setup's screenshots

Real screenshots for the setup window's two walkthroughs, built into knobs.exe and knobs-tray.exe as `RCDATA` resources: `pictures.rc2` lists them, and `knobs.rc` and `knobs-tray.rc` both take it in. A picture's name, `"obs-main"`, is its resource's, `OBS_MAIN`.

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

## The VB-Cable walkthrough

Installing VB-Cable has a walkthrough too, eleven screenshots of Windows 11 at 200% on a 2560 by 1285 screen, 2560 by 1189 above the taskbar, which they leave out:

- `vb-downloads.png`, `vb-zip.png`: VB-Audio's driver pack (`VBCABLE_Driver_Pack45.zip`) in Downloads, then with the pointer on it.
- `vb-zip-menu.png`, `vb-extract-all.png`: its right-click menu, then with Extract All highlighted.
- `vb-extract-window.png`, `vb-extract.png`: the window that asks where to extract it, then with Extract highlighted.
- `vb-folder.png`, `vb-setup-file.png`: the extracted folder, then with the pointer on `VBCABLE_Setup_x64.exe`.
- `vb-setup-menu.png`, `vb-run-as-admin.png`: its right-click menu, then with Run as administrator highlighted.
- `vb-setup.jpg`: VB-Cable's setup, with Install Driver. A JPEG, at quality 92: its banner is a photo, which a PNG keeps at twice the size.

### Taking them

In Windows Sandbox, where VB-Cable isn't installed, so its setup offers Install Driver, not Remove Driver. A `.wsb` file maps the downloaded driver pack in read-only and a folder for the screenshots, turns networking off, and runs a PowerShell script at sign-in. The script copies the pack to Downloads, opens it in Explorer, maximizes each Explorer window as it opens, and saves the screen as a numbered PNG whenever it changes, a few times a second, with no pointer in it.

1. Set the monitor the sandbox will be on to 200% in Windows' Settings. The sandbox takes it from there, as its own Settings has no display scale.
2. Open the sandbox, maximize it, and start the script again if the scale changed after it started: the screen it records changes with the scale.
3. Do the guide's steps, resting on each menu item and button for a second so a frame catches it highlighted: right-click the pack, Extract All, Extract, right-click `VBCABLE_Setup_x64.exe`, Run as administrator, Yes, then rest on Install Driver.
4. Pick the frames: highlights can flicker between frames. Cut them above the taskbar, and crop as for OBS's.

Windows asks for admin on a desktop of its own, which the recording can't see, so there's no screenshot of that prompt.

After copying a picture in, touch it: a copy keeps its old time, and the build only compiles the resources again for a newer picture.
