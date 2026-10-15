---
name: knobs
description: knobs's own UI, the setup window, in getknobs.app's graphite and Windows' own type.
colors:
  paper: "#f7f8fa"
  paper-subtle: "#eef0f3"
  surface: "#ffffff"
  sunken: "#f0f1f4"
  hairline: "#e1e3e8"
  hairline-strong: "#c8ccd3"
  ink: "#17181b"
  ink-hover: "#34363c"
  on-ink: "#ffffff"
  graphite-2: "#4a4e57"
  graphite-3: "#686d77"
  paper-dark: "#0f1012"
  paper-subtle-dark: "#141518"
  surface-dark: "#17181b"
  sunken-dark: "#121316"
  hairline-dark: "#26282d"
  hairline-strong-dark: "#363940"
  ink-dark: "#e8eaee"
  ink-hover-dark: "#ffffff"
  on-ink-dark: "#17181b"
  graphite-2-dark: "#b0b4bc"
  graphite-3-dark: "#8b8f98"
  mark-ok: "#23875a"
  mark-stop: "#e5484d"
  mark-warn: "#f0a33c"
  mark-info: "#4a4c52"
  mark-glyph: "#ffffff"
  mark-glyph-on-warn: "#17181b"
typography:
  display:
    fontFamily: "Segoe UI Variable Display, Segoe UI, sans-serif"
    fontSize: "22px"
    fontWeight: 600
    lineHeight: "28px"
  title:
    fontFamily: "Segoe UI Variable Text, Segoe UI, sans-serif"
    fontSize: "14px"
    fontWeight: 600
    lineHeight: "20px"
  body:
    fontFamily: "Segoe UI Variable Text, Segoe UI, sans-serif"
    fontSize: "14px"
    fontWeight: 400
    lineHeight: "21px"
  button:
    fontFamily: "Segoe UI Variable Text, Segoe UI, sans-serif"
    fontSize: "14px"
    fontWeight: 400
    lineHeight: "20px"
  body-sm:
    fontFamily: "Segoe UI Variable Text, Segoe UI, sans-serif"
    fontSize: "13px"
    fontWeight: 400
    lineHeight: "18px"
  chain:
    fontFamily: "Segoe UI Variable Text, Segoe UI, sans-serif"
    fontSize: "13px"
    fontWeight: 400
    lineHeight: "20px"
  label:
    fontFamily: "Segoe UI Variable Text, Segoe UI, sans-serif"
    fontSize: "12px"
    fontWeight: 400
    lineHeight: "17px"
  numeral:
    fontFamily: "Segoe UI Variable Text, Segoe UI, sans-serif"
    fontSize: "14px"
    fontWeight: 600
    lineHeight: "21px"
  disc-numeral:
    fontFamily: "Segoe UI Variable Text, Segoe UI, sans-serif"
    fontSize: "12px"
    fontWeight: 600
    lineHeight: "20px"
  mono:
    fontFamily: "Consolas, monospace"
    fontSize: "1em"
    fontWeight: 400
rounded:
  control: "4px"
  panel: "8px"
  round: "50%"
spacing:
  step: "5px"
  sm: "8px"
  md: "12px"
  lg: "16px"
  xl: "20px"
  gutter: "24px"
  control: "32px"
  band: "104px"
