# Logo files

knobs's icons and the installer's images, rendered from the logo's masters in `brand/`: the [knobs-brand](https://github.com/danielalyoshin/knobs-brand) repository, a git submodule. The masters, the brand's colors and the rules for using the logo are there, in `brand/BRAND.md`. Nothing here is drawn by hand: change the logo in knobs-brand, move the submodule, and render these again.

- `knobs.ico`: the app icon, the color mark with no tile: the graphite dial and the red sweep (`brand/logo/svg/knobs-mark-dark.svg`). It is knobs.exe's icon, as Explorer and Start show it, the installer's and the uninstaller's, and the windows' and notifications' icon on a light ground. It has the small (16 px) and large (32 px) icons at every display scale from 100% to 300%: 16, 20, 24, 28, 32, 36, 40, 48, 56, 64, 72, 80 and 96 px, and 256. 96 is also the notifications' icon at 100%.
- `knobs-on-dark.ico`: the color mark for dark grounds, the white dial (`knobs-mark-light.svg`), at the same sizes, for the windows and notifications knobs shows on a dark ground.
- `knobs-tray-white.ico` and `knobs-tray-black.ico`: the tray icon, the one-color mark, white on a dark taskbar and black on a light one (`knobs-mark-white.svg` and `knobs-mark-black.svg`). At 16 and 20 px it's the knob alone (`knobs-knob-white.svg` and `knobs-knob-black.svg`), filling its square, since the sweep and its gap are under a pixel there. Each has the tray's small icon at display scales from 100% to 400%: 16, 20, 24, 28, 32, 36, 40, 48, 56 and 64 px.
- `installer/`: the installer's images, at each size Inno Setup asks for from 100% to 250% display scaling. `wizard-<width>.png` is the stacked lockup with dark ink, centered on the brand's light ground (`scheme.light.ground`), for the "Completing setup" page; the installer fills the rest of that area with the same ground. `header-<size>.png` is the horizontal lockup with dark ink on a transparent ground, top right on the other pages: 32 px tall at 100%, centered on the 58 px header, with a 24 px margin on its right, the page title's inset on the left. The uninstaller shows `knobs.ico` there, as tall as the lockup and with the same margin.
- `brand-sources.txt`: what these were made from. It has the SHA-256 of each master used, with its line endings counted as LF, and each token used, with its value.

## Making them

`tools\render-brand-assets.ps1` renders them all from `brand/`, with `tools\render-icon.ps1` and `tools\render-installer-images.ps1`, and writes `brand-sources.txt`. Headless Edge draws each SVG at 8 times each size, once on black and once on white, and each 8×8 block is averaged into one pixel, with its alpha from the two pictures. Each icon frames the mark as the brand's favicon does (the viewBox of `brand/web/favicon.svg`), so the mark fills its square. Icon sizes below 256 are 32-bit bitmaps, and 256 is a PNG.

The `brand_assets` test (`tools/check-brand-assets.cmake`) compares `brand-sources.txt` with `brand/` as it is. When the submodule moves on to a knobs-brand with a different logo or ground, the tests fail until these are rendered again. `packaging\package.ps1` gives the installer the ground recorded there, so its color always matches its images.

## License

These files aren't under the GPL like the rest of knobs. They're Copyright © 2026 Daniel Alyoshin, all rights reserved, apart from what [TRADEMARKS.md](../TRADEMARKS.md) allows: sharing them unmodified with the knobs source and its official builds, and using them unmodified to refer to knobs. A fork that you distribute needs icons of its own in place of `knobs.ico` and the tray icons.
