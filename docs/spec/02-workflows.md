# 02 · Core Workflows

Step-by-step behavior of Weft Studio's core workflows. Each workflow lists preconditions, the user's steps, the system's response, edge cases, and decisions. "The system" means the Weft client unless the relay server is named. Collaboration effects are noted where a workflow touches shared state; the full model is in `05-collaboration-spec.md`.

Shortcuts are Weft defaults (see `00-README.md`).

---

## W-0 · First run and audio device setup

**Covers:** F-UI-7, F-UI-8, F-UI-14.

1. On first launch the system shows the Audio Setup dialog before the main window.
2. The dialog lists driver types available on the machine: *WASAPI (shared)*, *WASAPI (exclusive)*, and *ASIO* (only if at least one ASIO driver is installed).
3. Default selection: WASAPI shared, system default output device, system default input device, the device's current sample rate, buffer 480 samples at 48 kHz (10 ms) or nearest supported.
4. The user may change driver, devices, sample rate, and buffer size. The dialog shows the resulting round-trip latency in ms as reported by the driver.
5. **Test** plays a 1 kHz sine at −18 dBFS for 1 second on outputs 1–2.
6. **Continue** saves the settings and opens the Welcome screen (new project / open project / recent).
7. If no output device can be opened, the system still opens the main window in *No audio device* mode: transport plays silently against a system clock, recording is disabled, and a banner offers to open Audio Setup.

**Edge cases**
- Device unplugged during a session: transport stops, a banner says the device was lost, and the system retries opening the same device every 2 s; the user can pick another in Preferences. No document state is lost.
- Sample rate mismatch between project and device: see D-W0-1.

**Decisions**
- **D-W0-1.** If the device cannot run at the project's sample rate, the system opens the project, warns, and plays through a high-quality resampler at the device rate. Recording is disabled until the rates match. (Simplest behavior that never blocks opening.)
- **D-W0-2.** WASAPI shared is the default because it works on every Windows machine without exclusive access. ASIO is recommended in the dialog's help text for low latency.

---

## W-1 · Record audio

**Covers:** F-REC-1, F-REC-2, F-REC-4, F-REC-5, F-REC-8, F-REC-9, F-REC-14, F-REC-15, F-REC-16, F-TRN-2, F-TRN-7, F-TRN-8.

**Preconditions:** an audio device with at least one input is open; project exists.

### W-1a · Single take, linear

1. User creates an audio track (Track › New Track › Audio, or "+" in the tracks header). The New Track dialog asks: format (mono/stereo, default mono), input (default input 1, or 1–2 for stereo), output (default Master), count (default 1).
2. User clicks the track's **Record Enable** button. The button lights red. The track's meter shows input level (F-REC-14). If input monitoring is set to Auto (default), the input is audible through the channel strip only while the transport is stopped-and-armed or recording. **Decision D-W1-1:** Auto monitoring is on for armed tracks by default, because headphone users expect to hear themselves; users with hardware monitoring can switch it off globally.
3. User places the playhead (click the ruler) and presses **R** (or the Record button).
4. If disk space remaining for the armed tracks at current format is under 10 minutes, the system shows a warning with **Record anyway / Cancel** (F-REC-16).
5. If count-in is enabled (default: 1 bar), the system plays the metronome for one bar *before* the playhead position; the playhead starts one bar earlier and recording begins when it reaches the original position. The count-in clicks are not recorded.
6. During recording:
   - A red "recording" region grows on each armed track from the record start point, with a live waveform overview.
   - The metronome sounds if enabled for recording (default: on during record, off during play).
   - Existing regions on the armed track keep playing until the recording region reaches them; from that point the track plays the new recording only (new region is on top).
