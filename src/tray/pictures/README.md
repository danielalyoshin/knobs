# The setup's screenshots

Real screenshots for the setup window's guides, built into knobs.exe and knobs-tray.exe as `RCDATA` resources (`knobs.rc`, `knobs-tray.rc`). A guide names its picture (`"obs-filters"` is the `OBS_FILTERS` resource) and the step it shows. The setup window draws a picture 264 DIPs wide if it's wider than 1.45 times its height, and 212 if not, so each is cropped to the part its step is about and taken at twice the app's size: it then shows at the app's own size, sharp at 200%.

- `obs-filters.png`: OBS 32.2.2's Filters window for a mic, at 2x: the foot of the empty Audio Filters list, with **+**, for step 3 of setting up a mic in OBS. 532 by 240 pixels.

## Taking OBS's

From a portable copy of OBS, such as the one `knobs-compare` makes in `%LocalAppData%\knobs\compare`, with settings of its own in its `config` folder, so `%AppData%\obs-studio` stays as it is. Its scene holds one Audio Input Capture on a device that doesn't exist, so OBS opens no mic: its log says the device failed to start, and Windows lists no mic use for the copy. Start `bin\64bit\obs64.exe --portable --multi --disable-updater --disable-missing-files-check` from `bin\64bit` with `QT_SCALE_FACTOR=2`, select the source and click **Filters** under the preview. Avoid menus at that scale: OBS 32.2.2 crashes opening the mixer's. The window can't be made shorter than its list, so crop below the list's top.

After changing a picture, touch the `.rc` files: the resource compiler doesn't notice a changed picture.
