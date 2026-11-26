# 04 · UI Specification (functional)

This file describes the regions of Weft Studio's main window and what each does. It is deliberately functional: it names areas, their contents, and their behavior, and leaves visual design (palette, iconography, typography, control shapes, spacing) to Weft's own design system. Nothing here prescribes the appearance of any existing product.

Visual identity requirements for the design system (summary):
- Weft's own name, wordmark, and icon set, drawn from scratch.
- Default dark theme and a light theme (F-UI-3), defined as tokens so every control can be re-themed.
- A distinct identity: Weft's accent colour and control language should be chosen to look unlike other DAWs, and its 24-swatch track palette is its own.
- Minimum touch/click target 24 × 24 px at 100% scaling; all text ≥ 11 px at 100%.

---

## 1. Main window layout

```
+--------------------------------------------------------------------------+
| A. Title bar and menu bar                                                |
+--------------------------------------------------------------------------+
| B. Control bar: panel toggles, transport, displays, modes, presence      |
+------------+---------------------------------------------+---------------+
| C.         | D. Global tracks (markers, tempo, signature)| F. Side panel |
| Inspector  +---------------------------------------------+               |
|  (left)    | E. Tracks area                              |  (right; tabs:|
|            |    track headers | timeline with regions    |  Library,     |
|            |                  |                          |  Browser,     |
|            |                  |                          |  Comments,    |
|            |                  |                          |  Chat,        |
|            |                  |                          |  History)     |
+------------+---------------------------------------------+---------------+
| G. Bottom area (tabs: Mixer, Piano Roll, Audio Editor), resizable        |
+--------------------------------------------------------------------------+
| H. Status bar: messages, sync state, media transfer, CPU/disk            |
+--------------------------------------------------------------------------+
```

- Panels C, F, and G are each shown or hidden with a toggle button in the control bar and a shortcut (Inspector **I**, Side panel **Ctrl+L**, Mixer **X**, Piano Roll **P**, Audio Editor **E**). §11 lists all default shortcuts and is authoritative.
- The tracks area (E) always remains visible and takes the remaining space.
- Dividers between panels are draggable. Panel sizes and visibility persist per user (local view state).
- Minimum window size: 1024 × 640 at 100% scaling. Below 1280 px wide, the side panel overlays the tracks area instead of pushing it.

## 2. A · Title and menu bar

- Title: `<Project name>[*] — Weft Studio`, asterisk when unsaved.
- Menus: **File** (New, Open, Recent, Save, Save As, Save Version, Bounce, Import, Project Settings, Exit), **Edit** (Undo, Redo, Cut, Copy, Paste, Delete, Select All, Split, Join, Quantize), **Track** (New Track, Duplicate, Delete, Color), **Region** (Mute, Loop, Gain, Fades, Apply Quantize), **Mix** (New Bus, Show Mixer), **View** (panels, zoom, follow playhead, theme), **Share** (Share Project, Invite, Sign In/Out, Version History, Go Offline), **Help** (Shortcuts, About, Licenses).
- Every menu item shows its shortcut. Native Windows title bar (no custom chrome in MVP).

## 3. B · Control bar

Left to right, functional groups:

1. **Panel toggles:** Inspector, Side panel, Mixer, Editor, Global tracks.
2. **Transport buttons:** Go to start, Rewind, Fast forward, Stop, Play, Record, Cycle. Record is visually distinct. Each shows state (lit when active). **Decision D-UI-17:** Stop pressed while already stopped moves the playhead to the cycle start when Cycle is on and the playhead isn't there, otherwise to the project start (also the **Stop** command, numpad 0). With Cycle on, Play (and Space) starts at the cycle start wherever the playhead is; Pause and Record are unchanged. **Count-in** (Shift+K) turns the count-in off and back on at the length last used.
3. **Main display (LCD-like readout, own design):**
   - Position: bars.beats.division.ticks and time h:mm:ss.ms (click either to type a position; Enter jumps).
   - Tempo (BPM) at the playhead: drag vertically or double-click to type; edits the tempo point in effect at the playhead.
   - Time signature at the playhead: click to edit.
   - Cycle range start/end when cycle is on.
4. **Mode buttons:** Metronome, Count-in (with dropdown 0/1/2 bars), Autopunch, Snap (with grid value dropdown), Follow playhead.
5. **Meters:** CPU (audio thread load %), Disk; both turn amber > 70%, red > 90% or on dropout. Clicking shows a detail popup with dropout count since session start.
   **Decision D-UI-21:** when the window is too narrow for the whole bar, the CPU meter and then the count-in menu are dropped as before, and a **More** button ("…") appears in their place. Its menu holds whatever was dropped: the count-in choices (the current one ticked) and a line with the CPU load, dropouts and disk underruns. The count-in is also always in Navigate › Count-in.
6. **Presence and sharing:** avatars of connected collaborators (initials in their color; up to 5, then "+N"), a sync state indicator (see §10), and a **Share** button.

