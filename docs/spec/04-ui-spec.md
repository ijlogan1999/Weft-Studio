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
2. **Transport buttons:** Go to start, Rewind, Fast forward, Stop, Play, Record, Cycle. Record is visually distinct. Each shows state (lit when active).
3. **Main display (LCD-like readout, own design):**
   - Position: bars.beats.division.ticks and time h:mm:ss.ms (click either to type a position; Enter jumps).
   - Tempo (BPM) at the playhead: drag vertically or double-click to type; edits the tempo point in effect at the playhead.
   - Time signature at the playhead: click to edit.
   - Cycle range start/end when cycle is on.
4. **Mode buttons:** Metronome, Count-in (with dropdown 0/1/2 bars), Autopunch, Snap (with grid value dropdown), Follow playhead.
5. **Meters:** CPU (audio thread load %), Disk; both turn amber > 70%, red > 90% or on dropout. Clicking shows a detail popup with dropout count since session start.
6. **Presence and sharing:** avatars of connected collaborators (initials in their color; up to 5, then "+N"), a sync state indicator (see §10), and a **Share** button.

## 4. C · Inspector (left)

Shows properties of the current selection in collapsible sections:

1. **Region section** (when regions are selected): name, color, mute, gain (audio), fades and curves (audio), quantize/strength/swing (MIDI), loop on/off, transpose and velocity offset (MIDI; v2 UI). Multi-selection shows mixed values as "—"; editing sets all.
2. **Track section** (selected track): name, color, input, output, monitor mode, automation mode, lock state.
3. **Channel strips:** two compact channel strips side by side: the selected track's strip, and the strip of its output destination (Master or a bus). Each has instrument slot (instrument tracks), insert slots, send slots, output button, pan, fader, meter, mute/solo. These are the same controls as the mixer (§8) and edit the same data.
4. **Note section** (in piano roll focus with notes selected): pitch, velocity, length, position, channel.

## 5. D · Global tracks

Lanes above the tracks area, sharing its timeline:

- **Ruler:** bar numbers (and optionally time). Click to move playhead. A thin strip at the top of the ruler holds the cycle range (drag to create; drag edges; click to toggle) and the punch range (shown when autopunch is on).
- **Marker lane:** markers as labeled flags. Double-click empty lane to create at that position; drag to move; double-click label to rename; right-click to delete or set color. Clicking a marker jumps the playhead. Collaborator comments also appear here as pins (§9.3) when the "Comments on timeline" toggle is on (default on).
- **Tempo lane:** step graph of tempo; points draggable (W-11).
- **Signature lane:** meter changes as labels at bar starts.

## 6. E · Tracks area

### 6.1 Track header (left column)
Per track, from left: color strip, track number, name (double-click to rename), mute **M**, solo **S**, record-enable **R** (audio/instrument only), input monitor (audio only), lock icon (shown when locked; clickable to lock/unlock), and a small fader/pan pair when the header is tall enough (≥ 64 px). When automation view is on: parameter selector and mode selector.

Presence on headers: when a collaborator is editing on a track (their last edit or selection is on that track within the last 10 s), a small avatar badge in their color appears on the header. A red pulsing badge means they're recording on it.

Header right-click menu: New track (same type), Duplicate, Delete, Color, Lock/Unlock, Show automation.

Header drag: reorder. Header area below the last track: double-click creates a new track (opens New Track dialog).

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
- **Decision D-UI-7:** the piano roll's keys (arrows, Ctrl+X/C/V, Delete, Ctrl+A, Ctrl+M, Q, Esc) act on notes once the piano roll has been clicked; clicking the tracks area gives them back to regions. Left/Right move the selected notes by the piano roll's grid (1/16 when its snap is off); in the tracks area plain arrow keys do nothing.
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
| Previous / next marker | Alt+, / Alt+. | | Go to marker 1–9 | 1–9 |
| Repeat regions | Ctrl+R | | | |

**Decision D-UI-1:** side panel is **Ctrl+L**; add marker is **;** (semicolon). Single-letter shortcuts are active only when focus is not in a text field.

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
| D-UI-7 | Piano roll keys act on notes after the piano roll is clicked; Left/Right move notes by its grid. |
| D-UI-8 | Piano keys always sound; audition plays notes up to 1 s on the track's instrument. |
| D-UI-9 | Inspector note fields set every selected note; velocity shows a range when mixed. |
