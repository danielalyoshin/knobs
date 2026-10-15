# Product

<!-- impeccable:product-schema 1 -->

## Platform

windows

## Users

Windows users who want a processed mic (noise suppression, gate, expander, compressor, limiter) in every app they use it in. Two groups, both primary:

- **OBS users with a tuned chain.** Streamers, podcasters and gamers, many with an XLR mic, who already tuned a filter chain in OBS and route it to a virtual cable. Today they keep OBS open all day to do that. They know OBS and its filters.
- **People new to OBS.** People who install OBS only to tune a mic for knobs. They don't know OBS's filters or how to build a chain, so knobs has to explain that setup.

The job for both: tune the mic once, get that sound in every app, and stop thinking about it.

## Product Purpose

knobs runs the mic filter chain from the user's OBS settings and sends the result to a virtual audio cable, with no OBS process. The audio goes through the user's own installed OBS filter code.

Success (measured in docs/design.md, Evidence): with OBS closed and knobs in the tray, the mic matches OBS's output wherever that output is repeatable (Evidence on Hand). A cold boot gives a working filtered mic with zero clicks, opening OBS while knobs runs never doubles the audio, and idle use is well below OBS minimized.

## Positioning

knobs gives OBS's mic sound without OBS. It loads the filter binaries from the user's own OBS install and recreates the mic with OBS's own loader. On test signals and real voice, its output is bit-identical to OBS's, apart from NVIDIA's noise removal and stereo VST plugins on a mono profile, which vary in OBS too. Equalizer APO, VoiceMeeter, NVIDIA Broadcast and NoiseTorch use different DSP. OBS plugins such as atkAudio still need OBS open.

## Operating Context

- Lives in the Windows tray with no window by default, and starts with Windows.
- Needs OBS Studio installed (32.2 to 33.x) and a virtual cable such as VB-Cable. Other apps pick the cable as their mic.
- Users tune filters in OBS. knobs reads the active OBS profile and scene collection and never changes them.
- Pauses while OBS runs and resumes when it exits, so the cable never gets the mic twice.
- Found through GitHub, the OBS forums and r/obs.

## Capabilities and Constraints

- Built: loading libobs from a private copy of the OBS install, importing the mic and its filters, VST filters and NVIDIA's noise removal through OBS's own modules, the live path into the cable, the comparison with OBS, the tray app with its first run, notifications and icon badges, and an update check that's off until the user turns it on. Released on GitHub; open work is tracked in GitHub issues.
- No custom DSP and no "improvements". No filter editing in v1, since users tune in OBS, and no VST plugin windows. Windows only in v1.
- Plain Win32 UI with no UI framework. A small exe that ships no libobs. Target: under 1% CPU and a small memory footprint.
- Terms: mic, filter chain, virtual cable (or cable), OBS profile, scene collection, monitoring device.
- The tray menu, first run and tray icon are designed in docs/design.md (The tray app).
- Distribution: an installer (for the current user, no admin rights) and a portable zip that keeps its data beside the exe, from GitHub releases. With no virtual cable on the PC, the installer offers VB-Cable, running VB-Audio's own setup. Installed copies can update themselves, once the user turns that on. Releases are signed with Azure Artifact Signing. SmartScreen may still warn on first run until a new release has built up reputation.

## Brand Commitments