components:
  band:
    backgroundColor: "{colors.paper-subtle}"
    textColor: "{colors.graphite-2}"
    height: "{spacing.band}"
  panel:
    backgroundColor: "{colors.surface}"
    textColor: "{colors.ink}"
    rounded: "{rounded.panel}"
    padding: "20px 24px 17px"
  button-primary:
    backgroundColor: "{colors.ink}"
    textColor: "{colors.on-ink}"
    typography: "{typography.title}"
    rounded: "{rounded.control}"
    padding: "0 16px"
    height: "{spacing.control}"
  button-primary-hover:
    backgroundColor: "{colors.ink-hover}"
  button-primary-pressed:
    backgroundColor: "{colors.graphite-2}"
  button-secondary:
    backgroundColor: "{colors.surface}"
    textColor: "{colors.ink}"
    typography: "{typography.button}"
    rounded: "{rounded.control}"
    padding: "0 16px"
    height: "{spacing.control}"
  button-secondary-hover:
    backgroundColor: "{colors.paper-subtle}"
  button-secondary-pressed:
    backgroundColor: "{colors.sunken}"
  button-disabled:
    backgroundColor: "{colors.paper}"
    textColor: "{colors.graphite-3}"
  door:
    backgroundColor: "{colors.surface}"
    textColor: "{colors.ink}"
    typography: "{typography.title}"
    rounded: "{rounded.control}"
    padding: "11px 16px"
  door-hover:
    backgroundColor: "{colors.paper-subtle}"
  door-pressed:
    backgroundColor: "{colors.sunken}"
  choice-group:
    backgroundColor: "{colors.surface}"
    textColor: "{colors.ink}"
    typography: "{typography.title}"
    rounded: "{rounded.control}"
    padding: "10px 16px 10px 44px"
  chain-well:
    backgroundColor: "{colors.sunken}"
    textColor: "{colors.graphite-2}"
    typography: "{typography.chain}"
    rounded: "{rounded.control}"
    padding: "7px 12px"
  checkbox-checked:
    backgroundColor: "{colors.ink}"
    textColor: "{colors.on-ink}"
    rounded: "{rounded.control}"
    size: "18px"
  step-disc:
    backgroundColor: "{colors.ink}"
    textColor: "{colors.on-ink}"
    typography: "{typography.disc-numeral}"
    rounded: "{rounded.round}"
    size: "20px"
---

# Design System: knobs

## Overview

**Creative North Star: "The Mic's Path"**

knobs's own UI is one window, the setup window, and it is about one thing: where the mic goes. Every page sits under a band that draws the path (your mic › OBS's filters › virtual cable › your other apps), and the panel below answers the one question for the stage its caret points at. The look is getknobs.app's graphite carried into a Windows window: the site's greys, its ink as the only action colour, its hairlines and its stroked icons, set in Windows' own type with Windows' own control shapes, so the window reads as a native app that belongs to the site.

It is calm and dense in the way a settings window is: a fixed 760 × 520 DIP client area, one panel, one heading per page, plain sentences in the product's voice, and nothing decorative. Colour other than graphite appears only as state marks, with the tray badge's proportions and clear ring. Light and dark follow Windows' app mode; high contrast drops the palette entirely and uses the user's colours.

Motion is structural and short: the wire inks and the caret slides as the signal reaches a stage, a page fades in, and a waiting page's dot breathes. The one long motion is a guide's walkthrough, which shows rather than decorates, and pauses at a click. With Windows' animations off, nothing moves until the user plays a walkthrough.

**Key Characteristics:**
- The path is always on screen: a band of four stages, each an icon on a 1px wire, with its name and what knobs knows of it.
- Ink is the only action colour; everything else is graphite and hairline.
- State is shown as marks only: the setup window's green check, amber triangle and red "!" disc. The tray's badges are a red "!" disc and white pause bars on a graphite disc.
- Segoe UI Variable with Consolas for file names, 4px controls and one 8px panel.
- Hairlines on whole device pixels, flat surfaces, one 1-pixel lift under the panel.

## Colors

A cool graphite ramp in two schemes, one ink for action, and the tray's red and graphite plus green and amber for setup's state marks.