## 4. C · Inspector (left)

Shows properties of the current selection in collapsible sections:

1. **Region section** (when regions are selected): name, color, mute, gain (audio), fades and curves (audio), quantize/strength/swing (MIDI), loop on/off, transpose and velocity offset (MIDI; v2 UI). Multi-selection shows mixed values as "—"; editing sets all. **Decision D-UI-22:** with several regions selected the name reads "N regions"; an edit is one undo step; a control that only audio regions have (gain, fades) shows only when no MIDI region is selected, and the MIDI controls only when every selected region is MIDI; mute, loop, delay and the name apply to any mix.
2. **Track section** (selected track): name, color, input, output, monitor mode, automation mode, lock state.
3. **Channel strips:** two compact channel strips side by side: the selected track's strip, and the strip of its output destination (Master or a bus). Each has instrument slot (instrument tracks), insert slots, send slots, output button, pan, fader, meter, mute/solo. These are the same controls as the mixer (§8) and edit the same data.
   - **Decision D-UI-16:** the channel section sits below the Track section under a "Channel" header. Above the two strips an EQ thumbnail (48 px, full width) draws the response of the selected track's first Weft EQ insert, 20 Hz–20 kHz on a log scale and ±18 dB, flat when there is none or it is bypassed; clicking it opens that EQ, or adds a Weft EQ in the first free insert slot and opens it. The strips are the mixer's own strip component, so every edit is the mixer's edit (one undo history); they share one slot-row count so their faders line up, and fill the panel's remaining height (the inspector scrolls when the sections need more). Clicking the output strip edits it without moving the selection. Inspector and mixer meters read the same channel independently (each sees every peak). The inspector is at least 224 px wide so two strips fit.
4. **Note section** (in piano roll focus with notes selected): pitch, velocity, length, position, channel.

## 5. D · Global tracks

Lanes above the tracks area, sharing its timeline:

- **Ruler:** bar numbers (and optionally time). Click to move playhead. A thin strip at the top of the ruler holds the cycle range (drag to create; drag edges; click to toggle) and the punch range (shown when autopunch is on).
- **Marker lane:** markers as labeled flags. Double-click empty lane to create at that position; drag to move; double-click label to rename; right-click to delete or set color. Clicking a marker jumps the playhead. Collaborator comments also appear here as pins (§9.3) when the "Comments on timeline" toggle is on (default on).
- **Tempo lane:** step graph of tempo; points draggable (W-11).
- **Signature lane:** meter changes as labels at bar starts.
- **Decision D-UI-23 (changed 2026-11):** right-click in these lanes opens a menu instead of deleting at once. A marker: Rename Marker..., Color (the palette, or No Color), Delete Marker. A tempo change: Set Tempo..., Delete Tempo Change. A time signature: Edit Time Signature..., Delete Time Signature. The first tempo and the first signature can't be deleted, so their Delete item is disabled. On empty lane space: Add Marker Here, Add Tempo Change Here (at the snapped position and the tempo under the pointer) or Add Time Signature Here.... Double-click still does the first of these directly.

## 6. E · Tracks area

### 6.1 Track header (left column)
Per track, from left: color strip, track number, name (double-click to rename), mute **M**, solo **S**, record-enable **R** (audio/instrument only), input monitor (audio only), lock icon (shown when locked; clickable to lock/unlock), and a small fader/pan pair when the header is tall enough (≥ 64 px). When automation view is on: parameter selector and mode selector.

Presence on headers: when a collaborator is editing on a track (their last edit or selection is on that track within the last 10 s), a small avatar badge in their color appears on the header. A red pulsing badge means they're recording on it.

Header right-click menu: New track (same type), Duplicate, Delete, Color, Lock/Unlock, Show automation.

Header drag: reorder. Header area below the last track: double-click creates a new track (opens New Track dialog). **Decision D-UI-19:** **New Audio Track** (Ctrl+Alt+A) and **New Instrument Track** (Ctrl+Alt+I) add one stereo track of that kind below the selected track and select it, without the dialog. **Mute Track** (Shift+M), **Solo Track** (S) and **Record-Enable Track** (Shift+R) press the selected track's buttons. A **+** button above the headers (right of the TRACKS caption) opens the New Track dialog, like the double-click below the last track.

