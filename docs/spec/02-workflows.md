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
- **Decision D-W0-3 (M9, setup details):** the Audio & MIDI preferences are the Audio Setup dialog reopened from the File menu, with OK and Cancel instead of Continue; it also lists the MIDI inputs (F-UI-8). The choices are kept per user in `audio-device.json` in the settings folder, and that file's absence is what makes a launch the first one. The Welcome screen opens on every launch, since it carries the recent projects; closing it keeps the empty project Weft starts with. In No-audio mode the engine runs on the UI timer against the system clock, and pressing Record says that recording needs an audio device.
- **Decision D-W0-4 (M9, MIDI inputs):** every connected MIDI input is opened, and inputs switched off in preferences are dropped by device name before they reach the engine. Inputs plugged in later open when the system reports the device list changed. Only channel messages are used for now.

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
   - The metronome sounds if enabled for recording (default: on during record and play, D-W1-10).
   - **Decision D-W1-11 (metronome clicks):** Navigate › Metronome Clicks turns the bar, beat and division clicks on and off on their own (default bar and beat). A division is a quarter of a beat, 6 dB softer than a beat. With bars off, a bar's first beat clicks as a beat. Only During Count-in silences the click once the count-in ends. Tone moves every click from an octave down to an octave up in five steps (default the middle). These are per-user settings like the rest of the metronome (D-DM-12). A count-in always clicks: with every click type off, it counts in with bars and beats.
   - **Decision D-W1-12 (Record Toggle and Replace):** Record Toggle (Alt+R) while playing starts recording right where the playhead is, with no count-in and no jump; while recording it stops recording and playback goes on; stopped, it records as R does. Stop then returns to where playback started. Replace is one setting for MIDI and audio: Navigate › Replace, the Replace button beside Punch in the control bar, and Navigate › MIDI Recording › Replace all switch it. With it on, MIDI recording clears what the pass covered (D-W2-4), and an audio recording deletes the parts of audio regions on its track under it (split at its edges, or the take folder's span for cycle takes) in the same "Record" undo step.
   - **Decision D-W1-13 (Capture as Recording):** while the transport plays without recording, Weft keeps the MIDI it receives (up to 65,536 messages), starting over each time playback starts. Capture as Recording (Ctrl+Shift+R) lays out what's been played and not captured yet as a MIDI recording on the selected instrument track, with the current MIDI Recording options (merge, cycle passes, Replace), as one undo step "Record". Nothing played, or a selected track that isn't an instrument track, says so and does nothing.
   - **Decision D-W1-14 (MIDI auto record enable):** Settings › Audio › "Record-enable the selected instrument track" (off by default): selecting an instrument track record-enables it and disarms the instrument track that was armed that way before; tracks armed by hand stay armed. Nothing changes while recording.
   - **Decision D-W1-15 (left out for now):** Note Repeat, Spot Erase, Record Into Cell, Free Tempo Recording, a flashback buffer for audio, and choices for overlapping audio recordings (they always go on top, or into a take folder when cycling) aren't built.
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
6. **Decision D-W1-9:** each time Autopunch is turned on the range is set again: the cycle range when the cycle is on, else 4 bars from the start of the bar at the playhead. With Autopunch on, dragging either end of the red range in the ruler's top strip moves it, and Shift+drag in the strip sets a new range; each drag is one undo step.

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
4. **Create note:** pencil tool click, or Ctrl+click with the pointer tool on empty grid. Starts at the snapped click position, length = last used length (default 1/16), velocity = last used (default 100), channel = region default (1). **Decision D-W4-2:** the snapped position is the grid line at or before the click (the cell clicked), and dragging on after the click lengthens the new note in the same undo step. **Decision D-W4-10:** clicking an existing note also makes it the "last used" pattern, so the next drawn note copies its length and velocity.
5. **Audition:** clicking an existing note or creating one plays it through the instrument at its velocity for its length (max 1 s) when audition is on (default on).
6. **Move:** drag horizontally (snaps note start to grid) and vertically (semitones). Up/Down arrows transpose selected notes by a semitone; Shift+Up/Down by an octave; Alt+Left/Right move by grid value (Left/Right select the previous or next note, D-UI-28). Notes cannot go below 0 or above 127; a transpose that would push any selected note out of range is clamped for the whole selection (the move stops at the boundary). **Decision D-W4-3:** "clamped for the whole selection" means no note changes pitch (AT-PRL-4: Shift+Up on a note at 120 moves nothing). Moving left stops when the earliest selected note reaches its region's start. Drags preview and commit one undo step on release; Alt drags without snap; Ctrl+drag copies. **Decision D-W4-11:** holding Shift during a note drag (move or Ctrl+drag copy) keeps it to the direction it set off in once it has gone 4 px: along time or across pitches. Releasing Shift frees it again.
7. **Resize:** drag a note's right edge to change length (snapped); drag left edge to change start keeping end fixed. Minimum length 1 tick. **Decision D-W4-12:** with several notes selected, dragging an edge of one of them resizes them all in one undo step ("Resize Notes"): the right edge adds the same amount to every length, the left edge moves every start by the same amount with the ends fixed, and no note gets shorter than the grid value (or its own length, if that is shorter; 1 tick with snap off). Holding Shift while dragging a right edge makes every selected note end where the grabbed note ends; Alt+Shift gives them all the grabbed note's length. An unselected note is resized alone.
8. **Delete:** Delete key or eraser click.
9. **Velocity:**
   - Note fill brightness/hue encodes velocity (low = cool/dim, high = warm/bright, in Weft's palette).
   - Velocity lane under the grid shows one bar per note. Dragging a bar sets that note's velocity; dragging across bars with the pencil draws a line.
   - Velocity tool: drag vertically on any selected note to change all selected velocities by the same offset. **Decision D-W4-13 (changed 2026-11):** the drag stops when the first selected note reaches 1 or 127, so the notes keep their differences. Holding Alt lets it run on, each note stopping at the limit on its own; Alt+Shift sets every selected note to the dragged note's new velocity.
   - **Decision D-W4-14 (note functions):** Edit › Note Functions acts on the selected notes in the piano roll, each as one undo step named after it. **Select All Following** (Shift+F) selects every shown note that starts at or after the earliest selected note (the playhead with none selected). **Legato** (Ctrl+Shift+L) ends each note where the next later-starting selected note starts; notes starting together end together, the last ones keep their length, and it can lengthen or shorten. **Remove Overlaps** ends each note no later than the next note of the same pitch in the shown regions starts. **Note Start to Playhead** moves the start of notes that span the playhead to it, keeping their end; **Note End to Playhead** makes every note that starts before the playhead end at it. **Lengthen** / **Shorten** (Alt+Shift+Right/Left; on regions in the arrange view, D-W3-11) change the notes' length by the note snap value (240 ticks with snap off), never below one tick. With nothing to change they say so and add no undo step.
   - **Decision D-W4-15 (second toolbar row):** under the tools, the piano roll shows the shown regions' names; a **Velocity** slider with the first selected note's velocity, which sets every selected note while dragged (one undo step "Note Velocity") and becomes the pencil's velocity; **Scale root** (C to B), **Scale** (Major, Natural Minor, Harmonic Minor, Melodic Minor, Dorian, Phrygian, Lydian, Mixolydian, Locrian, Major Pentatonic, Minor Pentatonic, Blues) and **Scale Q**, which moves each selected note to the nearest pitch of that scale (halfway between two, the lower one; one undo step "Scale Quantize"); and at the right the pitch and bar position under the pointer, plus the pitch and velocity of the note there. Each note draws a line near its bottom edge as long as its velocity (127 = the whole note).
   - **Decision D-W4-16 (more MIDI region parameters):** the inspector's MIDI region section adds, after Swing, **Q-Range** (ticks: positive, only notes within that distance of their grid point move, so deliberate pushes stay; negative, only notes further away move; 0, all) and **Q-Flam** (notes that land on one point spread that many ticks apart, low to high, or high to low when negative), and after Velocity, **Dynamics** (Fixed, 3:1, 2:1, 1.5:1, 1.25:1, Off, 1:1.25, 1:1.5, 1:2, 1:3: each velocity's distance from 64 is scaled, Fixed plays 64, then the Velocity offset adds), **Gate Time** (Off, Fixed = 30 ticks, 25% to 200% of each note's length, Legato = each note lasts until the next later one starts and the last keeps its length) and **Clip Length** (on by default: notes stop at the region's end; off, they play their whole length). Every region's **Delay** has a unit menu, ms or ticks (±960; ticks follow the tempo; switching sets 0). Audio regions show **Reverse** as a tick box, ticked once the region plays a reversed copy (each click reverses again). Edit › **MIDI Region Parameters** › **Apply All Parameters Permanently** writes the quantize, transpose, velocity, dynamics, gate time, clip length and delay (milliseconds converted at the tempo where the region starts) into the notes and resets them, so it plays the same; **Apply All Except Delay** keeps the delay. One undo step each. Not done: groove templates (Quantize type, Q-Velocity, Q-Length); there is no region MIDI channel, so the "except channel" forms are the two above.
   - Inspector shows the selected note's velocity, or the range "64–110" for multi-selection; typing sets all.
10. **Controller lanes:** a lane selector picks Velocity, Mod (CC1), Sustain (CC64), Pitch Bend, Aftertouch, or any CC number. Click adds a point; drag moves; Delete removes. Pitch bend is drawn centered.
11. **Note mute:** Ctrl+M mutes/unmutes selected notes.
12. Edits outside the region's current bounds: creating a note after the region end extends the region to the next bar boundary. Notes before region start are not allowed (the click is ignored). **Decision D-W4-4:** the same applies to notes moved, resized or pasted past the end: the region grows to the end of the bar holding the note's end. **Decision D-W4-5:** Ctrl+V pastes into the topmost MIDI region under the playhead on the piano roll's track, keeping the notes' relative offsets; with no region there nothing is pasted and the status bar says why.
14. **Scissors** split a note at the snapped point into two notes with the same pitch, velocity and channel (one undo step "Split Note"). **Decision D-W4-6.**
15. **Controller points** snap to the piano roll grid and hold their value until the next point (drawn as steps; no interpolation in MVP). Pitch bend is stored as −8192…8191 and sent as a 14-bit message centred on 8192; aftertouch is channel pressure. Clicking a point selects it and Delete removes it; the eraser or Alt+click also removes points. **Decision D-W4-7.**
16. **Velocity tool:** each pixel of vertical drag is one velocity step. **Decision D-W4-8.**
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
3. Clicking a filled slot's left edge toggles bypass; dragging moves it to another slot or (with Ctrl) copies it to another channel; the slot menu offers Remove and Replace. **Decision D-W6-9:** dropped on a slot of another channel without Ctrl, the plug-in moves there: it keeps its identity, settings and bypass, and its window stays open. With Ctrl the original stays and the target gets a copy with the same settings (also within one strip). The slot it's dropped on sets the position; the empty slot means the end. A channel that already holds 15 refuses. Automation lanes of the plug-in's parameters stay on the channel it left, as when a plug-in is removed. Alt+click on a filled slot removes the plug-in. Each of these is one undo step. **Decision D-W6-10:** Shift+click on a filled slot toggles its bypass (Alt is taken by remove). Pressing a bypass dot and dragging across other slots' dots, in the same strip or others, gives each the state the first took, as one undo step ("Bypass Plug-ins"); free slots are skipped. A filled slot's menu has **Insert Before**, which puts a new plug-in in that slot and moves the rest down, in place of a hover line between slots. **Decision D-W6-11:** **Search and Add Plug-in...** (Ctrl+Shift+P, Mix menu) opens a search field over the plug-in list: every typed word must appear in the name or the maker, any case; Up/Down move through the matches and Enter adds the highlighted one to the selected track, an effect in its next free insert slot (its editor opens, as from the slot menu) or, on an instrument track, an instrument as its instrument. Instruments are listed only for an instrument track. With no track selected the command only says so. **Decision D-W6-12:** a plug-in window's header (after Bypass) has **<**, a **Settings** button that shows the loaded setting's name (with " *" once it has been changed) and opens the Settings menu, **>**, **Compare**, **Copy** and **Paste**. A *setting* is the plug-in's parameters and state chunk saved under a name; settings are kept on this computer per plug-in (in the app's settings folder under "Plug-in Settings", one file each), not in the project, so every project can use them. The menu: Next Setting, Previous Setting (the saved settings in name order, wrapping; also < and >), Copy Setting, Paste Setting (only onto the same plug-in), Reset Setting (every parameter to its default; the state chunk is left), Save Setting (over the loaded one; without one it asks for a name), Save Setting As..., Save Setting as Default, Recall Default Setting, Delete "name", then the saved settings, the loaded one ticked. Loading, pasting, resetting and recalling are each one undo step ("Load Setting", "Paste Setting", "Reset Setting", "Recall Default Setting"). **Compare** is on once the plug-in differs from the setting as loaded or saved: it loads that setting and sets the changes aside, and a second press brings them back (each press an undo step "Compare"); a change made while comparing drops the set-aside changes. A plug-in inserted or swapped in starts from its default setting when one was saved. **Show/Hide All Plug-in Windows** (V, Mix menu) hides the open plug-in windows and shows them again; opening a plug-in window shows them all. The slider list for a plug-in without its own editor can be widened. Not built: separate plug-in undo, Link, side-chain input, hiding the header, and a switch from a plug-in's own editor to the slider list. **Decision D-W6-13:** a channel strip shows a **peak number** beside the volume readout: the highest level since it was last clicked, red over 0 dB; clicking it resets it and the clip light. **Mute Off for All** (Ctrl+Alt+Shift+M) unmutes every track as one undo step. **Clear/Recall Solo** (Ctrl+Alt+Shift+S) keeps the solos aside and clears them; with nothing soloed it brings the kept ones back. **Solo Tracks of Selected Regions** solos exactly the tracks the selected regions are on. **Input Monitoring Track** (Shift+I) switches the selected audio track's monitoring between On and Auto. **Copy / Paste Channel Strip Setting** (Mix menu) copy the selected track's volume, pan and insert effects (each with its settings and bypass) and put them on the selected track in place of its own inserts, as one undo step; sends, input, output and the instrument stay. These live in the Track menu (the solo and mute ones) and the Mix menu (the strip ones). Not built: strip presets saved to disk, a gain-reduction meter, a mono/stereo button, Dim, a per-strip Bounce button, and pre-fader or input metering. **Decision D-W6-14:** a bar above the mixer's strips has the view buttons **Single** (the selected channel and every bus its signal reaches through its output and sends), **Tracks** (the default: the channels as the track list shows them, buses last) and **All** (hidden tracks too), and the filters **Audio**, **Inst** and **Bus**, which show or hide those kinds in Tracks and All; Alt+click on a filter shows only that kind, and clicking a filter in Single goes to Tracks. **Cycle Through Mixer Views** (Mix menu) goes Single, Tracks, All. The view and filters are this machine's, not saved. The master is always pinned at the right. Not built: narrow strips, the other filter kinds, selecting several strips, and a separate mixer window.
4. Adding a plugin that reports latency triggers delay compensation (F-MIX-13): every path to the master is delayed so all paths have equal total latency. Delay compensation updates within one buffer of the change, with a short crossfade (10 ms) to avoid clicks. **Decision D-W6-3:** compensation applies to all channels, including live-monitored inputs, in MVP; low-latency monitoring mode (F-REC-10) is v2. **Decision D-W6-6:** a bypassed insert adds no latency, so bypassing a latent plug-in re-aligns the paths like removing it. Latency of master inserts isn't compensated: everything has already been summed, so there is nothing left to align (the offline render still drops that delay from the start of the file).
5. **Decision D-W6-4:** a mono channel runs its inserts in stereo (the signal copied to both sides) and averages the result back to mono before the fader, so stereo-only plug-ins work on mono tracks.
6. **Decision D-W6-5:** a parameter changed outside automation (a plug-in window, the generic editor, undo, a collaborator) takes effect from the start of the next audio block; only automation is sample-accurate (D-W8-3).
7. **Decision D-W6-7:** muting cuts the channel at once; unmuting brings it back with the fader's one-block ramp from silence, so it doesn't click.
8. **Decision D-W6-15:** **groups.** A track is in at most one group (of up to 32, numbered from 1; a new group takes the lowest free number and the name "Group N"). Each channel strip has a **Group** slot beside a **VCA** slot under its Output: its menu has No Group, the groups (ticked: this track's), New Group (this track in a new group) and Group Settings...; Alt+click puts the track in the group last chosen. **Groups...** (Alt+Shift+G, Mix menu, or Group Settings...) opens a floating Groups window: Groups active, New Group and Delete Group across the top; each group's number, On, name (double-click to rename; Alt+click selects its first track and every region on its tracks) and H (hide its tracks: no rows, no strips); and for the chosen group what it links: Editing (Selection), Quantize-Locked (Audio), Automation Mode, Volume, Mute, Input, Pan, Solo, Record, Color, Track Zoom, Hide, and Send 1 to 8. A new group links volume, mute and solo. A change to a linked control of one member goes to the other members in the same undo step: volume and send levels move by the same number of dB (stopping at -inf and +6; from or to -inf they all take the value), pan by the same amount, and mute, input monitoring (members of the same kind), colour, automation mode and Hide take the same value; solo, record enable and track height are this machine's and follow the same way; with Editing on, clicking a region also selects the regions on the other members that overlap it in time. Quantize-Locked is stored with the group but does nothing yet: audio regions aren't quantized (the stretch engine, W5). Holding **Shift** while changing a control changes only that track (the temporary override); Shift keeps its fine-adjust meaning on a fader too, and key commands that use Shift link as usual. A group turned off, or groups turned off with **Enable/Disable Groups** (Shift+G, Mix menu), links and hides nothing. **Create Group for Selected Tracks** (Ctrl+Alt+G) puts the selected track and the tracks of the selected regions in a new group. **Toggle Hide Group 1** to **9** (Ctrl+Shift+1 to 9, Mix › Hide Groups) toggle that group's H. Tracks locked by someone else are left out of a linked change. Deleting a group leaves its tracks in no group. The window doesn't dock in the inspector, and H doesn't show a dash when only some members are hidden. Not built: a track in several groups, the More menu, and group-linked automation writes (a touch in a live automation mode moves only that track).
9. **Decision D-W6-16:** **VCA faders.** **New VCA Fader** (Mix menu), or New VCA in a strip's VCA slot (which also assigns that track, one undo step), makes a VCA "VCA N" at 0 dB. VCAs have strips in the mixer after the channels: the name (double-click to rename), how many tracks are on it, a fader (-inf to +6 dB) with a readout to type into, and Mute; right-click offers Delete VCA, which unassigns its tracks. A strip's VCA slot lists No VCA, the VCAs and New VCA. A VCA scales each assigned track's output by its level, on top of the track's fader and its automation, and its Mute silences them like their own mute (their meters fall, their mute buttons don't change). Shift+click on the inspector strip's VCA slot shows that VCA's strip in place of the output's, until another track is selected. A VCA carries no sound, has no inserts, sends or automation, and VCAs don't nest; the master can't be on one.

### Steps: choosing outputs
1. The channel strip's Output button shows the current destination (default "Master").
2. Clicking lists Master, existing buses, and "New Bus".
3. Choosing a bus that would create a cycle (the bus's own path leads back to this channel) is disallowed: the menu item is disabled with tooltip "Would create a feedback loop". This check runs on the routing graph including sends. **Decision D-W6-8:** menu items can't carry tooltips, so the disabled item shows the reason in its text: "Bus 2 (Would create a feedback loop)". A send menu also disables a bus the channel already sends to, marked "(already sending)" (step 7 of W-7).

---

## W-7 · Buses and sends

**Covers:** F-MIX-8, F-MIX-9, F-TRK-4.

1. User clicks an empty send slot on a channel. The menu lists existing buses and "New Bus".
2. Choosing **New Bus** creates a bus channel named "Bus N" (N = next free number), output Master, fader at 0 dB, placed in the mixer to the right of the track channels and listed as a bus track in the arrange view (no regions; can show automation). **Decision D-W7-3:** the bus goes at the end of the track order, and "next free number" is the lowest N with no track named "Bus N". New Bus from an output menu routes the channel to the new bus instead of adding a send. Creating the bus and its send (or output) is one undo step "New Bus". **Decision D-W7-1:** buses appear in the tracks area so their automation can be edited; their header has no record or region lane.
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
- **Decision D-W8-4:** what a pass writes. Touch writes each touch from the moment it starts to its release, and a second touch in the same pass writes a second range. Latch writes from the first touch to stop. Write writes every existing lane of the track from play start, holding the value the control showed there until it's touched, plus any other control touched during the pass from its first touch. When the cycle jumps back, the pass keeps what it wrote before the jump and writes nothing more for that control. The Touch glide back is linear over 200 ms. Clicking Mute while its automation is live counts as a touch and a release. Touching a control with nothing to follow while stopped changes its static value, as in Off.
- **Decision D-W8-6 (block-size independence):** while the transport rolls, a volume, pan or send lane that is playing as written (not touched or gliding back) is read at every 32nd sample of the timeline, counted from the project start, and the gain moves in a straight line between those readings. The audio device's buffer size and the bounce block size therefore change nothing, and a bounce matches playback sample for sample. Touches, glides and stopped playback keep the per-buffer ramp. Mute points still switch at the start of a buffer (D-W6-7).
- **Decision D-W8-7 (mode commands and lanes):** View › Automation has **Set All Tracks to Automation Off / Read / Touch / Latch** (each one undo step "Automation Mode"; a track someone else has locked keeps its mode), **Toggle Track Automation Off/Read** (Ctrl+Alt+Shift+R) and **Latch/Read** (Ctrl+Alt+Shift+L) for the selected track, and **Cycle Through Used Automation Parameters** (Ctrl+Alt+Shift+A), which shows automation view and puts the next parameter that has points, in the parameter menu's order, in the track's first lane. Holding Alt while choosing a mode in a header sets every track to it. Each lane has a **×** beside its **+** that closes it (its points stay); closing the last one leaves Volume. With automation points selected, Up / Down change their values by 1% and Shift+Up / Shift+Down by 10%, one undo step each, instead of selecting tracks. Not built: region automation, a trim field, per-parameter on/off, mode colours, the event list and snap offset.
- **Decision D-W8-5:** the master's pan acts as balance, like a stereo track's, and the master's volume, pan, mute and insert parameters can all be automated (F-AUT-11).

### Collaboration notes
A live write pass is local until stop; then the points arrive as one CRDT change. While writing, presence shows "writing automation on <track>".

---

## W-9 · Bounce and export

**Covers:** F-EXP-1 … F-EXP-5, F-EXP-11.

1. User chooses File › Bounce › Mix (Ctrl+B).
2. Dialog fields:
   - **Range:** start and end (default: cycle range if cycle on, else project start to end marker; if no end marker, end of last region).
   - **Include tail:** on (default) per F-EXP-11.
   - **Mode:** Automatic (default) / Offline / Real-time (D-W9-11). **2nd cycle pass:** off (default).
   - **Formats:** one or more of WAV, AIFF, FLAC and MP3, each with settings:
     - WAV: bit depth 16 / 24 (default) / 32-float; sample rate project (default) / 44.1 / 48 / 96 kHz. AIFF and FLAC use the same bit depth and rate (D-W9-8).
     - MP3: bitrate 320 kbps CBR (default), 256, 192, 128, or VBR quality 0–9; sample rate 44.1 or 48 kHz (default follows project if supported, else 44.1); tags Title (default project name), Artist (default user display name), Album, Year; Include tempo (on).
   - **Dither:** Auto (TPDF when bit depth < 24, default) / Off / On. **Normalize:** Off (default) / Overload Protection Only / On (D-W9-7).
   - **Channels:** Interleaved (default) / Split (L and R files) / Mono (D-W9-8).
   - **Destination:** folder (with Choose... and New Folder...) and filename (default `<project folder>/Bounces/<project name>.wav`).
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
- **Decision D-W9-3 (tail end):** the tail is rendered until the output (both channels) has stayed below −90 dBFS for 200 ms, or 10 s past the range end. The file ends where that quiet stretch starts, never before the range end; the 200 ms only confirms the tail is over. So a project with nothing ringing past the range bounces to exactly the range even with Include tail on.
- **Decision D-W9-4 (dither):** dither is TPDF of ±1 LSB at the target bit depth (the difference of two uniform values from a fixed-seed xorshift generator), added to the float mix after sample-rate conversion. The writer then rounds to the nearest step and clips integer formats at full scale. "Truncation" in AT-EXP-8 means this rounding without dither. In digital silence the dithered 16-bit output has an RMS of 0.5 LSB, about −96.3 dB relative to full scale; AT-EXP-8 checks −96.3 ± 2 dB (its "~−101 dBFS" was an estimate).
- **Decision D-W9-5 (dialog details):** Range is typed as bar.beat.division.tick. Artist defaults to the computer user's full name until accounts arrive (M10). For D-W9-1 the warning sits in the dialog, the Bounce button reads "Bounce Anyway", and Cancel is the "Wait" choice. Add to project imports the first file written at the range start on a new audio track below the last one, and mutes that track. The cycle never loops a bounce: offline and real-time renders play the range straight through, except for the second cycle pass (D-W9-11).
- **Decision D-W9-6 (real-time-only plug-ins):** a VST3 plug-in reports that it can't render offline through the "OnlyRT" sub-category. Weft reads it from the plug-in list, and a bounce with such a plug-in on any track switches to real time and says which plug-in caused it (in Automatic mode; see D-W9-11).
- **Decision D-W9-7 (normalize):** Normalize scales the float mix (after the mono mix-down, before sample-rate conversion and dither) so its highest sample peak is exactly 0 dBFS. On always does it; Overload Protection Only does it only when the peak is above 0 dBFS; Off (the default, so the clip warning still shows) never does. The completion notice adds "Normalized by +X.X dB", and a normalized bounce doesn't report clipping. Peaks between samples after conversion are not measured.
- **Decision D-W9-8 (formats and channels):** AIFF and FLAC are written by the platform layer at the WAV row's bit depth and rate; they hold 16 or 24 bits, so 32-bit float writes them at 24. Split writes each side as a mono file named `<name>.L.<ext>` and `<name>.R.<ext>`; Mono writes one file of (L + R) / 2. MP3 is mono for Mono and stereo otherwise. Include tempo writes the tempo at the range start, rounded to a whole BPM, as the MP3 tag's TBPM frame; WAV, AIFF and FLAC files carry no tempo.
- **Decision D-W9-9 (Export Track, Export All Tracks):** File › Export Track as Audio File exports the selected audio or instrument track; Export All Tracks as Audio Files exports every audio and instrument track that isn't muted, in track order. Both open the Bounce dialog, whose settings apply to each file; each track renders offline with only it soloed, so buses it feeds (and their effects) and the master strip are heard. One track is named after the track (editable); several are each named after their track. Replace / Keep both asks once for all of them, and the notice says "Exported N files."
- **Decision D-W9-10 (Export Selection as MIDI File):** the selected MIDI regions go into one type 1 file at 960 PPQN, one file track per project track (in track order, named after it), starting at the earliest selected region; notes play as in AT-EXP-13, and the conductor track has the key, tempo and meter from that start. The file is chosen with a save dialog starting in `Bounces`.
- **Decision D-W9-11 (mode and second cycle pass):** Automatic renders offline and switches to real time for a real-time-only plug-in (D-W9-6); Offline always renders offline and says such a plug-in is rendered anyway; Real-time plays the range. The 2nd cycle pass renders the range twice as a cycle and keeps the second time round, so delay and reverb tails from the end are heard at the start; no tail is added after it, and a real-time bounce ignores it with a note. Track exports are always offline.

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

### Decisions (M9)
- **Decision D-W10-5 (Save As):** the new folder must not exist yet and the project takes its name. Plug-in state blobs and `Bounces/` are copied; `Cache/` and `Recovery/` start empty. Without "Copy media", the new project gets `media-links.txt`, listing the absolute media folders of the original, and media is looked up in `Media/` first and then in those folders. Undo history carries on in the new folder.
- **Decision D-W10-6 (unsaved changes):** New, Open and Quit ask "Save changes to "<name>"?" with Save, Don't Save and Cancel. Don't Save archives the recovery log, so the next open doesn't offer those changes. Quitting no longer saves silently. The "Untitled" project Weft starts with is deleted if it's replaced or closed without any edit; projects made with File › New stay. Plain Save never asks for a name; Save As renames.
- **Decision D-W10-7 (view state):** this user's playhead, scroll and zoom are kept in `Cache/view.txt` (local, safe to delete) and restored on open.
- **Decision D-W10-8 (recent projects):** File › Open Recent keeps project folders in `recent-projects.txt` in the settings folder. A project joins it when opened, saved or saved as. A missing one reads "<name> (not found)", and clicking it removes it.
- **Decision D-W10-9 (open reports):** missing plug-ins are listed in one message after open. Locate… looks at every file under the chosen folder but only hashes files whose size matches a missing file's recorded size.
- **Decision D-W10-10 (Project Settings):** each changed setting is its own undo step. Metronome defaults take effect when the project is next opened; the control bar's metronome toggle stays per session.

---

## W-11 · Tempo and time signature changes

**Covers:** F-TMP-1, F-TMP-3, F-TMP-4, F-TRN-9, F-TRN-10.

1. The global tracks area shows a Tempo lane and a Signature lane (toggle via Global Tracks button or G).
2. Clicking in the Tempo lane with the pencil (or Ctrl+click) adds a tempo change at the snapped position with the value under the pointer; dragging a tempo point vertically changes its BPM (20–999, 0.01 BPM resolution); dragging horizontally moves it.
3. The initial tempo (at tick 0) cannot be deleted or moved.
4. Signature changes are added in the Signature lane at bar starts only; dialog asks numerator (1–32) and denominator (1, 2, 4, 8, 16, 32).
5. MIDI regions and automation are anchored in musical time (ticks): when the tempo changes, they move in absolute time. Audio regions are anchored by their start in ticks but their content plays at its own speed (not stretched) in MVP. **Decision D-TMP-1** (see 03).
6. Metronome and grid follow the maps.
7. Alt-dragging a tempo change copies it: the original stays, and the copy is made once the pointer moves sideways and follows the drag like a moved point (L-TMP-3). The first tempo can be copied too. One undo step "Copy Tempo Change".
8. Tap Tempo (Shift+T, Navigate menu) sets the tempo in effect at the playhead from the taps (L-TMP-9).

- **Decision D-W11-1 (tap tempo):** taps more than 2 s apart start a new run. From the second tap on, the tempo is 60,000 ms divided by the average gap of the last four gaps (five taps), rounded to 0.01 BPM and clamped to 20–999. It changes the tempo in effect at the playhead, like typing in the tempo display (AT-TRN-13), and a whole run of taps is one undo step "Tap Tempo".

---

## Decisions in this file

| ID | Decision |
|---|---|
| D-W11-1 | Tap tempo averages the last four gaps; 2 s apart starts again; a run is one undo step. |
| D-W0-1 | Device/project sample-rate mismatch: open, warn, resample playback, disable recording. |
| D-W0-2 | Default driver WASAPI shared. |
| D-W0-3 | Audio & MIDI preferences reuse Audio Setup; settings file marks first launch; Welcome every launch; No-audio mode runs on the system clock. |
| D-W0-4 | All MIDI inputs open; disabled ones dropped by name; hot-plug via device list changes. |
| D-W1-1 | Auto input monitoring on armed tracks by default. |
| D-W1-2 | R while recording stops recording, keeps playback. |
| D-W1-3 | New recordings cover older overlapping regions without trimming them. |
| D-W1-4 | One WAV per track per recording pass; takes reference offsets. |
| D-W1-5 | Punch regions get 10 ms handles with crossfades. |
| D-W1-6 | Recording onto a track deleted remotely lands on a recovered track. |
| D-W1-9 | Autopunch on sets the range to the cycle, else 4 bars from the playhead's bar; drag its ends (Shift+drag for a new range). |
| D-W1-10 | New projects click during playback and recording ("Click During: Both"), so turning the metronome on is always audible; Recording only stays a choice in Project Settings. (Changed 2026-11.) |
| D-W1-11 | Navigate › Metronome Clicks: bar, beat and division (a quarter beat, 6 dB softer) clicks on their own; Only During Count-in; five tones an octave either side; a count-in always clicks. |
| D-W1-12 | Record Toggle punches in and out while playing; Replace covers audio and MIDI, with a control bar button. |
| D-W1-13 | Capture as Recording: MIDI played since playback started becomes a recording on the selected instrument track. |
| D-W1-14 | MIDI auto record enable arms the selected instrument track. |
| D-W1-15 | Recording features left out for now. |
| D-W2-1 | Selected instrument track receives live MIDI unarmed. |
| D-W2-2 | Recorded MIDI regions round outward to whole bars. |
| D-W2-4 | MIDI Recording › Replace (off by default): a pass deletes the notes and controllers already on its track between where it started recording and where it stopped (the whole cycle for a cycle pass), deletes regions that end up empty inside that range, and makes a new region instead of merging. It all undoes with the recording. |
| D-W3-1 | Move snaps region start. |
| D-W3-2 | Overlap stacking by per-region z-order. |
| D-W3-3 | Split keeps crossing notes whole in the left region. |
| D-ARR-3 | Joining non-contiguous audio renders a new file. |
| D-ARR-4 | Joined audio keeps the first fade-in and the last fade-out. |
| D-ARR-5 | Smart snap uses the finest note grid whose lines are at least 16 px apart, else whole bars. |
| D-ARR-6 | A take moves, copies and pastes with its whole take folder. |
| D-ARR-7 | Audio dropped on an audio track lands there end to end; other drops make a track per file named after it. Files with more than 2 channels become one mono track per channel, named "<file> 1", "<file> 2"..., even when dropped on a track, and the status bar says so (changed 2026-11 from keeping the first 2 channels, which lost audio). A WAV whose data is shorter than its header says imports what's there, and the status bar says so. |
| D-ARR-8 | The Loop Region command (no drag) loops up to the next region on the track; with none after it, to the cycle end when the cycle is on and ends later; otherwise for 3 repeats. A region directly followed by another isn't looped. |
| D-W3-4 | Undo history is per session, not persisted. |
| D-W3-5 | Region Delay (inspector, audio and MIDI regions) shifts playback by −500…+500 ms without moving the region; it applies to loop repeats too, and the drawn region stays where it is. |
| D-W3-7 | Hide Track (track header menu) takes a track's row out of the tracks area; it still plays. Its mixer strip goes with the row (changed 2026-11: it used to stay in the mixer). View › Show Hidden Tracks (Ctrl+Shift+H) brings hidden rows and strips back, marked HIDDEN, so they can be unhidden. Both the flags and the switch are saved with the project and shared with collaborators. Row-based edits (dragging regions across rows, the marquee) skip hidden rows. |
| D-W3-8 | The Mute and Eraser tools, clicked on one of several selected regions, act on the whole selection as one undo step; clicked on an unselected region they act on that one. |
| D-W3-9 | Right-click on an empty lane offers Create MIDI Region (instrument tracks: one bar at the snapped click position, selected) or Import Audio... (audio tracks: the files land at the click position on that track). |
| D-W3-10 | Shift during a region drag keeps it to the direction it set off in once it has gone 4 px: along the track, or across tracks at the same position. Shift is pressed after the drag starts (Shift+click still toggles selection). |
| D-W3-11 | Edit › Nudge Value (Grid, the default and the arrange grid as before; Bar; Beat; Division, a quarter beat; Tick; local, not saved) sets what Nudge Left/Right, Lengthen/Shorten (Alt+Shift+Right/Left; the selected regions' end, MIDI at least 1 tick, audio at least the shortest region) and Slip Left/Right (Ctrl+Alt+Left/Right: what plays inside the region moves while the region stays; Slip Right moves the content later, so audio can't slip right past the start of its file) move regions by. Shuffle Left/Right move each selected region until it meets the region before it on its track (left; the project start with none) or after it (right; with none it stays). Each is one undo step ("Region Length", "Slip Region", "Shuffle"). View › Grid Lines hides the arrange grid. In the piano roll Lengthen/Shorten act on notes by the snap value (D-W4-14). |
| D-W3-12 | Edit › Select and Edit › Playhead and Cycle, in the arrange view. **Select All Following** (Shift+F) selects every region starting at or after the earliest selected one (the playhead with none). **Select All Inside Cycle** (Shift+L) selects the regions that start and end within the cycle range. **Select Muted Regions**; **Select Same-Colored Regions** (the selected regions' colours, a region without its own colour counting as its track's); **Select First / Last Region** (on the selected track, or of the project with none). **Add Previous / Next Region to Selection** (Shift+Left/Right) extend the selection along the track. Up / Down select the previous / next track (in the piano roll they still transpose). **Scroll to Selection** brings the selection's start and its first track into view. **Move Region to Playhead** moves the selected regions so the earliest starts at the playhead (one undo step); **…and Select Next** then selects the next region on the track. **Set Region Start / End to Playhead** trims each selected region the playhead is inside ("Set Region Start", "Set Region End"). **Split Regions at Cycle** splits the selected regions at both cycle locators as one undo step ("Split at Cycle"). With nothing to act on each says so. |
| D-W3-13 | Instrument tracks have track parameters in the inspector's Track section. **MIDI In** (All or one connected device; a device that went away stays listed while a track uses it) and **In channel** (All or 1-16) set the track's input; the computer keyboard reaches every track. **Out channel** (As played or 1-16) sends every note and controller on that channel. **Delay** in ms (±500) or ticks (±960; switching the unit sets it to 0) shifts what the regions play, on top of each region's delay; it doesn't delay live input. **Transpose** (±96) and **Velocity** (±99) add to the regions' own; **No Transpose** ignores the regions' transpose (for drum kits). **Key limit** and **Velocity limit** drop notes outside them, judged on the transposed pitch and the offset velocity; a limit moved past its partner takes the partner along. These apply to region playback and to live input, never change the notes, and are one undo step ("Track Parameters"; a slider drag is one). Not done: freeze, articulation sets, tuning modes, internal MIDI routing and the No Reset option. |
| D-W3-6 | Snap Edits to Zero Crossings (View menu, off by default, per user) applies to audio editor trims: the edge goes to the nearest zero crossing of the channels summed within 10 ms, in place of grid snap; Alt still bypasses it. |
| D-W4-1 | MIDI 60 is labeled C3. |
| D-W4-2 | The pencil places a note on the grid line at or before the click; dragging on lengthens it in the same undo step. |
| D-W4-3 | A transpose that would leave 0–127 for any selected note moves none; moving left stops at the earliest region start; drags commit on release; Alt bypasses snap; Ctrl+drag copies. |
| D-W4-4 | Notes moved, resized, drawn or pasted past a region's end grow it to the end of that bar. |
| D-W4-5 | Note paste goes into the topmost MIDI region under the playhead on the shown track; none there means no paste. |
| D-W4-6 | Scissors split a note into two at the snapped point. |
| D-W4-7 | Controller points snap to the grid and hold until the next point; pitch bend is 14-bit centred on 8192. |
| D-W4-8 | Velocity tool: one velocity step per pixel. |
| D-W4-9 | MIDI region Transpose (−96…+96 semitones) and Velocity (−99…+99) are playback offsets set in the inspector; the stored notes don't change, results are clamped to 0–127 pitch and 1–127 velocity, and out-of-range values are refused. |
| D-W4-10 | Clicking a note makes it the pencil's pattern: the next drawn note has its length and velocity. |
| D-W4-11 | Shift during a note drag keeps it to one direction (time or pitch), chosen by the first 4 px. |
| D-W4-12 | Resizing one of several selected notes resizes them all (one undo step); Shift: same end; Alt+Shift: same length. |
| D-W4-13 | The velocity tool stops when the first selected note reaches 1 or 127; Alt runs on (each clamped); Alt+Shift sets them all alike. |
| D-W4-14 | Note Functions: Select All Following, Legato, Remove Overlaps, Note Start/End to Playhead, Lengthen/Shorten. |
| D-W4-15 | Piano roll second row: region names, velocity slider, Scale Quantize, pointer info; velocity lines in notes. |
| D-W4-16 | MIDI region Q-Range, Q-Flam, Dynamics, Gate Time, Clip Length; region delay in ticks; Reverse tick box; Apply All Parameters (except Delay). |
| D-W5-1 | Swing 0–100% maps from straight to triplet feel. |
| D-W5-2 | Quantize moves note starts only. |
| D-W6-1 | Solo keeps upstream/downstream routing audible. |
| D-W6-2 | Mute silences pre-fader sends too. |
| D-W6-3 | Delay compensation applies to monitored inputs in MVP. |
| D-W6-4 | Mono channels run inserts in stereo, then average back to mono. |
| D-W6-5 | Parameter changes outside automation apply from the next block. |
| D-W6-6 | Bypassed inserts add no latency; master insert latency isn't compensated. |
| D-W6-7 | Mute cuts at once; unmute ramps in over one block. |
| D-W6-8 | Disabled routing menu items show the reason in their text. |
| D-W6-9 | Dragging an insert to another channel moves it (same instance); Ctrl+drag copies it; Alt+click removes it. |
| D-W6-10 | Shift+click bypasses an insert; a drag across bypass dots sets them all; Insert Before in the slot menu. |
| D-W6-11 | Search and Add Plug-in (Ctrl+Shift+P) adds a match to the selected track. |
| D-W6-12 | Plug-in settings (save, load, next/previous, compare, copy/paste, reset, default) and Show/Hide All Plug-in Windows (V). |
| D-W6-13 | Peak number; Mute Off for All; Clear/Recall Solo; Solo Tracks of Selected Regions; Input Monitoring Track; Copy/Paste Channel Strip Setting. |
| D-W6-14 | Mixer views Single/Tracks/All and the Audio/Inst/Bus filters. |
| D-W6-15 | Groups: one per track, Group slot and Groups window, linked controls with Shift to override, Enable/Disable, Create, Toggle Hide 1-9. |
| D-W6-16 | VCA faders: strips after the channels, VCA slot, scale and mute their tracks; Shift+click the inspector's VCA slot. |
| D-W7-1 | Buses appear in the tracks area. |
| D-W7-2 | New sends start at 0 dB. |
| D-W7-3 | New buses go at the end of the track order, lowest free "Bus N", one undo step. |
| D-W8-1 | Write mode reverts to Latch after a pass. |
| D-W8-2 | Automation thinning thresholds. |
| D-W8-3 | Automation interpolation and parameter queues. |
| D-W8-4 | What Touch, Latch and Write passes write; cycle jumps; glide; mute clicks. |
| D-W8-5 | Master pan is balance; master volume, pan, mute and inserts are automatable. |
| D-W8-6 | Volume, pan and send lanes follow a 32-sample timeline grid, so bounces match playback at any block size. |
| D-W8-7 | Automation mode commands for all tracks and the selected one, Alt+mode, lane ×, Cycle Through Used Parameters, Up/Down nudge points. |
| D-W9-1 | Warn when bouncing with undownloaded media. |
| D-W9-2 | Offline bounce is deterministic (fixed dither seed). |
| D-W9-3 | Where the bounce tail ends. |
| D-W9-4 | Dither: TPDF ±1 LSB, then rounding; noise level in AT-EXP-8. |
| D-W9-5 | Bounce dialog details: range format, default Artist, missing-media choice, Add to project, no cycling. |
| D-W9-6 | Real-time-only plug-ins are VST3 "OnlyRT". |
| D-W9-7 | Normalize: Off, Overload Protection Only, On, to a 0 dBFS sample peak. |
| D-W9-8 | AIFF and FLAC (16 or 24 bits), split and mono files, MP3 tempo tag. |
| D-W9-9 | Export Track and Export All Tracks: each track soloed, offline, named after it. |
| D-W9-10 | Export Selection as MIDI File: one track per project track, from the earliest region. |
| D-W9-11 | Bounce modes Automatic, Offline, Real-time; the second cycle pass. |
| D-W10-1 | Media stored by content hash. |
| D-W10-2 | Project is a folder. |
| D-W10-3 | Plain Save doesn't create a history version. |
| D-W10-4 | Refuse to open projects from newer versions. |
| D-W10-5 | Save As copies blobs and bounces; without copying media it links the original media folders. |
| D-W10-6 | Save / Don't Save / Cancel before New, Open, Quit; Don't Save archives the log; untouched startup project is deleted. |
| D-W10-7 | Per-user playhead, scroll and zoom in `Cache/view.txt`. |
| D-W10-8 | Open Recent stored in settings; missing entries removed when clicked. |
| D-W10-9 | Missing plug-ins listed after open; Locate hashes only size matches. |
| D-W10-10 | Each Project Settings change is one undo step; metronome defaults apply on next open. |
