# Logo files

knobs's icons and the installer's images, rendered from the logo's masters in `brand/`: the [knobs-brand](https://github.com/danielalyoshin/knobs-brand) repository, a git submodule. The masters, the brand's colors and the rules for using the logo are there, in `brand/BRAND.md`. Nothing here is drawn by hand: change the logo in knobs-brand, move the submodule, and render these again.

- `knobs.ico`: the app icon, the color mark with no tile: the graphite dial and the red sweep (`brand/logo/svg/knobs-mark-dark.svg`). It is knobs.exe's icon, as Explorer and Start show it, the installer's and the uninstaller's, and the windows' and notifications' icon on a light ground. It has the small and large icons at every display scale from 100% to 300%, and 256 px; its 96 px is also the notifications' icon at 100%.
- `knobs-on-dark.ico`: the color mark for dark grounds, the white dial (`knobs-mark-light.svg`), at the same sizes, for the windows and notifications knobs shows on a dark ground.
- `knobs-tray-white.ico` and `knobs-tray-black.ico`: the tray icon, the one-color mark, white on a dark taskbar and black on a light one (`knobs-mark-white.svg` and `knobs-mark-black.svg`), at the tray's sizes from 100% to 400% display scaling. At 16 and 20 px it's the knob alone (`knobs-knob-white.svg` and `knobs-knob-black.svg`), filling its square, since the sweep and its gap are under a pixel there.
- `installer/`: the installer's images, at each size Inno Setup asks for from 100% to 250% display scaling: `wizard-<width>.png`, the stacked lockup on the brand's light ground (`scheme.light.ground`) for the "Completing setup" page, and `header-<size>.png`, the horizontal lockup on a transparent ground, top right on the other pages.
- `brand-sources.txt`: what these were made from: the SHA-256 of each file of `brand/` used, with its line endings counted as LF, the favicon whose framing they share included, and each token used, with its value.
- `LICENSE`: the terms below.

## Making them

`tools\render-brand-assets.ps1` renders them all from `brand/`, with `tools\render-icon.ps1` and `tools\render-installer-images.ps1` (their help says how), and writes `brand-sources.txt`. The icons frame the mark as the brand's favicon does (the viewBox of `brand/web/favicon.svg`), so the mark fills its square, and the tray's knob alone at 16 and 20 px fills its own.

The `brand_assets` test (`tools/check-brand-assets.cmake`) compares `brand-sources.txt` with `brand/` as it is: when a file or token these were made from changes, it fails until they're rendered again. `packaging\package.ps1` gives the installer the ground recorded there, so its color always matches its images.

## License

These files aren't under the GPL like the rest of knobs: see [LICENSE](LICENSE) and [TRADEMARKS.md](../TRADEMARKS.md). A fork that you distribute needs its own in place of all of them.