### 6.2 Timeline (right)
- Background shows the grid (bar lines strong, beat lines medium, subdivisions faint, adapting to zoom).
- Regions are rectangles in their color, with a name bar on top and content overview: waveform for audio (peak data from the Cache), mini piano-roll for MIDI.
- Region handles (hover reveals cursor change): lower corners = trim, upper corners = fade (audio), upper-right edge = loop, center = move.
- Take folders show a header strip with "Take N/M" and an expand chevron; expanding shows each take as a sub-lane.
- Muted regions are desaturated. Regions with missing or downloading media show a hatched pattern with a progress indicator.
- Playhead: vertical line across global tracks and tracks area, with a draggable handle in the ruler.
- Collaborator playheads: thinner lines in their colors with name tag in the ruler, shown only while their transport is running (§9).
- Collaborator pointers: small cursors with name labels in their color, shown when their pointer is over the tracks area of the same project (§9).
- Collaborator selections: regions selected by others get an outline in that collaborator's color. Your own selection is drawn as filled highlight.
- Automation lanes (when shown): under each track, a line with points in the track color.
- Scrolling: wheel = vertical, Shift+wheel = horizontal, Ctrl+wheel = horizontal zoom at pointer, Ctrl+Shift+wheel = vertical zoom (track heights).
- Tool selection: T opens the tool menu at the pointer; Esc returns to pointer tool.
- **Decision D-UI-18:** **Deselect All** (Ctrl+Shift+A) clears the region selection (the note selection while the piano roll has focus). **Invert Selection** (Ctrl+Shift+I) selects every unselected region on a track that has a row and deselects the rest (in the piano roll: the notes of the shown regions).
- **Decision D-UI-20:** while the global tracks are hidden, each marker shows as a small flag on the ruler's lower edge (a line in the marker's colour and its name); clicking a flag moves the playhead to the marker. Renaming, moving and deleting markers stay in the marker lane.

### 6.3 Empty state
A new project shows one empty audio track and a hint in the timeline: "Drop audio or MIDI files here, or press R to record."

## 7. G · Bottom area editors

Tabs: **Mixer** (X), **Piano Roll** (P), **Audio Editor** (E). Double-clicking a region opens the matching editor. The bottom area is resizable from 160 px to 80% of window height; double-clicking its divider toggles maximized.

### 7.1 Piano roll
- Left: vertical keyboard; clicking a key plays it and selects all notes of that pitch (Shift adds).
- Top: its own ruler with region boundaries; local snap/grid dropdown and Quantize dropdown with **Q** button; strength and swing fields.
- Center: note grid. Notes drawn as bars with velocity-coded fill and the note name when wide enough. Muted notes hollow. Selected notes highlighted. Out-of-region area shaded.
- Bottom: collapsible controller lane(s) with a lane selector (Velocity default).
- Toolbar: tool selector (pointer, pencil, eraser, velocity, scissors), audition toggle, follow toggle, zoom sliders.
- Collaborator pointers and note selections appear here too when they have the same region open.
- **Decision D-UI-7:** the piano roll's keys (arrows, Ctrl+X/C/V, Delete, Ctrl+A, Ctrl+M, Q, Esc) act on notes once the piano roll has been clicked; clicking the tracks area gives them back to regions. Alt+Left/Right move the selected notes by the piano roll's grid (1/16 when its snap is off); Left/Right select the previous or next note (D-UI-28, which replaces the earlier "Left/Right move notes" and "arrow keys do nothing in the tracks area").
- **Decision D-UI-8:** clicking a piano key always sounds it, with audition on or off; the audition toggle governs notes clicked, drawn or moved. Auditioned notes go straight to the track's instrument for their length (at most 1 s), whether or not the transport runs.
- **Decision D-UI-9:** the Inspector's note Position field sets every selected note to the typed start; Pitch, Length and Channel likewise set all; mixed values show "—", except velocity, which shows its range (W-4 step 9).

### 7.2 Audio editor
- Waveform of the selected region's track, zoomable to sample level, with the region's used part highlighted against the full source file (greyed outside).
- Trim handles and fade handles as in the arrange view, sample-accurate when snap is off.
- Region gain line draggable.

### 7.3 Mixer
See §8.

## 8. Mixer

Shown in the bottom area (MVP; detachable window in v2).