7. User presses **Space** (stop) or **R** again (stop recording but keep playing; **Decision D-W1-2:** pressing R while recording stops recording and continues playback; Space stops both).
8. On stop the system:
   - Finalizes a WAV (BWF) file per armed track at the project sample rate and recording bit depth (F-REC-15). The file is written to `Recovery/` while recording, then moved into `Media/` under its content hash (D-W10-1), with the display name `<TrackName>#<n>` recorded as its original name.
   - Shifts the recorded data earlier by the total round-trip latency (driver input latency + driver output latency + user "recording delay" offset, default 0) so the recording lines up with what the performer heard (F-REC-9).
   - Hashes the file (BLAKE3) and registers it as a media blob (see 03 §Media, 05 §8).
   - Creates a region whose start is the record start position and whose length is the recorded duration.
   - The new region sits on top of any region it overlaps; overlapped portions of older regions are hidden (not deleted). **Decision D-W1-3:** in linear recording over existing material, the newer region simply covers the older region where they overlap; the older region is not trimmed. Moving the new region away reveals the old one. This is the simplest non-destructive behavior.
   - Pushes one undo step: "Record".
9. If the recording lasted less than 100 ms, no region is created and the file is deleted.
10. **Decision D-W1-7:** the display name's `<n>` is one more than the highest `<TrackName>#<n>` already in the project's media, so names stay unique after deletes and across collaborators' recordings of the same track.

### W-1b · Multitrack recording

Same as W-1a with several tracks armed. Each armed track records its own input. Two armed tracks may share an input (both get identical audio). All files start at the same sample. One undo step covers all tracks.

### W-1c · Cycle recording (take folders)

