---
name: knobs
description: knobs's own UI, the setup window, in the knobs brand's graphite and cap red and Windows' own type.
colors:
  ground: "#eef0f4"
  ground-deep: "#e3e6ec"
  raised: "#ffffff"
  raised-2: "#f4f6f8"
  sunken: "#e6eaef"
  screen: "#ffffff"
  line: "#d0d4dc"
  line-strong: "#b3b8c0"
  text: "#191b1d"
  text-2: "#494d55"
  text-3: "#5c6169"
  action: "#c55149"
  action-hover: "#b2403a"
  action-text: "#ab3a34"
  on-action: "#ffffff"
  link-underline: "#c55149"
  link-hover: "#ab3a34"
  focus: "#c55149"
  ground-dark: "#191b1d"
  ground-deep-dark: "#0c0e10"
  raised-dark: "#202225"
  raised-2-dark: "#25282c"
  sunken-dark: "#131518"
  screen-dark: "#131518"
  line-dark: "#2d2f33"
  line-strong-dark: "#3e4146"
  text-dark: "#e5ecf6"
  text-2-dark: "#b9bec7"
  text-3-dark: "#90969e"
  action-dark: "#df695e"
  action-hover-dark: "#e8796d"
  action-text-dark: "#df695e"
  on-action-dark: "#191b1d"
  link-underline-dark: "#df695e"
  link-hover-dark: "#df695e"
  focus-dark: "#df695e"
  mark-ok: "#23875a"
  mark-stop: "#e5484d"
  mark-warn: "#f0a33c"
  mark-info: "#4a4c52"
  mark-glyph: "#ffffff"
  mark-glyph-on-warn: "#17181b"
  mark-info-on-dark: "#ffffff"
  mark-glyph-on-info-on-dark: "#4a4c52"
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
  check-label:
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
  none: "0px"
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
    backgroundColor: "{colors.ground-deep}"
    textColor: "{colors.text-2}"
    height: "{spacing.band}"
  panel:
    backgroundColor: "{colors.raised}"
    textColor: "{colors.text}"
    rounded: "{rounded.none}"
    padding: "20px 24px 17px"
  button-primary:
    backgroundColor: "{colors.action}"
    textColor: "{colors.on-action}"
    typography: "{typography.title}"
    rounded: "{rounded.none}"
    padding: "0 16px"
    height: "{spacing.control}"
  button-primary-hover:
    backgroundColor: "{colors.action-hover}"
    textColor: "{colors.on-action}"
  button-primary-pressed:
    backgroundColor: "{colors.action-text}"
    textColor: "{colors.on-action}"
  button-secondary:
    backgroundColor: "{colors.ground}"
    textColor: "{colors.action-text}"
    typography: "{typography.title}"
    rounded: "{rounded.none}"
    padding: "0 16px"
    height: "{spacing.control}"
  button-secondary-hover:
    backgroundColor: "{colors.action}"
    textColor: "{colors.on-action}"
  button-secondary-pressed:
    backgroundColor: "{colors.action-text}"
    textColor: "{colors.on-action}"
  button-disabled:
    backgroundColor: "{colors.ground}"
    textColor: "{colors.text-3}"
  door:
    backgroundColor: "{colors.raised}"
    textColor: "{colors.text}"
    typography: "{typography.title}"
    rounded: "{rounded.none}"
    padding: "11px 48px 11px 16px"
  door-hover:
    backgroundColor: "{colors.raised-2}"
  door-pressed:
    backgroundColor: "{colors.sunken}"
  choice-group:
    backgroundColor: "{colors.raised}"
    textColor: "{colors.text}"
    typography: "{typography.title}"
    rounded: "{rounded.none}"
    padding: "10px 14px 10px 44px"
  radio-selected:
    backgroundColor: "{colors.action}"
    textColor: "{colors.on-action}"
    rounded: "{rounded.round}"
    size: "18px"
  checkbox-checked:
    backgroundColor: "{colors.action}"
    textColor: "{colors.on-action}"
    rounded: "{rounded.none}"
    size: "18px"
  chain-well:
    backgroundColor: "{colors.sunken}"
    textColor: "{colors.text-2}"
    typography: "{typography.chain}"
    rounded: "{rounded.none}"
    padding: "7px 12px"
  step-disc:
    backgroundColor: "{colors.text}"
    textColor: "{colors.raised}"
    typography: "{typography.disc-numeral}"
    rounded: "{rounded.round}"
    size: "20px"
  walkthrough-toggle:
    backgroundColor: "{colors.ground-dark}"
    textColor: "{colors.text-dark}"
    rounded: "{rounded.round}"
    size: "26px"
