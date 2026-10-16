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

knobs runs the mic filter chain from the user's OBS settings and sends the result to a virtual cable, with no OBS process. The audio goes through the user's own installed OBS filter code.

Success (measured in docs/design.md, Evidence): with OBS closed and knobs in the tray, the mic sounds as it does in OBS: its output is bit-identical to OBS's, apart from two cases that vary in OBS too (Evidence on Hand). A cold boot gives a working filtered mic with zero clicks, opening OBS while knobs runs never doubles the audio, and idle use is well below OBS minimized.

## Positioning

knobs gives OBS's mic sound without OBS. It loads the filter binaries from a private copy of the user's own OBS install and recreates the mic with OBS's own loader. On test signals and real voice, its output is bit-identical to OBS's, apart from NVIDIA's noise removal and stereo VST plugins on a mono profile, which vary in OBS too. Equalizer APO, VoiceMeeter, NVIDIA Broadcast and NoiseTorch use different DSP. OBS plugins such as atkAudio still need OBS open.

## Operating Context

- Lives in the Windows tray with no window by default, and starts with Windows.
- Needs OBS Studio installed (32.2 to 33.x) and a virtual cable such as VB-CABLE. Other apps choose the cable as their mic.
- Users tune filters in OBS. knobs reads the active OBS profile and scene collection and never changes them.
- Keeps running while OBS is open, so users who stream through OBS keep their filtered mic in their other apps. By default it pauses while OBS runs only when OBS's Audio Monitoring also plays the mic into its cable, so the cable never gets the mic twice, and reads OBS's settings again when OBS exits.
- Found through GitHub and getknobs.app; to be announced on the OBS forums and r/obs.

## Capabilities and Constraints

- Built: loading libobs from a private copy of the OBS install, importing the mic and its filters, VST filters and NVIDIA's noise removal through OBS's own modules, the live path into the cable, the comparison with OBS, the tray app with its setup window, notifications and icon badges, and an update check that's off until the user turns it on. Released on GitHub; open work is tracked in GitHub issues.
- No custom DSP and no "improvements". No filter editing, since users tune in OBS, and no VST plugin windows. Windows only for now.
- Plain Win32 UI with no UI framework. A small exe that ships no libobs. Target: under 1% CPU and a small memory footprint.
- Terms: mic, filter chain, virtual cable (or cable), OBS profile, scene collection, monitoring device (OBS's Monitoring Device in user-facing text), setup window, VB-CABLE.
- The tray menu, setup window and tray icon are designed in docs/design.md (The tray app), and the setup window's look in DESIGN.md.
- Distribution: an installer (for the current user, no admin rights) and a portable zip that keeps its data beside the exe, from GitHub releases. With no virtual cable on the PC, the installer offers VB-CABLE, running VB-Audio's own installer. Installed copies can update themselves, once the user turns that on. Releases are signed with Azure Artifact Signing. SmartScreen may still warn the first time a new release runs, until it has built up reputation.

## Brand Commitments

The brand's rules (logo, colours, state marks, type, geometry, motion and voice) are in knobs-brand's `brand/BRAND.md`, a git submodule shared with getknobs.app, and the setup window's look in DESIGN.md, drawn from the brand's tokens. These are the commitments for the app:

- **Name:** "knobs", all lowercase, even at the start of a sentence, as in the audio kind. Code keeps it in one constant (`KNOBS_DISPLAY_NAME`), and user-facing strings use that constant.
- **Tagline:** as BRAND.md's Voice gives it, word for word, in the README, the About box and the GitHub repository's description.
- **Voice:** plain and exact, like the README. Say what happened and what to do, in everyday words. Short sentences, no hype, no exclamation marks, no jokes. Give numbers when they help ("88 ms"). The rest is in BRAND.md's Voice.
- **Look:** the app's UI is drawn from the brand's tokens, and its icons and installer images are rendered from the brand's logo files (`assets/`), so a change in knobs-brand reaches the app in its next release. The app keeps Windows' own type rather than the brand's. Cap red marks what can be pressed and what is knobs, never an error.
- **Logo:** the knob mark, the wordmark with its red "obs", and their lockups, used only as BRAND.md says. The Windows icon is the mark alone, with no tile, and the tray icon is the one-colour mark following the taskbar.
- **Logo licence:** the logo files in knobs-brand's `logo/` and `web/`, and the icons and images rendered from them in `assets/`, are all rights reserved, outside the GPL that covers the code. `TRADEMARKS.md` says how the name and logo may be used; forks need their own.
- **Independence:** wherever knobs is presented, say: "knobs is an independent project, not affiliated with the OBS Project."

## Evidence on Hand

- knobs's output is bit-identical to OBS's on test signals, on VST plugins and on a recording of real voice (docs/design.md, Same output as OBS), apart from a stereo VST plugin on a mono profile, which isn't repeatable in OBS either. NVIDIA's noise removal is close to OBS's, but its output depends on when the mic started, in OBS too (docs/design.md, NVIDIA's noise removal).
- Mic to cable: 90.0 ms in knobs, 91.3 ms in OBS, each the average of three runs on the same cable input (Audient iD4; docs/design.md, Mic-to-cable latency).
- On real hardware (docs/design.md, On real hardware): running, knobs used 0.92–0.96% of a core and 27 MB, against OBS minimized at 7.06–7.10% and 443–448 MB. After a cold boot, the filtered mic was on the cable 9.5 s after signing in, with nothing clicked.
- [obs-studio#12650](https://github.com/obsproject/obs-studio/issues/12650): since OBS 32.0 the Safe Mode prompt blocks OBS from running unattended as a background mic processor, which is this product's use case.
- The logo, its lockups and the brand's rules in `brand/` (knobs-brand), and the icons rendered from them in `assets/`.
- Pictures of the tray menu, setup window, notifications and icon: `knobs-tray --screenshot` and `--icons` take them on its fake core, whose state is made up but realistic.
- Not on hand, and not to be made up: users, testimonials, download counts.

## Product Principles

1. **Fidelity is the product.** The audio goes only through OBS's own filters, and the VST plugins and NVIDIA's noise removal OBS runs. Nothing in knobs changes the audio, and the app never offers to "enhance" it.
2. **Invisible while it works.** Tray only, starts with Windows, nothing to look at once it's set up.
3. **Loud when it breaks.** When the mic, the cable or OBS goes missing, say so in a notification that says what to do. Never fail silently.
4. **OBS is the editor.** Tuning happens in OBS. knobs imports the chain, and for people new to OBS it explains how to set one up there rather than building one itself.
5. **Leave OBS alone.** Never change OBS's settings or install. Where OBS sends the mic to the cable itself, make way instead of competing with it.