### Primary
- **Graphite Ink** (#17181b light, #e8eaee dark): the one action colour. The default button's fill, the selected radio, the checked box, the step discs, the caret, the stage the page is about, the wire as far as the signal has got, and names inside secondary text. It is also the logo's ink.
- **Ink Hover** (#34363c light, #ffffff dark): the default button under the pointer.
- **On Ink** (#ffffff light, #17181b dark): text and glyphs on an ink fill.

### Neutral
- **Paper** (#f7f8fa light, #0f1012 dark): the window's ground, the foot, and the Windows 11 title bar.
- **Paper Subtle** (#eef0f3 light, #141518 dark): the band's ground, and the hover fill of secondary buttons and doors.
- **Surface** (#ffffff light, #17181b dark): the panel, buttons, doors and the box of choices.
- **Sunken** (#f0f1f4 light, #121316 dark): the chain's well, a pressed control, and a picture's frame before it loads.
- **Hairline** (#e1e3e8 light, #26282d dark): the band's bottom rule, rules between choices, the well's border, and the scroll edges.
- **Hairline Strong** (#c8ccd3 light, #363940 dark): the panel's border, button, door and group borders, a picture's frame.
- **Graphite 2** (#4a4e57 light, #b0b4bc dark): lead paragraphs, notes under door and choice titles, stage names and values once known, the chain's filters, the waiting status.
- **Graphite 3** (#686d77 light, #8b8f98 dark): stages not yet known, the unlit wire, step numerals, the foot's note, a link's underline at rest, control edges, and disabled text.

### Tertiary (state marks)
- **Mark Green** (#23875a): the "set" badge, a disc with a white check. The same in both schemes.
- **Mark Red** (#e5484d): the "needs you" badge, a disc with a white "!". It is the logo's dark-scheme red and the tray's attention badge.
- **Mark Amber** (#f0a33c): the warning triangle, with a graphite "!" (#17181b).
- **Mark Graphite** (#4a4c52): the information disc beside a note, with a white "i". It is the tray's pause badge colour.

### Named Rules
**The One Ink Rule.** Ink is the only colour that means "act" or "here". No accent hue exists; the default button, the current stage and the live wire all use the same ink.

**The Marks Only Rule.** Green, amber and red appear only in state marks. They never colour text, borders, fills or the wire.

**The User's Colours Rule.** In high contrast every ground is the system's window colour and every line, text and badge is its window-text colour; links take the hot-light colour, disabled text gray-text, and the current stage the highlight colour. Shapes and words carry the meaning, and the panel has no lift.

## Typography

**Display Font:** Segoe UI Variable Display (with Segoe UI on Windows 10)
**Body Font:** Segoe UI Variable Text (with Segoe UI on Windows 10)
**Label/Mono Font:** Consolas, inline only

**Character:** Windows' own type, so the window sits with the rest of the system; the brand comes from colour, line and icon, not the typeface. Lines are uniformly spaced with the font's ascent and descent centred in the line, so every line of a paragraph shares a baseline rhythm.

### Hierarchy
- **Display** (600, 22/28): the page's heading, one per page, at the top of the panel.
- **Title** (600, 14/20): stage names in the band, door and choice titles, the default button's label.
- **Body** (400, 14/21): the page's text, steps and notes. The first paragraph is the lead, in Graphite 2; the rest say what to do, in ink. Under the chain the order flips.
- **Button** (400, 14/20): secondary button labels and the check box's label.
- **Body Small** (400, 13/18): the note under a door's or choice's title, and what a waiting page waits for.
- **Chain** (13/20): the chain in its well; the mic and the cable at 600 in ink, the filters at 400 in Graphite 2.
- **Label** (400, 12/17): a stage's value in the band, and the foot's note.
- **Numeral** (600, 14/21): step numbers, centred, in Graphite 3. **Disc numeral** (600, 12/20) on an ink disc, the glyphs centred on their ink.
- **Mono** (Consolas, the surrounding size): file names, URLs and other literal text the user types or looks for.

### Named Rules
**The Names In Ink Rule.** A name the user looks for in another UI (a button in OBS, a device, a menu path) is set at 600 and, inside secondary text, in ink. Nothing else is bold.

**The Typographic Quotes Rule.** Apostrophes and quotes render as typographic ones (’ “ ”), whatever the copy's source uses.

## Layout

A fixed client area of 760 × 520 DIPs in three bands, the same on every page:

- **The band** (0–104) on Paper Subtle, ruled off by a hairline. Four stages with their icons' left edges at x 48, 220, 392 and 564: a 21 DIP icon at y 16, the name at 43, the value (up to two lines, 152 wide, cut with an ellipsis) at 64. The wire runs through the icons' centres, 6 DIPs clear of each icon, ending in an open arrowhead before the next.
- **The panel** (24–736 × 118–452): the gutter is 24 DIPs from the window's sides. Its content starts 24 DIPs in from the panel's outside edge, so it lines up with the band's first icon, 20 from the top and 17 from the bottom. It scrolls when its content doesn't fit, with a hairline at the cut edge as the only sign.
- **The foot** (468–500): the note and any waiting status on the left from x 24, the buttons on the right ending at x 736, 8 DIPs apart, centred on the 32 DIP button height.

Vertical rhythm inside the panel: heading, then 6 to a lead paragraph and 12 between paragraphs; 20 before doors, 16 before choices or notes, 14 before steps, 18 before the check box. Doors stack 8 apart, notes 12, steps 5. The guide's walkthrough takes 300 by 188 DIPs at the right of the steps, level with their top, with a 28 DIP gap.

The window scales as a whole: it opens on the pointer's monitor at that monitor's DPI, multiplied by Windows' text size (1 to 2.25), stepping down by 0.05 until it fits the work area. There are no breakpoints and no reflow.

### Named Rules
**The Path Always Shows Rule.** Every page keeps the band with all four stages in their fixed places. A page is about at most one stage; pages about none, such as Looking for OBS, show no caret.

## Elevation & Depth

Flat, with tonal layering: Paper under Paper Subtle under Surface, and Sunken for wells. The only shadow is the panel's lift, a copy of the panel shape offset 1 DIP down in black at 10% opacity (light) or 50% (dark), none in high contrast. Focus is a 2 DIP ink ring drawn 3 DIPs outside the control. Hairlines are whole device pixels, about one DIP, snapped to the pixel grid at every scale.

### Shadow Vocabulary
- **Panel lift** (0 1px 0 rgba(0,0,0,0.10) light; rgba(0,0,0,0.5) dark): under the one panel only.

### Named Rules
**The One Lift Rule.** The panel is the only raised thing. Buttons, doors, groups and pictures are flat, separated by hairlines and fills.

## Shapes

Two radii and full circles. Controls, the box of choices, the chain's well, the check box and pictures take 4 DIPs; the panel takes 8. Radios, step discs, the waiting dot and badges are circles. Borders are single hairlines drawn inside the shape's edge.

The caret is the panel's top edge rising to a point under the current stage: two 6 DIP ink strokes at 45°, the panel's surface filled behind them so the border opens.

Icons are the site's stroked glyphs (mic, sliders, plug, app grid), drawn from path data in a 20-unit square with 1.5-unit round-capped strokes in a 21 DIP box; at 100% scale they are redrawn on pixel centres with a 1 DIP stroke so they match the wire. Small icons (chevron, check) use a 16-unit box. A link's "open in new window" mark isn't drawn: it is Windows' own glyph (U+E8A7) from its icon font, Segoe Fluent Icons or Segoe MDL2 Assets, set as text.

### Named Rules
**The Two Radii Rule.** 4 DIPs for anything you click or that holds a value, 8 for the panel. No other corner radius on a surface or control.

**The Badge Rule.** A stage's mark uses the tray badge's proportions: 7/16 of the icon (9 DIPs on a 21 DIP icon), flush with its bottom-right corner, with a 1 DIP clear ring of the band's ground around it. Beside a note the same mark stands alone at 14.5 DIPs, with no ring.

## Components

### Buttons
Quiet, native-sized, right-aligned in the foot.
- **Shape:** gently squared (4px), 32 DIPs high, at least 84 wide, 16 DIPs of padding each side.
- **Primary:** the page's default, the one Enter clicks: ink fill and border, On Ink label at 600. Hover lifts to Ink Hover; pressed drops to Graphite 2.
- **Secondary:** Surface with a Hairline Strong border, ink label at 400. Hover is Paper Subtle with a Graphite 3 border; pressed is Sunken.
- **Disabled:** Paper with a Hairline border and Graphite 3 text.
- **Focus:** the 2 DIP ink ring, 3 DIPs out, shown once the keyboard is used.
- **Access keys:** every button, door and the check box has one ("&Back" in the page's data). Its letter is underlined in the label's colour while Alt is held, until the mouse is used, or always when Windows is set to underline access keys; Alt and the letter, or the letter alone, clicks it.

### Doors (command links)
The first page's two ways in, full panel width.
- **Shape:** 4px, at least 62 DIPs high; title (Title) over note (Body Small, Graphite 2), 16 DIPs from the left, a 16 DIP chevron 30 from the right.
- **Default:** an ink border and an ink chevron. **Other:** Hairline Strong border, Graphite 3 chevron.
- **Hover:** Paper Subtle fill and an ink border; pressed is Sunken.

### Choices
- **Style:** one box of rows on Surface with a Hairline Strong border, rows ruled by hairlines. Each row has an 18 DIP radio 13 DIPs in, the title at 44, its note under it in Graphite 2.
- **State:** selected is an ink disc with an On Ink centre (3 DIP radius); unselected is a Surface disc with a Graphite 3 ring that turns ink on hover. Arrows move the choice.

### Check box
- **Style:** an 18 DIP box at 4px; unchecked is Surface with a Graphite 3 edge (ink on hover), checked is an ink fill with an On Ink check.

### Links
- **Style:** ink text with a one-pixel underline 3 DIPs below the baseline, Graphite 3 at rest and ink on hover, under the words only. Each ends in Windows' "open in new window" glyph at 0.8 of the text's size, 2 DIPs after the last word, in the link's colour. In high contrast they take the system's link colour.

### Chain well
The chain as it will run: Sunken fill, Hairline border, 4px. The mic and the cable lead and close with their 16 DIP icons in ink at 600; filters sit between in Graphite 2, each step separated by a 12 DIP Graphite 3 chevron, wrapping onto 22 DIP rows.

### The band (signature)
Four stages on a wire. A stage's icon and name are ink when it is the page's stage, Graphite 2 once known, Graphite 3 while unknown. The wire is ink from a set stage to the next one that is set or that the page is about, Graphite 3 elsewhere. As the signal reaches a stage the wire inks along over 320 ms on the site's curve (cubic-bezier(0.2, 0, 0, 1)) and the caret slides under the new stage in step.

### Guide (signature)
Numbered steps with the number in Graphite 3 at Numeral size, text in Body. Beside them, the walkthrough, framed at 4px with a Hairline Strong border. The step it shows has its number on a 20 DIP ink disc, and so has its top-left corner, ringed with 2 DIPs of Surface, and both move on as it does. A walkthrough is real screenshots of OBS, or of Windows and VB-Cable's setup, at twice their size, with a camera that follows a drawn Windows arrow pointer (white, with a black edge, as big as Windows' own at the app's size) that clicks with a white ring spreading 20 DIPs and fading. Its play and pause sit on a 26 DIP disc of #111317 at 72% (92% on hover) in the frame's bottom-right corner, white bars or a white triangle (in high contrast, a Window disc ringed and marked in WindowText, Highlight on hover, with the click's ring in Highlight and the pointer in Window and WindowText); the whole frame is the toggle, and its focus ring is the frame's. Where the camera sees past the screen's edge, the frame shows the screenshots' own ground: #111317 round OBS, #e3e6ee round Windows' light Explorer. Without a picture the steps take the panel's full width.

### Notes and waiting
Notes are body text 28 DIPs in, each led by a 16 DIP mark: the amber triangle for a warning, the graphite "i" disc for information. A page that waits shows a 7 DIP Graphite 3 dot in the foot, breathing between 35% and 100% opacity over 1.6 s, beside the status in Body Small.

## Do's and Don'ts

### Do:
- **Do** keep the band and its four stages on every page, at x 48, 220, 392 and 564.
- **Do** use ink for the one default action per page, and for whatever the page is about.
- **Do** show state with the tray's badges at their tray proportions: 7/16 of the icon, flush with the corner, a 1 DIP clear ring.
- **Do** set file names, URLs and literal text in Consolas, and names from other UIs at 600 in ink.
- **Do** snap hairlines to whole device pixels, and use 4 DIP radii for controls and 8 for the panel.
- **Do** move only on the site's curve, cubic-bezier(0.2, 0, 0, 1): 320 ms for structure, 200 ms for a page arriving, and nothing when Windows' animations are off.
- **Do** derive high contrast from the system colours alone.

### Don't:
- **Don't** colour text, borders or fills green, amber or red; state is a mark, not a tint.
- **Don't** add a second accent or a coloured primary button; the default button is ink.
- **Don't** raise anything but the panel, or give controls shadows.
- **Don't** replace the path with a numbered stepper or a "Step 2 of 5" count.
- **Don't** draw screenshots of OBS or VB-Cable as illustrations; the guide shows real screenshots or none.
- **Don't** give a surface or control a radius other than 4 or 8, or use Consolas for anything but literal text.