1. User enables Cycle (C) and sets a cycle range by dragging in the ruler's cycle strip.
2. User presses R. Count-in plays (if enabled) before the cycle start.
3. Recording continues across passes. At each wrap from cycle end to cycle start, the system ends the current take and starts a new one.
4. On stop, for each armed track the system creates one **take folder** spanning the cycle range, containing one take per completed pass plus the partial final pass if it lasted ≥ 1 beat. The most recent take is active (plays); others are muted and visible when the folder is expanded. **Decision D-W1-8:** the first take starts where recording started (the folder starts there if that's before the cycle start); "1 beat" is a beat of the time signature at the cycle start.
5. The take folder header shows "Take N of M"; clicking a take makes it active.
6. All takes live in one continuous WAV file per track for the whole recording; each take is a region referencing a different offset in that file. **Decision D-W1-4:** one file per track per recording pass is simpler for the disk-writing thread and blob sync than one file per take.
7. Comping (selecting parts of different takes) is v2 (F-REC-6). In MVP a take folder plays exactly one whole take.

### W-1d · Punch in/out

1. User enables Autopunch (toolbar button or Ctrl+Alt+P). A red punch range appears in the ruler, defaulting to the cycle range or 4 bars from the playhead.
2. User starts recording before the punch range. The transport plays normally; armed tracks play existing material.
3. At punch-in the system starts writing audio; at punch-out it stops writing. The transport keeps going until stopped.
4. The result is a new region covering exactly the punch range (plus 10 ms on each side, faded, see D-W1-5) on top of existing material.
5. **Decision D-W1-5:** the punched region gets 10 ms of pre/post handle with 10 ms crossfades against the material underneath, to avoid clicks.

**Collaboration notes**
- Recording is local. While recording, the system emits presence "recording on track X" so others see a red indicator on that track.
- The created region and media reference become a CRDT change at stop time. The audio blob uploads in the background (05 §8).
- If a collaborator deletes the track during your recording, your recording still finishes; on stop, the region is added to a newly created track named "<old name> (recovered)". **Decision D-W1-6.**

---

## W-2 · Record MIDI

**Covers:** F-REC-3, F-REC-5, F-REC-7, F-TRK-2, F-PLG-9.

1. User creates a Software Instrument track. Default instrument: Weft Keys (built-in). Default MIDI input: All inputs, all channels.
2. Selecting the track (or arming it) makes it receive live MIDI: played notes sound through its instrument. **Decision D-W2-1:** the selected instrument track receives live MIDI even when not armed (common DAW behavior that lets users noodle immediately). Arming additional instrument tracks makes them receive too.
3. User presses R. Count-in plays if enabled.
4. During recording, incoming MIDI events are timestamped to the sample position at which they arrived, adjusted by the input latency of the MIDI driver (assumed 0 for MVP; configurable offset), and converted to ticks via the tempo map.
5. A growing MIDI region shows notes as they're played. Note-ons with no note-off yet are drawn extending to the playhead.
6. On stop:
   - Hanging notes (note-on without note-off) are closed at the stop position.
   - Linear recording creates a new region from the bar containing the first event's start to the bar end after the last event's end (rounded outward to bars). **Decision D-W2-2:** region boundaries round to whole bars because users then get tidy, loopable regions.
   - If nothing was played, no region is created.
   - One undo step: "Record".
7. Cycle recording with **Merge** (default for cycle, F-REC-7): every pass adds its notes into the same region, so users can build a drum pattern layer by layer. With **Take folders** mode (option), each pass creates a take as for audio.
8. Linear recording over an existing MIDI region with **Replace** (default for linear): a new region on top, like audio. With **Merge**: notes merge into the existing region (region extends if needed). **Decision D-W2-4:** "the existing region" is the topmost unlooped MIDI region on the track that overlaps the recorded span; if the new notes start before its sequence does, its notes shift so positions stay non-negative. Cycle recording uses Merge or Replace in MVP; the Take folders option for MIDI arrives with the piano roll (M5).

**Edge cases**
- Sustain pedal (CC64) held at stop: a CC64 value 0 is appended at the stop position.
- MIDI arrives on several channels: stored with their channel. Instrument receives them all.
- Tempo change during recording: positions are converted with the tempo map at each event's position.
- **Decision D-W2-3 (F-REC-17):** keyboard MIDI uses `A W S E D F T G Y H U J K O L P ;` for C to E an octave up, starting at C3 (60); Z/X move the octave down/up (±5), C/V lower/raise the velocity by 20 (starting at 100, clamped 1–127). Key repeat never retriggers; a key's release ends the note it started even after an octave change; turning the mode off releases held notes. Keys the mode doesn't use keep their commands.

---

## W-3 · Edit regions in the arrange view

**Covers:** F-ARR-1 … F-ARR-27, F-TRK-*.

All region edits are **non-destructive**: they change region metadata (position, offset, length, gain, fades), never source audio files. Every completed gesture is one undo step.

### W-3a · Select

- Click a region: selects it, deselects others. Shift+click toggles. Drag on empty space: rubber-band select of regions it touches. Ctrl+A: all regions. Clicking empty track area: deselect all.
- Selection is local UI state but is published as presence (05 §5).

### W-3b · Move

1. User drags a selected region. All selected regions move together, keeping relative offsets.
2. Horizontal movement snaps the *region start* to the grid (F-ARR-2). Holding Alt disables snap. **Decision D-W3-1:** snapping uses region start, not the pointer position, so an off-grid region snaps to grid on first move.
3. Vertical movement moves regions to other tracks of a compatible type. If any region would land on an incompatible track, that region stays on its original track. Dragging below the last track creates a new track of the right type.
4. A tooltip shows the new position and track while dragging.
5. Overlaps after the move follow D-W1-3 (later-moved region on top). **Decision D-W3-2:** "on top" is determined by a z-order integer per region; any move or creation sets z to max+1 on that track.
6. Esc during drag cancels.

### W-3c · Copy

Ctrl+drag copies. Ctrl+C then Ctrl+V pastes at the playhead onto the selected track (first selected region's track if none), preserving relative positions and tracks.

### W-3d · Trim

1. Hovering a region's lower-left or lower-right corner shows the trim cursor.
2. Dragging the left edge moves region start and source offset together (the audio stays in place on the timeline). Dragging the right edge changes length.
3. Audio regions cannot be extended beyond their source file's bounds. MIDI regions can be extended freely (empty space).
4. Trimming snaps to grid unless Alt held.
5. Minimum region length: 1 tick for MIDI, 64 samples for audio.

### W-3e · Split

- Ctrl+T splits all selected regions at the playhead (or all regions on selected tracks at the playhead if none selected and the playhead crosses them).
- The scissors tool splits the clicked region at the click position (snapped).
- Splitting an audio region produces two regions referencing the same file with adjusted offsets. Existing fades: the left piece keeps the fade-in, the right piece keeps the fade-out.
- Splitting a MIDI region: notes starting before the split belong to the left region. **Decision D-W3-3:** notes that cross the split point are kept whole in the left region and are *not* shortened (simplest; avoids changing musical content). An option "shorten notes at split" is v2.

### W-3f · Join

- MIDI: Ctrl+J merges selected MIDI regions on the same track into one region spanning them; events keep absolute positions.
- Audio: if selected regions are contiguous pieces of the same file at their original relative offsets, joining just re-creates one region. Otherwise the system renders the selection (with gain and fades) to a new WAV and replaces the regions with one region. **Decision D-ARR-3:** rendering is acceptable in MVP; the original regions remain recoverable through undo and version history. Contiguous means: same file, each region's `sourceOffset` equals the previous region's `sourceOffset + length`, and its start lies within one sample of the previous region's end. **Decision D-ARR-4:** the joined region keeps the first region's fade-in and takes the last region's fade-out.

### W-3g · Delete

Delete or Backspace removes selected regions. Gap remains. Deleting a take folder deletes all its takes. Media files are not deleted from disk (they may be referenced by history; cleanup is F-PRJ-8).

### W-3h · Fades and crossfades

1. Hovering the upper-left or upper-right corner of an audio region shows the fade cursor. Dragging inward sets the fade length.
2. Fade curve default: equal-power for fades; changeable via region inspector (linear, equal-power, S-curve).
3. Crossfade: with two overlapping regions on a track, the Crossfade command (or fade tool drag across the boundary) sets fade-out on the upper-left region and fade-in on the lower-right region over the overlap length, with equal-power curves.
4. Fade-in + fade-out length can never exceed region length; if a trim shortens the region, fades shrink proportionally.

### W-3i · Gain, mute, loop, rename, color

- Region gain: inspector field or Ctrl+drag vertically on the region's gain line (dB, −inf to +24).
- M toggles mute on selected regions.
- Loop: dragging the upper-right edge of a region extends a loop. Loop iterations are drawn lighter. Editing the source updates all iterations. Loop length = region length; loops end at the drag point, possibly mid-iteration.
- Double-click region name (inspector) to rename. Color picker in inspector.

### W-3j · Undo/redo

- Ctrl+Z undoes the last of *your* changes; Ctrl+Y redoes.
- A drag is one step (not one per mouse move). Typing a value is one step on commit.
- Undo history is per user and per session; it is not persisted across app restarts (version history covers long-term recovery). **Decision D-W3-4.**
- Undo of an edit to an object that a collaborator has since deleted is skipped with a status message "Can't undo: item was deleted by <name>". See 05 §6.

---

## W-4 · Piano roll

**Covers:** F-PRL-1 … F-PRL-15.

1. Double-clicking a MIDI region (or pressing P with it selected) opens the piano roll in the bottom editor area showing that region. Selecting several regions on one track shows all of their notes; the region boundaries are drawn as vertical lines.
2. Vertical axis: keyboard C-2 (MIDI 0) to G8 (127). Middle C (60) is labeled C3. **Decision D-W4-1:** octave naming uses C3 = 60, a common DAW convention; configurable to C4 = 60 in v2.
3. Horizontal axis matches the arrange ruler; the piano roll has its own zoom and grid.
4. **Create note:** pencil tool click, or Ctrl+click with the pointer tool on empty grid. Starts at the snapped click position, length = last used length (default 1/16), velocity = last used (default 100), channel = region default (1).
5. **Audition:** clicking an existing note or creating one plays it through the instrument at its velocity for its length (max 1 s) when audition is on (default on).
6. **Move:** drag horizontally (snaps note start to grid) and vertically (semitones). Up/Down arrows transpose selected notes by a semitone; Shift+Up/Down by an octave; Left/Right move by grid value. Notes cannot go below 0 or above 127; a transpose that would push any selected note out of range is clamped for the whole selection (the move stops at the boundary).
7. **Resize:** drag a note's right edge to change length (snapped); drag left edge to change start keeping end fixed. Minimum length 1 tick.
8. **Delete:** Delete key or eraser click.
9. **Velocity:**
   - Note fill brightness/hue encodes velocity (low = cool/dim, high = warm/bright, in Weft's palette).
   - Velocity lane under the grid shows one bar per note. Dragging a bar sets that note's velocity; dragging across bars with the pencil draws a line.
   - Velocity tool: drag vertically on any selected note to change all selected velocities by the same offset, clamped to 1–127.
   - Inspector shows the selected note's velocity, or the range "64–110" for multi-selection; typing sets all.
10. **Controller lanes:** a lane selector picks Velocity, Mod (CC1), Sustain (CC64), Pitch Bend, Aftertouch, or any CC number. Click adds a point; drag moves; Delete removes. Pitch bend is drawn centered.
11. **Note mute:** Ctrl+M mutes/unmutes selected notes.
12. Edits outside the region's current bounds: creating a note after the region end extends the region to the next bar boundary. Notes before region start are not allowed (the click is ignored).
13. The piano roll follows the playhead when "follow" is on.

**Collaboration notes:** collaborators editing the same region see each other's pointer and selected notes in their colors (05 §5). Concurrent edits to different notes merge; concurrent edits to the same note's same field resolve last-writer-wins by CRDT rules (05 §4).

---

## W-5 · Quantize

**Covers:** F-PRL-9, F-QNT-1, F-QNT-2.

### Definitions
- **Grid value** *g* in ticks: 1/4 = 960, 1/8 = 480, 1/16 = 240, 1/32 = 120, 1/8T = 320, 1/16T = 160; also 1/1, 1/2, 1/64.
- **Strength** *s* ∈ [0, 1]. **Swing** *w* ∈ [0, 1] (0.5 = straight in UI terms "0%"; see below).
- For a note start *t*, the target grid point *q* is the nearest multiple of *g* (ties go earlier).
- **Swing** shifts every second grid point (odd-indexed points) later by `swingAmount × g`, where `swingAmount = (swingUI / 100) × (1/3)`. So swing 100% turns straight 16ths into triplet-feel 16ths (the off-beat lands at 2/3 of the pair). **Decision D-W5-1:** swing is defined so 0% = straight and 100% = full triplet feel; 50% is a moderate shuffle.
- New start: `t' = t + s × (q_swung − t)`.

### Destructive quantize (Q)
1. In the piano roll, with notes selected (or none selected = all notes in the shown regions), the user picks a grid value in the editor's Quantize menu and presses **Q**.
2. Each note's start moves to `t'`. Note lengths are unchanged (the note keeps its duration; its end moves with the start). Velocity unchanged.
3. Notes that would move before region start are clamped to region start.
4. One undo step "Quantize".
5. Quantizing twice at 100% strength is idempotent.

### Region (non-destructive) quantize
1. Region inspector has *Quantize: Off | grid*, *Strength*, *Swing*.
2. When set, playback uses quantized positions computed on the fly; the piano roll draws notes at quantized positions with a faint marker at their original position.
3. Stored note positions stay unchanged, so switching back to Off restores the original timing.
4. **Apply Quantize** (region menu) bakes the quantized positions into the notes and sets the region's quantize to Off. One undo step.
5. If both a region quantize setting and a destructive quantize exist, destructive edits operate on stored positions; the playback quantize applies on top.

**Decision D-W5-2:** Only note-on positions are quantized in MVP. Controller events are not moved.

---

## W-6 · Mixer routing

**Covers:** F-MIX-1 … F-MIX-13, F-TRK-17, F-PLG-*.

### Signal flow per channel (fixed order in MVP)

```
input (hardware input / instrument output / bus input)
  → insert 1 … insert N (in slot order, bypassed slots skipped)
  → pre-fader sends
  → volume fader
  → pan / balance
  → mute
  → post-fader sends
  → meter (post-fader)
  → output (Master or bus)
```

- Solo: when any channel is soloed, every non-soloed track channel is silenced, except channels that feed a soloed channel (a soloed bus keeps its sources audible) and buses fed by soloed channels. **Decision D-W6-1:** implement "solo-safe by routing": the set of audible channels is the soloed channels plus everything upstream and downstream of them in the routing graph.
- Mute on a channel silences its output and its post-fader sends; pre-fader sends also mute. **Decision D-W6-2:** mute silences pre-fader sends too (simplest mental model: a muted channel makes no sound anywhere).

### Steps: adding an insert
1. User hovers an empty insert slot on a channel strip; clicking opens the plugin menu (built-in effects first, then VST3 grouped by vendor).
2. Choosing a plugin instantiates it, places it in the slot, and opens its editor window.
3. Clicking a filled slot's left edge toggles bypass; dragging moves it to another slot or (with Ctrl) copies it to another channel; the slot menu offers Remove and Replace.
4. Adding a plugin that reports latency triggers delay compensation (F-MIX-13): every path to the master is delayed so all paths have equal total latency. Delay compensation updates within one buffer of the change, with a short crossfade (10 ms) to avoid clicks. **Decision D-W6-3:** compensation applies to all channels, including live-monitored inputs, in MVP; low-latency monitoring mode (F-REC-10) is v2.

### Steps: choosing outputs
1. The channel strip's Output button shows the current destination (default "Master").
2. Clicking lists Master, existing buses, and "New Bus".
3. Choosing a bus that would create a cycle (the bus's own path leads back to this channel) is disallowed: the menu item is disabled with tooltip "Would create a feedback loop". This check runs on the routing graph including sends.

---

## W-7 · Buses and sends

**Covers:** F-MIX-8, F-MIX-9, F-TRK-4.

1. User clicks an empty send slot on a channel. The menu lists existing buses and "New Bus".
2. Choosing **New Bus** creates a bus channel named "Bus N" (N = next free number), output Master, fader at 0 dB, placed in the mixer to the right of the track channels and listed as a bus track in the arrange view (no regions; can show automation). **Decision D-W7-1:** buses appear in the tracks area so their automation can be edited; their header has no record or region lane.
3. The send appears in the slot. **Decision D-W7-2:** a new send starts at 0 dB (unity) so the user hears the effect immediately; moving the knob adjusts. Alt+click resets to 0 dB.
4. Send mode toggles Post (default) / Pre fader via the send's context menu.
5. Users typically add a reverb or delay insert on the bus. The bus channel processes the sum of all sends.
6. Removing a bus with sends pointing to it: the system asks "Remove bus and N sends?"; on confirm, sends are removed. Channels whose output was that bus are rerouted to Master.
7. A channel may send to the same bus only once.
8. Cycle prevention as in W-6.

---

## W-8 · Automation

**Covers:** F-AUT-1 … F-AUT-6, F-AUT-11.

### Display and drawing
1. Pressing **A** toggles automation view. Each track header gains a parameter selector (default: Volume) and a mode selector (default: Read).
2. The automation lane overlays or sits below the track (user preference: below, default). Additional lanes can be added with the "+" on the lane header.
3. Click on the lane line to add a point at the click position (time snapped to grid if snap is on; value is the click height). Drag points to move (time and value; Shift constrains to one axis). Rubber-band to select; Delete removes.
4. Before the first point and after the last point, the value holds constant at that point's value.
5. A lane with no points uses the parameter's current static value (e.g. the fader position).
6. Segment curve: Alt+drag on a segment bends it (one parameter from −1 to +1, 0 = linear).

### Modes
| Mode | Playback | Touching a control during playback |
|---|---|---|
| Off | Automation ignored; control is static. | Changes static value. |
| Read (default) | Control follows automation. | Control moves momentarily but snaps back on release; nothing written. |
| Touch | Follows automation. | Writes while touching; on release, returns to the existing automation over 200 ms (glide). |
| Latch | Follows automation. | Writes from first touch until transport stops, holding the last value after release. |
| Write | Writes the current control value continuously from play start, erasing existing points along the way, until stop. | Same. |

- After a write pass in Write mode, the track's mode automatically switches to Latch. **Decision D-W8-1:** this avoids accidental erasure on the next play.
- Written automation is thinned on the fly: points are stored only when the value changes by more than 0.1% of range or at least every 50 ms of change, whichever is less frequent. **Decision D-W8-2.**
- A write pass is one undo step.
- Automation reaches the audio thread with sample-accurate interpolation at block boundaries for MVP (block size ≤ 512 samples), with per-sample smoothing for volume and pan to avoid zipper noise. **Decision D-W8-3:** sub-block accuracy for plugin parameters is via VST3 parameter change queues with sample offsets.

### Collaboration notes
A live write pass is local until stop; then the points arrive as one CRDT change. While writing, presence shows "writing automation on <track>".

---

## W-9 · Bounce and export

**Covers:** F-EXP-1 … F-EXP-5, F-EXP-11.

1. User chooses File › Bounce › Mix (Ctrl+B).
2. Dialog fields:
   - **Range:** start and end (default: cycle range if cycle on, else project start to end marker; if no end marker, end of last region).
   - **Include tail:** on (default) per F-EXP-11.
   - **Mode:** Offline (default) / Real-time.
   - **Formats:** one or more of WAV and MP3, each with settings:
     - WAV: bit depth 16 / 24 (default) / 32-float; sample rate project (default) / 44.1 / 48 / 96 kHz.
     - MP3: bitrate 320 kbps CBR (default), 256, 192, 128, or VBR quality 0–9; sample rate 44.1 or 48 kHz (default follows project if supported, else 44.1); tags Title (default project name), Artist (default user display name), Album, Year.
   - **Dither:** Auto (TPDF when bit depth < 24, default) / Off / On.
   - **Destination:** folder and filename (default `<project folder>/Bounces/<project name>.wav`).
3. On **Bounce**, the system renders the master output over the range:
   - Offline: the audio engine renders in a separate render graph instance (the live engine stops), as fast as possible, with progress bar and Cancel.
   - Real-time: transport plays the range and the master output is captured; audio is audible during capture.
   - Plugins that report they don't support offline rendering force real-time with a notice.
4. After render, the system converts and writes each chosen format. Sample-rate conversion (if needed) happens before dither. MP3 encoding uses LAME (LGPL, dynamically linked; see Phase 2 architecture for licensing).
5. On completion a notice offers **Show in folder** and **Add to project** (imports the bounce as a new audio track, muted).
6. If the destination file exists, the system asks Replace / Keep both (adds " (2)").

**Edge cases**
- Clipping: if any sample exceeds 0 dBFS in the float mix, the completion notice says "Bounce clipped (peak +X.X dBFS)". Float WAVs keep the overs; integer formats clip.
- Missing media during bounce (collaborator blob not downloaded yet): the dialog warns "N regions have media not yet downloaded. Wait / Bounce anyway". **Decision D-W9-1.**
- Offline bounce output must be bit-identical between two runs with the same project and settings, provided all plugins are deterministic (built-in ones are). Dither uses a fixed seed per bounce for this reason. **Decision D-W9-2.**

---

## W-10 · Save and load

**Covers:** F-PRJ-1, F-PRJ-3 … F-PRJ-7.

### Project on disk
A project is a folder `<Name>.weft/` containing:
- `project.automerge` (or equivalent CRDT binary): the full document with change history.
- `Media/`: audio files, named `<blake3-hex>.<ext>` plus a sidecar `media-index.json` mapping hashes to original filenames. **Decision D-W10-1:** storing media by hash makes collaboration blob sync and dedupe trivial.
- `Bounces/`, `Cache/` (waveform peak files, safe to delete), `Recovery/`.

Windows Explorer shows the folder as a normal folder; double-clicking `project.automerge` (registered as "Weft Project") opens it. **Decision D-W10-2:** a folder rather than a single file keeps large media out of the document and allows partial blob sync.

### Save
1. Ctrl+S writes the document's current state to `project.automerge` atomically (write temp, flush, rename).
2. Because every edit is already persisted to the recovery change log (F-PRJ-7), Save is fast and mostly marks a "saved" point. The title bar shows an asterisk while there are unsaved changes since the last save.
3. Save As copies the folder to a new location; option "Copy media" (default on) or reference media in the original location.
4. In collaborative projects, the relay is the source of truth for sharing, but the local folder is a full replica; Save still writes locally. **Decision D-W10-3:** "Save" in a shared project also creates a named checkpoint in version history only when the user chooses "Save Version…" (Ctrl+Alt+S); plain Save does not spam history.

### Open / load
1. File › Open (Ctrl+O) shows a folder/file picker filtered to Weft projects.
2. The system loads the document, validates its schema version, migrates older schemas forward (never backward), and builds the audio graph.
3. Plugins are instantiated with saved state. Missing plugins become placeholders (F-PLG-15) and a dialog lists them.
4. Missing media: regions referencing hashes not in `Media/` are drawn with a "missing" pattern; a dialog offers **Locate…** (search a folder for files whose hash matches) or **Continue**. If the project is shared, missing blobs are requested from the relay automatically instead.
5. The transport is at the saved playhead position; view state (zoom, scroll, open editors) is restored from per-user local view state (not in the shared document).
6. Opening a project saved by a newer Weft version: refuse with a message naming the required version. **Decision D-W10-4.**

### Crash recovery
- Every committed change is appended to `Recovery/changes.log` within 1 s.
- On open, if the log has changes newer than `project.automerge`, the system asks "Recover unsaved changes from <time>?" Recover applies them; Discard archives the log.

---

## W-11 · Tempo and time signature changes

**Covers:** F-TMP-1, F-TMP-3, F-TMP-4, F-TRN-9, F-TRN-10.

1. The global tracks area shows a Tempo lane and a Signature lane (toggle via Global Tracks button or G).
2. Clicking in the Tempo lane with the pencil (or Ctrl+click) adds a tempo change at the snapped position with the value under the pointer; dragging a tempo point vertically changes its BPM (20–999, 0.01 BPM resolution); dragging horizontally moves it.
3. The initial tempo (at tick 0) cannot be deleted or moved.
4. Signature changes are added in the Signature lane at bar starts only; dialog asks numerator (1–32) and denominator (1, 2, 4, 8, 16, 32).
5. MIDI regions and automation are anchored in musical time (ticks): when the tempo changes, they move in absolute time. Audio regions are anchored by their start in ticks but their content plays at its own speed (not stretched) in MVP. **Decision D-TMP-1** (see 03).
6. Metronome and grid follow the maps.

---

## Decisions in this file

| ID | Decision |
|---|---|
| D-W0-1 | Device/project sample-rate mismatch: open, warn, resample playback, disable recording. |
| D-W0-2 | Default driver WASAPI shared. |
| D-W1-1 | Auto input monitoring on armed tracks by default. |
| D-W1-2 | R while recording stops recording, keeps playback. |
| D-W1-3 | New recordings cover older overlapping regions without trimming them. |
| D-W1-4 | One WAV per track per recording pass; takes reference offsets. |
| D-W1-5 | Punch regions get 10 ms handles with crossfades. |
| D-W1-6 | Recording onto a track deleted remotely lands on a recovered track. |
| D-W2-1 | Selected instrument track receives live MIDI unarmed. |
| D-W2-2 | Recorded MIDI regions round outward to whole bars. |
| D-W3-1 | Move snaps region start. |
| D-W3-2 | Overlap stacking by per-region z-order. |
| D-W3-3 | Split keeps crossing notes whole in the left region. |
| D-ARR-3 | Joining non-contiguous audio renders a new file. |
| D-ARR-4 | Joined audio keeps the first fade-in and the last fade-out. |
| D-ARR-5 | Smart snap uses the finest note grid whose lines are at least 16 px apart, else whole bars. |
| D-ARR-6 | A take moves, copies and pastes with its whole take folder. |
| D-ARR-7 | Audio dropped on an audio track lands there end to end; other drops make a track per file named after it. Files with more than 2 channels keep the first 2. |
| D-W3-4 | Undo history is per session, not persisted. |
| D-W4-1 | MIDI 60 is labeled C3. |
| D-W5-1 | Swing 0–100% maps from straight to triplet feel. |
| D-W5-2 | Quantize moves note starts only. |
| D-W6-1 | Solo keeps upstream/downstream routing audible. |
| D-W6-2 | Mute silences pre-fader sends too. |
| D-W6-3 | Delay compensation applies to monitored inputs in MVP. |
| D-W7-1 | Buses appear in the tracks area. |
| D-W7-2 | New sends start at 0 dB. |
| D-W8-1 | Write mode reverts to Latch after a pass. |
| D-W8-2 | Automation thinning thresholds. |
| D-W8-3 | Automation interpolation and parameter queues. |
| D-W9-1 | Warn when bouncing with undownloaded media. |
| D-W9-2 | Offline bounce is deterministic (fixed dither seed). |
| D-W10-1 | Media stored by content hash. |
| D-W10-2 | Project is a folder. |
| D-W10-3 | Plain Save doesn't create a history version. |
| D-W10-4 | Refuse to open projects from newer versions. |