- One vertical channel strip per track in `trackOrder` order, then buses, then Master at the far right (Master is pinned so it's always visible when scrolling horizontally).
- Each strip, top to bottom:
  1. Name and color (editable).
  2. Input selector (audio tracks) / instrument slot (instrument tracks).
  3. Insert slots (shows filled slots + one empty slot; expands as needed up to 15). Slot shows plugin name; click name opens editor; bypass toggle; drag to reorder; right-click menu Remove/Replace/Bypass.
  4. Send slots (filled + one empty, up to 8): target bus name, level knob, pre/post indicator, mute.
  5. Output selector.
  6. Automation mode selector.
  7. Pan knob (balance for stereo tracks). Alt+click resets.
  8. Fader with dB scale and a numeric readout (double-click to type), and a post-fader meter with peak hold and clip LED (click to reset). Alt+click fader resets to 0 dB.
  9. Mute, Solo, Record-enable (tracks), Monitor (audio).
  10. Lock icon and collaborator badge as on track headers.
- Selecting a strip selects the track (and vice versa).
- Disabled feedback edges (D-DM-6) show a warning badge on the affected send or output button.
- Missing-plugin placeholders show the plugin name with a warning style and tooltip "Not installed on this computer".
- **Decision D-UI-10:** the Monitor button cycles Auto, On, Off (lit when On, "-" when Off). The instrument slot of an instrument track with no instrument shows "Weft Keys", the default. The fader runs from −90 dB (shown "-inf", stored as −inf) to +6 dB with 0 dB about three quarters up (D-UI-12 replaces the curve with the automation taper); Shift-drag is fine adjustment. Typing in the readout accepts "-6", "-6 dB" or "-inf".
- **Decision D-UI-12:** automation lanes sit below their track, 60 px each. Each lane's header area shows its parameter selector and a "+" that adds a lane for the next parameter not shown; the first lane also shows the track's mode selector. Which lanes are shown is local view state. In a lane: click adds a point (time snapped, value at the click); drag moves the selected points (time snapped at the grabbed point, Shift keeps only the larger of the two movements); Shift+click adds or removes a point from the selection; dragging from an empty spot rubber-bands; Alt+drag on a segment bends it (up bulges the line up); Delete removes the selected points. A lane with no points shows the static value as a thin line. The mixer's fader, pan and mute follow automation while it plays and use the same taper as the lanes, so 0 dB is at about 79 % of the fader travel.
- **Decision D-UI-11:** a plug-in window is titled "<plug-in> · <track>". It shows the plug-in's own editor, or a slider per parameter when it has none, or "Not installed on this computer" for a placeholder. Window positions are remembered per instance while the app runs, not saved in the project.
- **Decision D-UI-24:** above the editor, a plug-in window has a header strip with a **Bypass** button for insert effects (lit while bypassed; the same switch as the slot's bypass, one undo step). An instrument's window has the strip without the button. The window follows the editor's size when the plug-in resizes itself.
- **Decision D-UI-13:** design tokens. Every colour, size, radius, type style and depth comes from `app/src/ui/Theme.h`; `tools/ui_token_lint.py` (ctest `ui-token-lint`) fails on a colour literal or numeric font size anywhere else in the UI. Sizes sit on an 8 px grid (4 px only inside controls): controls and rows 24 px, the control bar 48, panel headers and the ruler 32, track rows and automation lanes 56 (this replaces D-UI-12's 60 px), gutters between zones 4 px and also the resize handles. Type is IBM Plex Sans (caption 11 caps, small 11, body 12, strong 13, title 15, display 24) with IBM Plex Mono for time, tempo and levels (11, 12, counter 20); both ship under the OFL. The dark theme uses low-glare greys (ground #16171A) with one accent, a warm orange, for selection, record and keyboard focus. Track colours are 24 muted swatches; a region's body is its colour laid over the canvas and only its name bar takes the full swatch. Depth is a top highlight and a short soft shadow on raised things and an inner shade on wells, drawn as a few translucent fills. The theme switch lives in the View menu, not the control bar. The transport's record button keeps the record colour while off, as a landmark; track arm buttons take it only when armed.
- **Decision D-UI-14:** waveforms are drawn from a min/max/RMS pyramid per media file (32-frame buckets, each coarser level 4 times wider, 16-bit values), as filled outlines in device pixels: the peak envelope in the region's content tone at partial strength with the RMS envelope solid inside it. Zoomed in past the finest bucket, the outline is interpolated between bucket centres, so it stays smooth rather than showing individual samples.
- **Decision D-UI-15:** mixer meters show RMS (an exponential average over about 300 ms, computed on the audio thread) as the solid bar and the peak (the existing ballistics) above it in a lighter tone, coloured by fixed zones: green below −18 dBFS, yellow to −6, red above. The meter uses the fader's taper, so one dB scale between fader and meter reads for both. The clip light is an 8 px block on top. Faders and knobs reset on double-click as well as Alt+click; dragging or hovering a fader shows its value in a tag, which says "fine" while Shift is held. Strips stay 104 px wide (13 grid units) so an insert's name fits beside its bypass dot; the dot is green while the effect runs and hollow when bypassed.

## 9. Collaboration UI

### 9.1 Presence
- Toolbar avatars (§3.6). Hovering an avatar shows name, status (editing / idle > 2 min / recording / offline-editing), and the track/editor they're in. Clicking an avatar scrolls your view to their playhead or selection (v2: follow mode).
- Collaborator colors are assigned per project from Weft's palette, distinct from track colors in saturation/outline style so they don't read as track colors. Each user keeps the same color in a project.

### 9.2 Share dialog
- Shows project link, invite by email field with role selector (Editor default, Viewer), list of members with roles (Owner can change roles and remove members).
- For an unshared project, **Share** first asks to upload: "This uploads the project and its N MB of audio to the relay." with Continue/Cancel.

### 9.3 Comments
- Side panel Comments tab lists all comment threads sorted by timeline position, with filters (Open, Resolved, Mine, Mentions).
- Creating: right-click on the timeline or a region and choose Add Comment, or press **Shift+C**. **Decision D-UI-2:** Shift+C because C alone toggles cycle. A comment pin appears in the marker lane and, if anchored to a track, a small pin on that track at the position.
- Clicking a pin opens a popover with the thread, reply box, Resolve, Edit/Delete (own comments).
- Unread comments show a dot on the Comments tab.

### 9.4 Chat
- Side panel Chat tab: message list with author avatar, name, time; input box at bottom (Enter sends, Shift+Enter newline). Unread count on the tab. Time references like "1:23" or "bar 17" in messages render as links that move the playhead. **Decision D-UI-3:** link patterns are `bar N`, `bar N beat M`, and `m:ss`.

### 9.5 Version history
- Side panel History tab: list of versions (named first-class, automatic ones collapsed by day), each with author, time, and name.
- Selecting a version shows a read-only preview banner over the tracks area ("Viewing version '<name>' – read-only. Restore / Return to current") with the timeline rendered at that state; playback of the preview is allowed if media is available.
- **Save Version…** button at the top (also Ctrl+Alt+S).

### 9.6 Locks
- Lock toggle on track header/strip. Locked by someone else: controls on that track are disabled (greyed) with tooltip "Locked by <name>"; regions on it can't be selected for editing. Owner of the project can break any lock (confirmation dialog).

## 10. H · Status bar and sync indicator

- Left: transient messages (e.g. "Region deleted by Ana", "Can't undo: item was deleted").
- Center: **sync state**: `Local only` (never shared), `Synced`, `Syncing…`, `Offline – N changes pending`, `Reconnecting…`, `Update required` (relay reports newer schema). Clicking shows details: relay URL, last sync time, pending change count.
- Media transfer: "↑ 2 files (34 MB) · ↓ 1 file" with a progress popup listing each file.
- Right: sample rate, buffer size, driver type; CPU and disk meters duplicate the control bar's at small widths.

## 11. Default keyboard shortcuts (authoritative)

| Command | Shortcut | | Command | Shortcut |
|---|---|---|---|---|
| Play/Stop | Space | | Undo / Redo | Ctrl+Z / Ctrl+Y, Ctrl+Shift+Z |
| Record | R | | Cut / Copy / Paste | Ctrl+X / C / V |
| Cycle on/off | C | | Delete | Delete, Backspace |
| Metronome | K | | Select all | Ctrl+A |
| Go to start / end | Home / End | | Split at playhead | Ctrl+T |
| Previous / next bar | , / . | | Join | Ctrl+J |
| Autopunch | Ctrl+Alt+P | | Mute region / notes | M / Ctrl+M |
| Save / Save As | Ctrl+S / Ctrl+Shift+S | | Quantize | Q |
| Save Version | Ctrl+Alt+S | | Nudge left/right | Alt+Left / Alt+Right |
| Open / New | Ctrl+O / Ctrl+N | | Transpose ±1 / ±12 | Up/Down, Shift+Up/Down |
| Bounce | Ctrl+B | | Zoom to fit | Z |
| New track | Ctrl+Alt+N | | Zoom in/out horizontally | Ctrl+= / Ctrl+− |
| Inspector | I | | Tool menu | T |
| Side panel | Ctrl+L | | Show automation | A |
| Mixer / Piano roll / Audio editor | X / P / E | | Global tracks | G |
| Add marker | ; (semicolon) | | Add comment | Shift+C |
| Follow playhead | Ctrl+F | | Snap on/off | N |
| Set cycle to selection | U | | Keyboard MIDI mode | Ctrl+K |
| Previous / next marker | Alt+, / Alt+. (or Ctrl+click Previous/Next Bar) | | Go to marker 1–9 / 10–19 | 1–9 / Ctrl+Numpad 0–9 |
| Repeat regions | Ctrl+R | | Stop / go to start | Numpad 0 |
| Count-in on/off | Shift+K | | Deselect all / Invert selection | Ctrl+Shift+A / Ctrl+Shift+I |
| Mute / Solo / Record-enable track | Shift+M / S / Shift+R | | New audio / instrument track | Ctrl+Alt+A / Ctrl+Alt+I |
| Rename / Delete marker | Shift+' / Alt+Delete | | Select previous / next region or note | Left / Right |
| Move notes left / right (piano roll) | Alt+Left / Alt+Right | | Tools, while the Tool menu is open | T P I J E M |
| Search and Add Plug-in | Ctrl+Shift+P | | New Track with Next Input | Ctrl+Shift+N |
| Search and Select Track | Ctrl+Alt+F | | Select All Following (piano roll) | Shift+F |
| Legato (piano roll) | Ctrl+Shift+L | | Lengthen / Shorten | Alt+Shift+Right / Left |
| Go to Position | Ctrl+G | | Go to Cycle Start / End | Ctrl+Home / Ctrl+End |
| Go to Selection Start / End | Shift+Home / Shift+End | | Play from Selection | Shift+Space |
| Move Cycle Forward / Back | Ctrl+Shift+Right / Left | | Slip Left / Right | Ctrl+Alt+Left / Right |
| Configure Track Header | Alt+T | | Show/Hide All Plug-in Windows | V |
| Mute Off for All | Ctrl+Alt+Shift+M | | Clear/Recall Solo | Ctrl+Alt+Shift+S |
| Input Monitoring Track | Shift+I | | Zoom Vertical In / Out | Ctrl+Down / Ctrl+Up |
| Save as Zoom 1-3 / Recall Zoom 1-3 | Ctrl+Alt+Shift+1-3 / Ctrl+Alt+1-3 | | Navigation: Back / Forward | Ctrl+[ / Ctrl+] |
| Page Up / Down | Page Up / Page Down | | Page Left / Right | Alt+Page Up / Alt+Page Down |
| Zoom Selected Track | Ctrl+Alt+Z | | | |

**Decision D-UI-1:** side panel is **Ctrl+L**; add marker is **;** (semicolon). Single-letter shortcuts are active only when focus is not in a text field.

- **Decision D-UI-25:** a track's Mute, Solo and Record buttons (header and mixer strip) take two modifiers: **Ctrl+click** turns it on for that track and off for every other track ("only this one"), and **Alt+click** gives every track (not the master) the state this one is about to take ("all"). Pressing a button and dragging over the same button of other tracks gives each of them the state the first one took; letting go ends the swipe without a second click. The mutes one Ctrl/Alt click or one swipe sets are one undo step ("Mute Only", "Mute All", "Unmute All", "Mute Tracks"); solo and record enable are local view state and have no undo. Ctrl rather than Alt for "only this one" keeps the solo click it already had (F-TRK-10).
- **Decision D-UI-26:** markers 10 to 19 are Ctrl with the numeric keypad's 0 to 9. **Rename Marker...** (Shift+') and **Delete Marker** (Alt+Delete) act on the last marker at or before the playhead, since markers have no selection; with none there the status line says so. Ctrl+click on Previous Bar or Next Bar goes to the previous or next marker.
- **Decision D-UI-27:** Z fits the selected regions; Z again, while the selection and the view are as Z left them, fits the whole project. With nothing selected Z fits the whole project as before.
- **Decision D-UI-28:** Left and Right select the previous or next region on the same track (the track of the selected regions, or the selected track; from the playhead when no region is selected). In the piano roll they select the previous or next note, in order of time and then pitch, and sound it when audition is on. Moving notes by the grid moves to Alt+Left and Alt+Right, the keys that nudge regions.
- **Decision D-UI-29:** while the Tool menu (T) is open one key picks a tool: T Pointer, P Pencil, I Scissors, J Glue, E Eraser, M Mute (and the D-UI-36 tools: N Text, Z Zoom, A Fade, G Gain, Q Quantize). The menu shows each key. The keys mean this only inside the menu.
- **Decision D-UI-30:** Shift during a tempo-change drag keeps it to the direction it set off in once it has gone 4 px: sideways keeps the tempo, up and down keep the position (as D-W3-10 for regions).
- **Decision D-UI-31:** while anything is soloed, the Mute button of every track the solo silences (not soloed, and not feeding or fed by a soloed track) shows a faint mute tint. It doesn't flash, so it stays readable for long sessions.
- **Decision D-UI-32:** the New Track dialog has, besides type, count and channels: **Input** (No input, or the device's inputs in the chosen format; Input 1 or 1-2 by default) with **Ascending** (several tracks take the next inputs in turn; a track whose input would be past the device's last gets none), **Output** (Master or an existing bus), **Instrument** (instrument tracks; Weft Keys by default), **Input monitoring** (audio: monitoring On instead of Auto) and **Record enable**. Everything but record enable, which is local, is in the tracks' one undo step. Right-click in the header column below the last track offers New Track..., New Audio Track, New Instrument Track and **Delete Unused Tracks**, which deletes every audio and instrument track that has no regions (buses and the master stay; a track locked by someone else stays) as one undo step; it is also in the Track menu.
- **Decision D-UI-33:** the file browser's header has **Back** and **Forward** (the last 50 folders shown; going somewhere new drops the forward ones), **Up**, and two places: **Home** (the user's home folder) and **Project** (the open project's folder). Below them, beside the folder name, a **Search** field shows only files whose name holds every typed word, any case; folders always show so you can keep moving, and Esc clears it. At the bottom, **Play** previews the selected audio file (Stop while one plays; a click on a file still previews it) and a slider sets the preview volume from -60 dB (silent) to 0 dB, for this session only.
- **Decision D-UI-34:** **New Track with Next Input** (Ctrl+Shift+N, Track menu, and Alt+click on the **+** button) adds one track like the selected audio or instrument track below it and selects it, as one undo step ("New Track"): same kind, channel count, output and (audio) monitoring, with an audio track on the input after the selected one's (none past the device's last input, or when the selected one has none) and an instrument track with a fresh copy of the same instrument (its kind, not its settings). With a bus, the master or nothing selected it only says "Select an audio or instrument track first." **Search and Select Track...** (Ctrl+Alt+F, Track menu) opens a search field over the track list (every track but the master, hidden ones marked): every typed word must be in the name, any case; Enter selects the highlighted track and scrolls it into view.
- **Decision D-UI-35:** Navigate has four submenus. **Go To:** Position... (Ctrl+G, edits the position display), Cycle Start (Ctrl+Home), Cycle End (Ctrl+End), Selection Start (Shift+Home) and Selection End (Shift+End) — the selected notes while the piano roll has the keys, else the selected regions — and Last Locate Position. **Play From:** Beginning, Cycle Start, Selection (Shift+Space), Left Window Edge (the arrange view's left edge), Previous Bar (the bar before the playhead's) and Last Locate Position. **Stop And:** go to the Beginning, the Cycle Start or the Last Locate Position (stopped already, they just go there). **Cycle Range:** Move Cycle Forward / Back by Its Length (Ctrl+Shift+Right/Left; back stops at tick 0), Double and Halve Cycle Length, each one undo step. The cycle commands work on the cycle range whether Cycle is on or not, and say "Set a cycle first." without one. The last locate position is the last place the playhead was put (a click, a go-to, a locate) or playback started from. Right-click on Play opens the Play From list and on Stop the Stop And list.
- **Decision D-UI-36:** the tracks area's Tool menu adds five tools, each with its key while the menu is open: **Text** (N) — a click on a region asks for its new name; **Zoom** (Z) — drag a box to fit its time range in the view, click to zoom in 2× around the pointer, Alt+click to zoom out; **Fade** (A) — on an audio region, a drag that starts in its left half sets the fade in and in its right half the fade out (one undo step "Fade"); **Gain** (G) — dragging up or down on an audio region changes its gain by 0.25 dB a pixel (to +24 dB; -60 dB and below is silence), and every selected audio region with it when it is one of them (one undo step "Region Gain"); **Quantize** (Q) — a click on a MIDI region quantizes its notes with the piano roll's quantize grid, strength and swing (the selected MIDI regions when it is one of them). **Next Tool** and **Previous Tool** (Edit menu) go through the Tool menu's order and wrap.
- **Decision D-UI-37:** **Configure Track Header** (Track menu, Alt+T, and a submenu of the header's and the header column's right-click menus) sets what the track headers show, for this machine only (not saved in the project): the optional **On/Off**, **Input Monitoring** (audio tracks) and **Hide** buttons (off by default), and **Volume**, **Pan**, **Track Numbers** and **Color Bars** (on by default) and **Track Icons** (off by default; one glyph per track kind, in the track's colour). The buttons sit left to right On, M, S, R, I, H. On/Off and Input Monitoring take the same gestures as Mute: Ctrl+click (On/Off: only this track on), Alt+click (every track takes the new state) and a swipe down the column, each one undo step; Input Monitoring switches between On and Auto. The header column's right edge drags to resize it (160 to 480 px, default 208); double-click it for the default. **Turn All Tracks On/Off** (Track menu and the header column's menu) turns every audio and instrument track on when any is off, otherwise turns them all off, as one undo step; a track someone else has locked stays as it is. Not built: control-surface bars, groove track, track alternatives, the pan knob's target menu, an extra name column, and a level meter in the track number.
- **Decision D-UI-38:** zoom and navigation. Two small sliders share the tracks area's caption row: **horizontal zoom** (a log scale from the furthest-out to the closest zoom, keeping the view's left edge) and **vertical zoom** (every track's height, 24 to 400 px). **Zoom Vertical In / Out** (Ctrl+Down / Ctrl+Up) make every track 25% taller or 20% shorter. **Save as Zoom 1-3** (Ctrl+Alt+Shift+1-3) keep the horizontal zoom and position, the track heights and the vertical scroll; **Recall Zoom 1-3** (Ctrl+Alt+1-3) bring them back (an unsaved one only says so). **Navigation: Back / Forward** (Ctrl+[ / Ctrl+]) step through the views before and after each zoom command (Zoom In/Out, Zoom to Fit, vertical zoom, a recalled zoom, a slider drag), up to 50. **Page Left / Right** (Alt+Page Up / Alt+Page Down) move the view by its width, never before the start; **Page Up / Down** scroll the tracks by the view's height. **Zoom Selected Track** (Ctrl+Alt+Z) makes the selected track about three quarters of the view's height, and again returns it to its height. Ctrl+drag on a track header's lower edge sets every track's height. All of these are view state, not saved, and are in View > Zoom and Navigation. Not built: a waveform zoom button, pinch gestures, and the last-clicked-parameter keys.

## 12. Dialogs and windows (MVP list)

New Track · Project Settings · Preferences (Audio, MIDI, General, Recording, Collaboration, Plugins) · Audio Setup (first run) · Bounce · Plugin Manager · Missing Plugins · Missing Media / Relink · Share · Sign In · Recover Unsaved Changes · About & Licenses · plugin editor windows (floating, always on top of main window, one per plugin instance; position remembered).

## 13. Accessibility and input (MVP baseline)

- Every control has a tooltip with name and shortcut (F-UI-10).
- Tab order is defined within dialogs; all dialogs are fully keyboard-operable.
- Colour is never the only carrier of meaning: muted regions are also desaturated *and* labelled in the inspector; collaborator presence uses initials as well as colour; meters use position as well as colour.
- Full screen-reader support is v2 (F-UI-11), but controls are built with accessible names from day one.

---

## Decisions in this file

| ID | Decision |
|---|---|
| D-UI-1 | Side panel Ctrl+L; Add marker is semicolon. |
| D-UI-2 | Add Comment is Shift+C. |
| D-UI-3 | Chat time links: "bar N", "bar N beat M", "m:ss". |
| D-UI-4 | Region handles: lower corners trim; upper-left fades in (audio) or trims (MIDI); upper-right in the name bar fades out (audio), below it loops; the "Take n/m" label switches takes. |
| D-UI-5 | The audio editor's start trim sets the source offset to the exact sample and keeps the region's end; the region start moves to the nearest tick. |
| D-UI-6 | Z with nothing selected fits the project from its start to the end of the last region. |
| D-UI-7 | Piano roll keys act on notes after the piano roll is clicked; Alt+Left/Right move notes by its grid (D-UI-28). |
| D-UI-8 | Piano keys always sound; audition plays notes up to 1 s on the track's instrument. |
| D-UI-9 | Inspector note fields set every selected note; velocity shows a range when mixed. |
| D-UI-10 | Strip details: Monitor cycles Auto/On/Off; empty instrument slot shows Weft Keys; fader law and typed levels. |
| D-UI-11 | Plug-in windows: title, editor choice, positions kept for the session. |
| D-UI-12 | Automation lane layout and point editing; mixer controls follow automation. |
| D-UI-13 | Design tokens, 8 px grid, IBM Plex type scale, one accent, depth; lanes 56 px. |
| D-UI-14 | Waveform overview pyramid and crisp outline drawing. |
| D-UI-15 | Mixer meters (RMS and peak, shared fader scale), reset gestures, value tag. |
| D-UI-16 | Inspector channel section: EQ thumbnail, the mixer's strips for the track and its output, independent meters. |
| D-UI-17 | Stop while stopped goes to the cycle start, then the project start; Play starts at the cycle start when Cycle is on; Count-in command (Shift+K). |
| D-UI-18 | Deselect All (Ctrl+Shift+A) and Invert Selection (Ctrl+Shift+I), for regions or, in the piano roll, notes. |
| D-UI-19 | New Audio Track / New Instrument Track without the dialog; keys for the selected track's Mute, Solo and Record-enable. |
| D-UI-20 | Markers show as flags on the ruler while the global tracks are hidden. |
| D-UI-21 | A narrow control bar keeps the count-in and CPU reading in a More menu. |
| D-UI-22 | Inspector with several regions selected: "N regions", one undo step per edit, controls limited to what every selected region has. |
| D-UI-23 | Right-click menus in the marker, tempo and signature lanes (rename, colour, edit, delete, add here) replace delete-on-right-click. |
| D-UI-24 | Plug-in windows have a header strip with a Bypass button for insert effects. |
| D-UI-25 | Ctrl+click on Mute, Solo or Record: only this track; Alt+click: all tracks; drag across a column to set several. |
| D-UI-26 | Markers 10–19 on Ctrl+keypad; Rename and Delete Marker act on the marker at or before the playhead; Ctrl+click Previous/Next Bar goes by marker. |
| D-UI-27 | Z toggles between fitting the selection and fitting the whole project. |
| D-UI-28 | Left/Right select the previous or next region or note; Alt+Left/Right move notes. |
| D-UI-29 | One-key tool picks while the Tool menu is open. |
| D-UI-30 | Shift locks a tempo-change drag to one direction. |
| D-UI-31 | Mute buttons of tracks a solo silences show a faint tint. |
| D-UI-32 | New Track dialog input/output/instrument/monitoring/record-enable; header-area menu; Delete Unused Tracks. |
| D-UI-33 | File browser Back/Forward, Home and Project, name search, Play/Stop and preview volume. |
| D-UI-34 | New Track with Next Input (Ctrl+Shift+N, Alt+click +); Search and Select Track (Ctrl+Alt+F). |
| D-UI-35 | Navigate › Go To, Play From, Stop And, Cycle Range; Play and Stop right-click menus. |
| D-UI-36 | Text, Zoom, Fade, Gain and Quantize tools; Next/Previous Tool. |
| D-UI-37 | Configure Track Header; On/Off, Input Monitoring and Hide buttons; header column width; Turn All Tracks On/Off. |
| D-UI-38 | Zoom sliders, vertical zoom, saved zooms, navigation back/forward, paging, Zoom Selected Track. |