---

# Design System: knobs

## Overview

**Creative North Star: "The Mic's Path"**

knobs's own UI is one window, the setup window, and it is about one thing: where the mic goes. Every page sits under a band that draws the path (your mic › OBS's filters › virtual cable › your other apps), and the panel below answers the one question for the stage its caret points at. The look is the knobs brand's, carried into a Windows window: graphite grounds and cool greys, cap red for everything that can be pressed, hairlines, square corners and stroked icons, set in Windows' own type, so the window reads as a native app that belongs to knobs.

Every colour, the state marks' shapes, the corner radius, hairline and focus ring, and the motion's curve, durations and rise come from knobs-brand's tokens (`brand/dist/knobs_tokens.h`, generated from the submodule's `tokens/tokens.json`; the rules are in `brand/BRAND.md`). This file mirrors those values; where they differ, the tokens win.

It is calm and dense in the way a settings window is: a fixed 760 × 520 DIP client area, one panel, one heading per page, plain sentences in the product's voice, and nothing decorative. Colour other than graphite and cap red appears only as state marks, cut as the tray's badges are. Light and dark follow Windows' app mode; high contrast drops the palette entirely and uses the user's colours. On Windows 11 the title bar takes the window's ground, and the title bar and taskbar each carry the colour mark drawn for their own ground.

**Key Characteristics:**
- The path is always on screen: a band of four stages, each an icon on a 1px wire, with its name and what knobs knows of it.
- Cap red marks what can be pressed and what is knobs; everything else is graphite, grey and hairline.
- State is shown as marks only: a green disc with a check, an amber triangle with "!", a red disc with "!", a graphite disc with "i" or pause bars, which turns over to white on a dark ground.
- Square corners everywhere, true circles for round things, 45° angles.
- Flat: depth by tone and hairlines on whole device pixels; nothing casts a shadow.
- Segoe UI Variable with Consolas for file names.

## Colors

The brand's two schemes by role: a cool pale grey desk with white panels in light, graphite grounds in dark, one red for action, and the product's four state marks. Each role below gives light, then dark.