- **Name:** "knobs", all lowercase, even at the start of a sentence, as in the audio kind. It was knOBS until 2026-10-05. Code keeps it in one constant (`KNOBS_DISPLAY_NAME`), and user-facing strings use that constant.
- **Tagline:** "Your OBS mic chain, in every app.", the headline of getknobs.app. The README, the About box and the GitHub repository's description use it (decided 2026-10-08, replacing "Your OBS mic chain, without OBS.").
- **Voice:** plain and exact, like the README. Say what happened and what to do, in everyday words. Short sentences, no hype, no exclamation marks, no jokes. Give numbers when they help ("88 ms").
- **Brand:** the knobs-brand repository, a git submodule at `brand/`, holds the brand's rules (`brand/BRAND.md`), the logo's masters and the design tokens, for the app and getknobs.app alike. The app's UI is drawn from those tokens, and its icons and installer images are rendered from those masters, so a change there reaches the app in its next release. The app keeps Windows' own type rather than the brand's (decided 2026-10-14).
- **Logo:** the knob mark (a knob turned to its top position, with the red sweep it has swept through, original artwork), the wordmark and their lockups, in `brand/logo/`. Don't recolor, rotate or stretch it. The app icon is the mark alone, with no tile (decided 2026-10-05, kept 2026-10-15): the graphite dial with the red sweep as the exe's and the installer's icon (decided 2026-10-15), and the white dial where knobs shows its icon on a dark ground itself, in its windows and notifications.
- **Colors:** graphite `#191b1d`, ink `#e5ecf6`, and cap red `#df695e` (`#c55149` on light grounds), which marks what can be pressed and what is knobs and never an error. No cream.
- **Red "obs" in the wordmark:** kept on purpose (decided 2026-10-05), although the rename to knobs dropped "OBS" from the name. If the OBS team objects, revisit it, as with the name.
- **State marks and tray badges:** the brand's state marks, cut square as the logo is: a green disc with a check, a red disc with "!", an amber triangle with "!", and a graphite disc with "i" or pause bars, which turns over to white on a dark ground (decided 2026-10-14 and 2026-10-15). The tray's badges are the red "!" and the graphite pause, 7/16 of the icon. The tray icon is the one-colour mark, white on a dark taskbar and black on a light one (decided 2026-10-14), and the knob alone at 16 and 20 px, where the sweep would smear (decided 2026-10-15).
- **Logo license:** the logo files in knobs-brand's `logo/` and `web/`, and the icons and images rendered from them in `assets/`, are all rights reserved, outside the GPL that covers the code. `TRADEMARKS.md` says how the name and logo may be used; forks need their own (decided 2026-10-07).
- **Independence:** knobs isn't affiliated with or endorsed by the OBS Project. Say so wherever knobs is presented.

## Evidence on Hand

- knobs's output is bit-identical to OBS's on test signals, on VST plugins and on a recording of real voice (docs/design.md, Same output as OBS), apart from a stereo VST plugin on a mono profile, which isn't repeatable in OBS either. NVIDIA's noise removal is close to OBS's, but its output depends on when the mic started, in OBS too (docs/design.md, NVIDIA's noise removal).
- Mic to cable: 88.1 ms in knobs, 87.7 ms in OBS, on the same cable input (Audient iD4; docs/design.md, Mic-to-cable latency).
- On real hardware (docs/design.md, On real hardware): running, knobs used 0.9% of a core and 27 MB, against OBS minimized at 7.1% and 443–448 MB. After a cold boot, the filtered mic was on the cable 9.5 s after signing in, with nothing clicked.
- [obs-studio#12650](https://github.com/obsproject/obs-studio/issues/12650): since OBS 32.0 the Safe Mode prompt blocks OBS from running unattended as a background mic processor, which is this product's use case.
- The logo, its lockups and the brand's rules in `brand/` (knobs-brand), and the icons rendered from them in `assets/`.
- Pictures of the tray menu, first run, notifications and icon: `knobs-tray --screenshot` and `--icons` take them on its fake core, whose state is made up but realistic.
- Not on hand, and not to be made up: users, testimonials, download counts, a website.

## Product Principles

1. **Fidelity is the product.** Audio passes through OBS's own code, including the VST plugins and NVIDIA runtime it hosts. Nothing in knobs changes the audio, and the app never offers to "enhance" it.
2. **Invisible while it works.** Tray only, starts with Windows, nothing to look at once it's set up.
3. **Loud when it breaks.** When the mic, the cable or OBS goes missing, say so in a notification that says what to do. Never fail silently.
4. **OBS is the editor.** Tuning happens in OBS. knobs imports the chain, and for people new to OBS it explains how to set one up there rather than building one itself.
5. **Leave OBS alone.** Never change OBS's settings or install. Make way while OBS runs instead of competing with it.