### Primary
- **Cap Red** (action; #c55149 light, cap red deep, and #df695e dark): what can be pressed. The default button's fill, every other button's edge, the suggested door's edge, the chosen radio and the checked box, and a control's edge or chevron under the pointer.
- **Action Hover** (#b2403a light, #e8796d dark): the default button under the pointer: deeper in light, lit a step in dark.
- **Action Text** (#ab3a34 light, #df695e dark): red words, a secondary button's label. A pressed button takes it in light, a step deeper than hover; dark lights its buttons under the pointer, so a pressed one goes back to Cap Red.
- **On Action** (#ffffff light, #191b1d dark): words and glyphs on a red fill: white in light, graphite in dark, never ink.
- **Link Underline** (#c55149 light, #df695e dark): a link's underline. The words keep their own colour.
- **Link Hover** (#ab3a34 light, #df695e dark): a link's words under the pointer.
- **Focus** (#c55149 light, #df695e dark): the focus ring.

### Neutral
- **Ground** (#eef0f4 light, #191b1d dark): the window's ground under the panel and the foot, the rest fill of the foot's secondary buttons, and the title bar on Windows 11.
- **Ground Deep** (#e3e6ec light, #0c0e10 dark): the band, the clear ring round a stage's mark, and the light frame round a walkthrough of Windows' Explorer.
- **Raised** (#ffffff light, #202225 dark): the panel, doors, the box of choices, an idle radio or check box, and the numerals on a step disc.
- **Raised 2** (#f4f6f8 light, #25282c dark): a door under the pointer.
- **Sunken** (#e6eaef light, #131518 dark): the chain's well and a pressed door.
- **Screen** (#ffffff light, #131518 dark): the brand's frame for a screenshot. A walkthrough of OBS has the dark Screen round it, and one of Windows' Explorer the light Ground Deep, close to its screenshots' own grounds.
- **Line** (#d0d4dc light, #2d2f33 dark): hairlines: the band's bottom rule, the panel's edge, rules between choices, the well's edge, and the scroll edges.
- **Line Strong** (#b3b8c0 light, #3e4146 dark): the box of choices' edge, a door's edge other than the suggested one, a walkthrough's frame, and a disabled control's edge.
- **Text** (#191b1d light, #e5ecf6 dark): headings, body text, names, door and choice titles, the stage the page is about, the caret, the wire as far as the signal has got, and the step disc.
- **Text 2** (#494d55 light, #b9bec7 dark): lead paragraphs, notes under door and choice titles, stage names and values once known (a name is Text on the page's stage), the chain's filters, the waiting status.
- **Text 3** (#5c6169 light, #90969e dark): a stage's icon and value while unknown, the unlit wire, step numerals, the foot's note, idle chevrons, an idle radio's or check box's edge (it holds 3:1 on Raised, as Line Strong doesn't), the waiting dot, and disabled text.

### Tertiary (state marks)
The same in both schemes but for the info disc, and the product's alone.
- **Mark Green** (#23875a): set, a disc with a white check.
- **Mark Red** (#e5484d): needs you, a disc with a white "!". It is the tray's attention badge. It is not Cap Red.
- **Mark Amber** (#f0a33c): a warning, a triangle with a dark "!" (#17181b).
- **Mark Graphite** (#4a4c52): a note, a disc with a white "i"; on the tray, paused, with white bars. On a dark ground (the dark scheme, a dark taskbar) it turns over: a white disc with a graphite "i" or bars, since the graphite disc barely shows there.

### Named Rules
**The Cap Red Rule.** Cap red marks what can be pressed and what is knobs, never an error or a warning. A problem is said in words in the text colours, with a state mark; only the buttons that fix it are red, because every button is.

**The Marks Only Rule.** Green, amber, the marks' red and the marks' graphite appear only in state marks. They never colour text, borders, fills or the wire.

**The User's Colours Rule.** In high contrast every ground is the system's window colour and every line, text, edge and mark is its window-text colour, with glyphs on marks in the window colour; under the pointer a control takes the user's highlight, with highlight text on it; links take the hot-light colour, disabled text gray-text, and the stage the page is about the highlight colour. Shapes and words carry the meaning.

## Typography

**Display Font:** Segoe UI Variable Display (with Segoe UI on Windows 10)
**Body Font:** Segoe UI Variable Text (with Segoe UI on Windows 10)
**Label/Mono Font:** Consolas, inline only

**Character:** Windows' own type, not the brand's Radio Canada, so the window sits with the rest of the system; the brand comes from colour, line, shape and icon.

### Hierarchy
- **Display** (600, 22/28): the page's heading, one per page, at the top of the panel.
- **Title** (600, 14/20): stage names in the band, door and choice titles, and every button's label: buttons are all 600, heavy enough for white words on cap red.
- **Body** (400, 14/21): the page's text, steps and notes. The first paragraph is the lead, in Text 2; the rest say what to do, in Text. Under the chain the order flips.
- **Check label** (400, 14/20): the check box's label.
- **Body Small** (400, 13/18): the note under a door's or choice's title, and what a waiting page waits for.
- **Chain** (13/20): the chain in its well; the mic and the cable at 600 in Text, the filters at 400 in Text 2.
- **Label** (400, 12/17): a stage's value in the band, and the foot's note.
- **Numeral** (600, 14/21): step numbers, in Text 3. **Disc numeral** (600, 12/20) on a step disc, the glyphs centred on it.
- **Mono** (Consolas, the surrounding size): file names, URLs and other literal text the user types or looks for.

### Named Rules
**The Names In Text Rule.** A name the user looks for in another UI (a button in OBS, a device, a menu path) is set at 600 and, inside secondary text, in Text. Nothing else in running text is bold.

**The Typographic Quotes Rule.** knobs's own text is written with typographic apostrophes and quotes (’ “ ”) in the source. Names, paths and errors from OBS, Windows or the user are shown as they are, straight quotes and all.

## Layout

A fixed client area of 760 × 520 DIPs in three bands, the same on every page:

- **The band** (0–104) on Ground Deep, ruled off by a Line hairline. Four stages with their icons' left edges at x 48, 220, 392 and 564: a 21 DIP icon at y 16, the name at 43, the value (up to two lines, 152 wide, cut with an ellipsis) at 64. The wire runs through the icons' centres, starting 6 DIPs after one icon and ending 7 before the next, ending in an open 45° arrowhead before the next.
- **The panel** (24–736 × 118–452): the gutter is 24 DIPs from the window's sides. Its content starts 24 DIPs in from the panel's outside edge, so it lines up with the band's first icon, 20 from the top and 17 from the bottom. It scrolls when its content doesn't fit, with a hairline at the cut edge as the only sign.
- **The foot** (468–500): the note and any waiting status on the left from x 24, the buttons on the right ending at x 736, 8 DIPs apart, centred on the 32 DIP button height.

Vertical rhythm inside the panel: heading, then 6 before a paragraph in Text 2, such as a lead, and 12 before one in Text (12 to the chain's well and 12 after it); 20 before doors, 16 before choices or notes, 14 before steps, 18 before check boxes. Doors stack 8 apart, notes 12, steps 5, check boxes 8. The guide's walkthrough takes 300 by 188 DIPs at the right of the steps, level with their top, with a 28 DIP gap.

The window scales as a whole: it opens on the pointer's monitor at that monitor's DPI, multiplied by Windows' text size (1 to 2.25), stepping down by 0.05 until it fits the work area. There are no breakpoints and no reflow.

### Named Rules
**The Path Always Shows Rule.** Every page keeps the band with all four stages in their fixed places. A page is about at most one stage; pages about none, such as Looking for OBS, show no caret.

## Elevation & Depth

Flat. Depth comes from tone and hairlines: Ground Deep for the band, Ground for the window, Raised for the panel and its controls, Raised 2 under the pointer, Sunken for the well and a pressed door. The panel is set apart by its tone and a Line hairline alone. Hairlines are whole device pixels, about one DIP, snapped to the pixel grid at every scale. Focus is the brand's ring: 2 DIPs wide, 2 DIPs outside the control, in Focus.

### Named Rules
**The No Shadow Rule.** Nothing casts a shadow: no lift, glow, gradient or bevel on the panel, a control, a mark or a picture.

## Shapes

Square corners and true circles, from the brand's geometry (corner radius 0). The panel, buttons, doors, the box of choices, the chain's well, the check box, a walkthrough's frame and every focus ring are square-cornered. Radios, step discs, the waiting dot, the walkthrough's toggle and the marks' discs are true circles. Borders are single hairlines drawn inside the shape's edge; the default door's is 2 DIPs.

Angles are 45°: the caret, the wire's arrowheads, the chevrons and the checks. The caret is the panel's top edge rising to a point under the current stage: two strokes in Text at 45°, each 6 DIPs across and 6 up, the panel's Raised filled behind them so the edge opens.

Icons are drawn, not filled: 1.5 DIP strokes at any size, with butt caps and miter joins. The four stages (mic, sliders, plug, square app tiles) are drawn in a 20-unit square in a 21 DIP box, so the 1px wire runs through their centre; at 100% scale they are redrawn on pixel centres with a 1 DIP stroke to match the wire. Small icons (chevrons, the check box's check) use a 16-unit box. Round only where the thing is round: the mic's capsule, the sliders' knobs.

The state marks are the brand's, drawn from its `status_shape` paths, the glyph filled over the ground, never stroked; their shapes are in `brand/BRAND.md` (State marks). Their bars and dots land on whole device pixels.

### Named Rules
**The Square Corner Rule.** Every rectangle has square corners. No rounded rectangle or pill on a surface or control; anything round is a full circle.

**The Badge Rule.** A stage's mark uses the tray badge's proportions: 7/16 of the icon (9 DIPs on a 21 DIP icon), flush with its bottom-right corner, with a 1 DIP clear ring of Ground Deep round it. Beside a note the same mark stands alone at 14.5 DIPs, with no ring. On the tray icon it is 7/16 of the icon, flush with the corner, cut clear by a pixel at 16 px and two at 32.

## Components

### Buttons
The brand's buttons, right-aligned in the foot.
- **Shape:** square, 32 DIPs high, at least 84 wide, 16 DIPs of padding each side, labels at 600.
- **Primary:** the page's default, the one Enter clicks: solid Cap Red with On Action words. Under the pointer, Action Hover; pressed, a step deeper.
- **Secondary:** a Cap Red outline with Action Text words on the Ground. Under the pointer it fills Cap Red with On Action words; pressed, a step deeper.
- **Disabled:** the Ground with a Line Strong edge and Text 3 words.
- **Focus:** the 2 DIP ring, 2 DIPs out, shown once the keyboard is used.
- **Access keys:** every button, door and check box has one, and the walkthrough's toggle has P ("&Back" in the page's data). Its letter is underlined in the label's colour while Alt is held, until the mouse is used, or always when Windows is set to underline access keys; Alt and the letter, or the letter alone, clicks it.

### Doors (command links)
The first page's two ways in, full panel width.
- **Shape:** square, at least 62 DIPs high; title (Title, Text) over note (Body Small, Text 2), 16 DIPs from the left, a 16 DIP chevron 30 from the right.
- **Edge:** the suggested way in has a 2 DIP Cap Red edge, as the main item in a list does, and an Action Text chevron, so it's the one the eye goes to. The other has a 1 DIP Line Strong edge and a Text 3 chevron.
- **Hover:** Raised 2 fill, the edge and chevron Cap Red; pressed is Sunken.

### Choices
- **Style:** one box of rows on Raised with a Line Strong edge, rows ruled by Line hairlines. Each row has an 18 DIP radio 13 DIPs in, the title at 44, its note under it in Text 2.
- **State:** chosen is a Cap Red disc with an On Action centre (3 DIP radius); unchosen is a Raised disc with a Text 3 ring that turns Cap Red under the pointer. Arrows move the choice.
- **Focus:** rows sit edge to edge, so a row's ring is drawn inside it, 5 DIPs in, clear of the rules.
- **More:** other playback devices sit behind a last row, its title 16 DIPs in, the chosen device's name under it in Body Small Text 2, and a 16 DIP chevron, down or up, 30 DIPs from the right in Text 3, Cap Red under the pointer.

### Check box
- **Style:** an 18 DIP square box; unchecked is Raised with a Text 3 edge (Cap Red under the pointer), checked is a Cap Red fill with an On Action check at 45°.

### Links
- **Style:** the words keep the colour of the text round them, on a hairline Link Underline 3 DIPs below the baseline, under the words only. Under the pointer the words turn Link Hover. Each word box takes its own focus ring. In high contrast they take the system's link colour. A link ends in Windows' open-in-new-window glyph (Segoe Fluent Icons E8A7) at 0.8 of its text size, which belongs to the link but isn't underlined.

### Chain well
The chain as it will run: Sunken fill, Line edge, square. The mic and the cable lead and close with their 16 DIP icons in Text at 600; filters sit between in Text 2, each step separated by a 12 DIP Text 3 chevron, wrapping onto 22 DIP rows.

### The band (signature)
Four stages on a wire. A stage's icon is in Text when it is the page's stage, Text 2 once known and Text 3 while unknown; its name is in Text on the page's stage and Text 2 elsewhere. The wire is Text from a set stage to the next one that is set or that the page is about, Text 3 elsewhere. As the signal reaches a stage the wire fills along and the caret slides under the new stage, over 320 ms on the brand's one curve, cubic-bezier(0.2, 0, 0, 1).

### Guide (signature)
Numbered steps with the number in Text 3 at Numeral size, text in Body. Beside them, the walkthrough, in a square frame with a Line Strong edge. The step it shows has its number on a 20 DIP Text disc with Raised numerals, and so has the frame's top-left corner, ringed with 2 DIPs of Raised, and both move on as it does. A walkthrough is real screenshots of OBS, or of Windows and VB-CABLE's installer, at twice their size, with a camera that follows a drawn Windows arrow pointer (white, with a black edge, as big as Windows' own at the app's size) that clicks with a white ring growing from 4 to 20 DIPs across its radius and fading. Its play and pause sit on a 26 DIP opaque disc of the dark scheme's Ground (#191b1d, Raised 2 #25282c under the pointer) ringed with its Line Strong, in the frame's bottom-right corner, with a play triangle or the state marks' pause bars in its Text, the same in both schemes since it sits on screenshots of either. In high contrast it is a Window disc ringed and marked in WindowText, Highlight under the pointer, with the click's ring in Highlight and the pointer in Window and WindowText. The whole frame is the toggle, and its focus ring is the frame's. Without a picture the steps take the panel's full width.

### Notes and waiting
Notes are body text 28 DIPs in, each led by a 14.5 DIP mark: the amber triangle for a warning, the "i" disc for information (graphite in light, turned over to white in dark). A page that waits shows a 7 DIP Text 3 dot in the foot, breathing between 35% and 100% opacity over 1.6 s, beside the status in Body Small, Text 2.

### Motion
One curve, cubic-bezier(0.2, 0, 0, 1): quick to start, then settling. Structure, the wire and the caret, takes 320 ms; a page arriving fades in and rises 4 DIPs over 160 ms, as feedback. The waiting dot breathes once every 1.6 s. A guide's walkthrough eases in and out on its own timing, so the camera never whips. With Windows' animations off, nothing moves until the user plays a walkthrough.

## Do's and Don'ts

### Do:
- **Do** keep the band and its four stages on every page, at x 48, 220, 392 and 564.
- **Do** make the page's default button solid cap red, and every other button a cap red outline with red words that fills red under the pointer.
- **Do** take every colour, mark shape, radius, hairline, focus ring, duration and distance moved from knobs-brand's tokens, and mirror them here.
- **Do** show state with the brand's marks at the tray badge's proportions: 7/16 of the icon, flush with the corner, a 1 DIP clear ring.
- **Do** set file names, URLs and literal text in Consolas, and names from other UIs at 600 in the text colour.
- **Do** snap hairlines to whole device pixels, and draw icons with 1.5 DIP strokes, butt caps and miter joins.
- **Do** move only on the brand's curve, cubic-bezier(0.2, 0, 0, 1): 320 ms for structure, 160 ms for a page arriving, and nothing when Windows' animations are off.
- **Do** derive high contrast from the system colours alone, with the user's highlight under the pointer.

### Don't:
- **Don't** use cap red for an error or a warning; a problem is words and a state mark.
- **Don't** colour text, borders or fills green, amber, the marks' red or the marks' graphite; state is a mark, not a tint.
- **Don't** make a button graphite or black, or add a second accent.
- **Don't** give anything a shadow, glow, gradient or bevel.
- **Don't** round a corner: rectangles are square and round things are full circles.
- **Don't** replace the path with a numbered stepper or a "Step 2 of 5" count.
- **Don't** draw screenshots of OBS or VB-CABLE as illustrations; the guide shows real screenshots or none.
- **Don't** use Consolas for anything but literal text.
