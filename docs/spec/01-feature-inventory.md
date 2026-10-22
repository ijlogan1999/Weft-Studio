# 01 · Feature Inventory

Every user-facing feature of Weft Studio, grouped by area. Each feature has a stable ID, a tag (**MVP**, **v2**, **Later**), and a one-line behavioral summary. Detailed behavior for core features is in `02-workflows.md`; acceptance tests for every MVP feature are in `06-acceptance-tests.md`.

Tag counts at the end of the file.

---

## 1. Project management (PRJ)

| ID | Feature | Tag | Summary |
|---|---|---|---|
| F-PRJ-1 | New project | MVP | Create an empty project with chosen sample rate (44.1/48/88.2/96 kHz), default tempo 120 BPM, 4/4, and one empty audio track. |
| F-PRJ-2 | New from template | v2 | Create a project from a saved template (track layout, routing, plugins, no audio). |
| F-PRJ-3 | Save | MVP | Save the project document and reference all media by content hash. Ctrl+S. |
| F-PRJ-4 | Save As | MVP | Save under a new name/location; media is copied or linked per the "consolidate" setting. |
| F-PRJ-5 | Open | MVP | Open a project file; missing media is reported in a relink dialog without blocking open. |
| F-PRJ-6 | Recent projects | MVP | File menu lists the 10 most recent projects. |
| F-PRJ-7 | Autosave and crash recovery | MVP | Document state is persisted continuously (the CRDT change log, see 05); after a crash, reopening offers the recovered state. |
| F-PRJ-8 | Consolidate project | v2 | Copy all referenced media into the project folder and drop unused media. |
| F-PRJ-9 | Project settings | MVP | Edit sample rate (on new projects only for MVP), bit depth for recording (16/24/32-float), project name, and default metronome settings. |
| F-PRJ-10 | Project notes | v2 | Free-text notes attached to the project and to each track. |
| F-PRJ-11 | Import tracks from another project | Later | Pull tracks, with regions and channel strips, from another Weft project. |
| F-PRJ-12 | Save as template | v2 | Save current track layout and routing as a template. |
| F-PRJ-13 | Archive (single-file bundle) | v2 | Export project plus all media as one `.weftpack` zip for handoff. |

## 2. Transport and timing (TRN)

| ID | Feature | Tag | Summary |
|---|---|---|---|
| F-TRN-1 | Play / Stop | MVP | Space toggles play/stop from the playhead. Stop returns the playhead to where play started (setting: "return to start on stop", default on). |
| F-TRN-2 | Record | MVP | R arms recording on record-enabled tracks and starts transport. |
| F-TRN-3 | Pause | MVP | Pause holds the playhead in place. Brought forward from v2 (D-INV-6). |
| F-TRN-4 | Go to start / end | MVP | Home and End move the playhead to project start / project end marker. |
| F-TRN-5 | Rewind / fast-forward | MVP | Comma / period move the playhead by one bar. |
| F-TRN-6 | Cycle (loop) region | MVP | C toggles looped playback of a cycle range set in the ruler. |
| F-TRN-7 | Metronome | MVP | K toggles a click on each beat, accented on beat 1; adjustable level; audible during record only, play only, or both. |
| F-TRN-8 | Count-in | MVP | Configurable count-in before recording (off, 1 bar, 2 bars). |
| F-TRN-9 | Tempo display and edit | MVP | Transport shows tempo; click-drag or type to change the project's initial tempo. |
| F-TRN-10 | Time signature display and edit | MVP | Transport shows the meter at the playhead; editable. |
| F-TRN-11 | Position display | MVP | Shows playhead as bars:beats:division:ticks and as h:m:s.ms; click to type a position. |
| F-TRN-12 | Tap tempo | v2 | Tap a key to set tempo from the average interval of the last four taps. |
| F-TRN-13 | Pre-roll | v2 | Playback begins N bars before the record start point. |
| F-TRN-14 | Solo-in-place playback of selection | v2 | Play only the selected regions. |
| F-TRN-15 | Scrubbing | v2 | Drag the playhead to hear audio at drag speed. |
| F-TRN-16 | CPU / disk load meter | MVP | Transport shows audio thread load (% of buffer period) and disk stream health. |
| F-TRN-17 | Follow playhead | MVP | Arrange view scrolls to keep the playhead visible during playback (toggle). |
| F-TRN-18 | Varispeed | Later | Play the whole project faster or slower with or without pitch change. |
| F-TRN-19 | External sync (MIDI clock, MTC) | Later | Send or receive MIDI clock and timecode. |
| F-TRN-20 | Set cycle to selection | MVP | U sets the cycle range to the selected regions' span, rounded outward to whole bars, and turns cycle on. |
| F-TRN-21 | Swap / set locators from playhead | v2 | Set cycle or punch start/end to the playhead; swap start and end. |
| F-TRN-22 | Network tempo sync (Link protocol) | Later | Share tempo and beat phase with other apps on the local network using the open-source Link protocol. |
| F-TRN-23 | Go to position dialog | v2 | / opens a field to type a bar or time and jump there. |

## 3. Recording (REC)

| ID | Feature | Tag | Summary |
|---|---|---|---|
| F-REC-1 | Record audio to one track | MVP | Record from a selected mono or stereo input to a record-enabled audio track. |
| F-REC-2 | Multitrack audio recording | MVP | Record simultaneously to every record-enabled track, each from its own input. |
| F-REC-3 | Record MIDI | MVP | Record MIDI notes, controllers, pitch bend, and aftertouch from any connected MIDI input to a record-enabled software instrument track. |
| F-REC-4 | Input monitoring | MVP | Per-track monitor button routes live input through the channel strip; auto mode monitors while armed and stopped or recording. |
| F-REC-5 | Take folders (cycle recording) | MVP | Recording over a cycle range creates one take per pass, grouped in a take folder; the latest take is active. |
| F-REC-6 | Take folder comping | v2 | Swipe across takes to build a composite (comp) from sections of different takes. |
| F-REC-7 | MIDI merge vs. replace | MVP | Recording MIDI over an existing region either merges into it (cycle mode default) or creates a new region (linear default). |
| F-REC-8 | Punch in / out (autopunch) | MVP | Record only between punch-in and punch-out locators while transport plays through. |
| F-REC-9 | Latency compensation for recording | MVP | Recorded audio is shifted by the driver-reported input+output latency plus a user offset so it lines up with playback. |
| F-REC-10 | Low-latency monitoring mode | v2 | Temporarily bypass plugins that add latency on the monitored path. |
| F-REC-11 | Step input (MIDI) | v2 | Enter notes one at a time from a MIDI keyboard at a chosen note length without the transport running. |
| F-REC-12 | Retrospective MIDI capture | v2 | Recover the last MIDI performance played while stopped or not recording. |
| F-REC-13 | Record-safe tracks | v2 | Prevent a track from being record-enabled. |
| F-REC-14 | Input level meter on armed tracks | MVP | Armed tracks show input level before recording starts. |
| F-REC-15 | Recording file format | MVP | Recordings are written as WAV (BWF) at project sample rate and project bit depth. |
| F-REC-16 | Disk-space warning | MVP | Warn before recording if remaining disk time for armed tracks is under 10 minutes. |
| F-REC-17 | Computer keyboard as MIDI input | MVP | Ctrl+K toggles keyboard mode: letter keys play notes (A=C, W=C#, S=D … K=C one octave up), Z/X shift octave, C/V change velocity; played notes reach the selected instrument track and can be recorded. |
| F-REC-18 | Discard last recording | v2 | Ctrl+. while recording stops, throws away the take, and returns to the record start. |
| F-REC-19 | Take folder management | v2 | Flatten a take folder to its active take or comp, unpack takes to separate tracks, rename and delete takes. |

## 4. Tracks (TRK)

| ID | Feature | Tag | Summary |
|---|---|---|---|
| F-TRK-1 | Audio track | MVP | Track playing audio regions, mono or stereo. |
| F-TRK-2 | Software instrument track | MVP | Track playing MIDI regions through an instrument plugin. |
| F-TRK-3 | External MIDI track | v2 | Track sending MIDI to external hardware. |
| F-TRK-4 | Bus (aux) channel | MVP | Mixer channel receiving sends or outputs from other channels; no regions. |
| F-TRK-5 | Folder / track stack | v2 | Group tracks under a collapsible parent; summing stack routes children to a bus. |
| F-TRK-6 | Create, delete, duplicate track | MVP | Via menu, the "+" button, Ctrl+Alt+N (new), and context menu. Duplicate copies settings, not regions (option to include regions). |
| F-TRK-7 | Reorder tracks | MVP | Drag track headers to reorder. |
| F-TRK-8 | Rename track | MVP | Double-click name to edit. |
| F-TRK-9 | Track color | MVP | Pick from a 24-swatch palette; new regions inherit the track color. |
| F-TRK-10 | Mute / solo | MVP | Per-track mute and solo buttons; solo is additive; Ctrl+click solo is exclusive. |
| F-TRK-11 | Record enable | MVP | Per-track arm button. |
| F-TRK-12 | Track height | MVP | Resize individual or all tracks. |
| F-TRK-13 | Freeze track | v2 | Render a track's plugins to audio to save CPU; unfreeze restores live processing. |
| F-TRK-14 | Hide tracks | v2 | Hide tracks from the arrange view without muting. |
| F-TRK-15 | Track icons | Later | Pick an icon for a track from Weft's original icon set. |
| F-TRK-16 | Track lock | MVP | Optional per-track edit lock, mainly for collaboration (see 05 §7). |
| F-TRK-17 | Input/output selection | MVP | Choose input (audio tracks), MIDI input filter (instrument tracks), and output bus. |
| F-TRK-18 | Track alternatives (playlists) | v2 | Keep several alternate versions of a track's regions and switch between them. |
| F-TRK-19 | Track header configuration | v2 | Choose which buttons and controls appear on track headers. |

## 5. Arrange view and region editing (ARR)

| ID | Feature | Tag | Summary |
|---|---|---|---|
| F-ARR-1 | Timeline with ruler | MVP | Horizontal time axis showing bars/beats or time; zoomable and scrollable. |
| F-ARR-2 | Snap / grid | MVP | Edits snap to a grid value (bar, beat, 1/8, 1/16, 1/32, triplets, off); Alt held while dragging temporarily disables snap. "Smart" snap follows zoom level. |
| F-ARR-3 | Select regions | MVP | Click, Shift+click toggle, rubber-band drag, Ctrl+A select all, select all on track. |
| F-ARR-4 | Move regions | MVP | Drag to a new time and/or track. Audio regions only to audio tracks; MIDI regions only to instrument tracks. |
| F-ARR-5 | Copy regions | MVP | Ctrl+drag, or Ctrl+C / Ctrl+V at playhead on the selected track. |
| F-ARR-6 | Trim region start/end | MVP | Drag a region's left or right edge; non-destructive (changes the region's window into its source). |
| F-ARR-7 | Split region | MVP | Split at playhead (Ctrl+T) or with the scissors tool at click point. |
| F-ARR-8 | Join regions | MVP | Ctrl+J joins adjacent MIDI regions into one; for audio, MVP creates a new audio file by rendering the regions (see Decision D-ARR-3 in 02). |
| F-ARR-9 | Delete regions | MVP | Delete key; leaves a gap (no ripple by default). |
| F-ARR-10 | Fades | MVP | Fade-in and fade-out handles on audio regions; curve shape linear, equal-power, or S-curve. |
| F-ARR-11 | Crossfades | MVP | Overlapping audio regions on one track can be crossfaded with a crossfade tool or command. |
| F-ARR-12 | Region gain | MVP | Per-region gain in dB (−inf to +24 dB). |
| F-ARR-13 | Loop region | MVP | Loop a region's content to the right by dragging its upper-right loop handle; repeats are ghost copies of the source. |
| F-ARR-14 | Mute region | MVP | M mutes selected regions; muted regions render grey and do not play. |
| F-ARR-15 | Rename / color region | MVP | Regions have their own name and color overrides. |
| F-ARR-16 | Undo / redo | MVP | Ctrl+Z / Ctrl+Y (Ctrl+Shift+Z also redo), unlimited during a session, per-user in collaboration (see 05 §6). |
| F-ARR-17 | Undo history panel | v2 | List of undoable steps; click to jump. |
| F-ARR-18 | Markers | MVP | Named markers at positions; create at playhead, rename, move, delete, jump. |
| F-ARR-19 | Arrangement sections | v2 | Named sections (Intro, Verse...) that can be dragged to move or copy all regions inside them. |
| F-ARR-20 | Ripple edit mode | v2 | Deleting or moving regions shifts later regions. |
| F-ARR-21 | Zoom to fit | MVP | Z zooms to fit the selection, or the whole project if nothing is selected. |
| F-ARR-22 | Tool palette | MVP | Pointer, pencil (draw empty MIDI region), scissors, glue, eraser, fade, mute, zoom. Right-click is configurable as a second tool or context menu (default context menu). |
| F-ARR-23 | Nudge | MVP | Alt+Left/Right nudges selection by the current grid value. |
| F-ARR-24 | Region time-stretch to grid | Later | Stretch audio regions to fit a length with time-stretching. |
| F-ARR-25 | Alias / linked regions | v2 | Linked copies of MIDI regions where editing one edits all. |
| F-ARR-26 | Drag-and-drop audio import | MVP | Drop WAV, AIFF, FLAC, MP3 files onto the arrange view to create regions; dropping below the last track creates new tracks. |
| F-ARR-27 | Drag-and-drop MIDI file import | MVP | Drop a Standard MIDI File (type 0 or 1) to create instrument tracks and regions. |
| F-ARR-28 | Marker navigation | MVP | Alt+, / Alt+. jump to the previous / next marker; number keys 1–9 jump to markers 1–9 (when keyboard MIDI mode is off). |
| F-ARR-29 | Repeat regions | MVP | Ctrl+R places a copy of the selection immediately after it (as many times as requested in a small dialog, default 1). |
| F-ARR-30 | Selection helpers | v2 | Invert selection, select all following, select all inside cycle range, select muted, select same color. |
| F-ARR-31 | Overlap tools | v2 | Remove overlaps; trim region end to next region; crop regions outside the cycle range. |
| F-ARR-32 | Edits between locators | v2 | Insert silence, cut, or repeat the whole project section between the cycle start and end, across all tracks, markers, tempo and automation. |
| F-ARR-33 | Marquee (range) tool | v2 | Select a time range across tracks independent of region boundaries, then cut, copy, delete, or play it. |
| F-ARR-34 | Name and color helpers | v2 | Name regions by track name; color regions by track color, and the reverse. |

## 6. Audio editing (EDA)

| ID | Feature | Tag | Summary |
|---|---|---|---|
| F-EDA-1 | Audio track editor | MVP | Detailed waveform view of the selected region with sample-accurate trim and zoom. |
| F-EDA-2 | Reverse | v2 | Reverse a region (renders a new file; original kept). |
| F-EDA-3 | Normalize | v2 | Set region gain so the peak hits a target level (non-destructive via region gain). |
| F-EDA-4 | Time-stretch (elastic audio) | Later | Move transients within audio to change timing without changing pitch. |
| F-EDA-5 | Pitch correction | Later | Detect and correct pitch of monophonic audio. |
| F-EDA-6 | Tempo detection of imported audio | Later | Detect the tempo of imported audio and optionally conform it to the project. |
| F-EDA-7 | Transient detection | Later | Detect transients for slicing and quantizing audio. |
| F-EDA-8 | Audio quantize | Later | Quantize audio via transient-based slicing or elastic timing. |
| F-EDA-9 | Strip silence | v2 | Split a region into pieces at silent parts based on a threshold. |
| F-EDA-10 | Destructive sample editor | Later | Edit an audio file's samples directly (pencil, fade, silence), with its own undo. |
| F-EDA-11 | Stem separation | Later | Split a mixed audio file into vocals, drums, bass, and other stems using an open-source separation model. |
| F-EDA-12 | Transient-based trimming | Later | Trim a region's start or end to the previous or next detected transient. |
| F-EDA-13 | Drum replacement | Later | Create MIDI hits from an audio drum track's transients to trigger a sampler. |

## 7. MIDI editing and piano roll (PRL)

| ID | Feature | Tag | Summary |
|---|---|---|---|
| F-PRL-1 | Piano roll editor | MVP | Grid of pitch (vertical, with a keyboard) by time (horizontal) showing the selected MIDI region's notes. |
| F-PRL-2 | Draw notes | MVP | Pencil tool or Ctrl+click on empty grid creates a note of the last-used length, snapped to grid. |
| F-PRL-3 | Select notes | MVP | Click, Shift+click, rubber-band, click a piano key to select all notes of that pitch. |
| F-PRL-4 | Move notes | MVP | Drag horizontally (time) and vertically (pitch); up/down arrow transposes by semitone, Shift+arrow by octave. |
| F-PRL-5 | Resize notes | MVP | Drag a note's right edge (and left edge) to change length. |
| F-PRL-6 | Delete notes | MVP | Delete key or eraser tool. |
| F-PRL-7 | Copy/paste notes | MVP | Ctrl+drag or Ctrl+C/V at playhead. |
| F-PRL-8 | Velocity editing | MVP | Velocity shown by note color/brightness; a velocity lane under the grid shows bars that can be dragged; a velocity tool drags selected notes' velocity relatively. |
| F-PRL-9 | Quantize notes | MVP | Quantize selected notes (or the whole region) to a grid value with strength (0–100%) and swing (0–100%). See W-5. |
| F-PRL-10 | Quantize note ends | v2 | Option to quantize note lengths as well as starts. |
| F-PRL-11 | Audition notes | MVP | Clicking or moving a note plays it through the track's instrument (toggle). |
| F-PRL-12 | Controller lanes | MVP | Lanes under the grid for velocity, mod wheel (CC1), sustain (CC64), pitch bend, and any CC; draw and edit points. |
| F-PRL-13 | Scale/key highlighting | v2 | Shade the grid rows that belong to a chosen key/scale. |
| F-PRL-14 | Scale quantize | v2 | Move notes to the nearest pitch in a chosen scale. |
| F-PRL-15 | Note mute | MVP | Mute individual notes (they render hollow and do not play). |
| F-PRL-16 | Legato / fix overlaps | v2 | Extend each note to the next note's start, or trim overlaps. |
| F-PRL-17 | Humanize | v2 | Randomize timing (±ticks) and velocity (±value) of selected notes. |
| F-PRL-18 | Transform presets | Later | Batch operations on selected events (crescendo, fixed velocity, reverse, invert). |
| F-PRL-19 | Event list editor | v2 | Tabular list of all MIDI events with editable values. |
| F-PRL-20 | Score / notation editor | Later | Standard music notation view and editing. |
| F-PRL-21 | Step sequencer | Later | Pattern-based grid sequencer for drums and synths. |
| F-PRL-22 | Drum editor mode | v2 | Piano roll mode with named drum lanes and fixed-length hits. |
| F-PRL-23 | Ghost notes from other regions | v2 | Show notes of other regions/tracks faintly for reference. |
| F-PRL-24 | Chord track / chord detection | Later | Track of chord symbols that MIDI can follow. |
| F-PRL-25 | Arpeggiator | Later | MIDI effect that arpeggiates held chords. |
| F-PRL-26 | Articulations | Later | Per-note articulation IDs that switch instrument playing styles. |
| F-PRL-27 | MIDI transform window | Later | Rule-based batch editing of events (select by condition, apply an operation). |
| F-PRL-28 | Brush tool | Later | Paint repeated notes along the grid with a pattern. |

## 8. Quantize (general, QNT)

| ID | Feature | Tag | Summary |
|---|---|---|---|
| F-QNT-1 | Region quantize parameter | MVP | Each MIDI region has a non-destructive playback quantize setting (off or grid value + strength + swing) applied at playback time; can be "applied" to bake it into note positions. |
| F-QNT-2 | Destructive quantize command | MVP | Quantize command (Q) in piano roll moves notes; undoable. |
| F-QNT-3 | Groove templates | Later | Quantize to the timing feel extracted from another region. |
| F-QNT-4 | Input quantize | v2 | Quantize MIDI as it is recorded. |

## 9. Mixer (MIX)

| ID | Feature | Tag | Summary |
|---|---|---|---|
| F-MIX-1 | Mixer window | MVP | Channel strips for all tracks, buses, and the master, side by side. |
| F-MIX-2 | Volume fader | MVP | −inf to +6 dB, 0 dB default; Alt+click resets; fine adjust with Shift-drag. |
| F-MIX-3 | Pan | MVP | Pan knob −100 (L) to +100 (R); constant-power −3 dB pan law (MVP fixed; configurable in v2). Stereo channels use balance. |
| F-MIX-4 | Mute / solo on strips | MVP | Same state as track header mute/solo. |
| F-MIX-5 | Peak meters | MVP | Post-fader peak meter with peak-hold and clip indicator (click to reset). |
| F-MIX-6 | Insert slots | MVP | Up to 15 insert slots per channel (MVP), each holding one effect plugin; bypass, reorder by drag, remove. |
| F-MIX-7 | Instrument slot | MVP | Instrument tracks have one instrument slot above the inserts. |
| F-MIX-8 | Sends | MVP | Up to 8 send slots per channel to buses; level −inf to +6 dB; pre-fader or post-fader (default post). |
| F-MIX-9 | Buses | MVP | Create a bus by choosing an unused bus from a send or output menu; a bus channel strip appears automatically. |
| F-MIX-10 | Output routing | MVP | Each channel outputs to the master or to any bus. Cycles are prevented (see W-7). |
| F-MIX-11 | Master channel | MVP | Final stereo sum with fader, inserts, and meter; feeds the hardware output and the bounce. |
| F-MIX-12 | Channel strip input selector | MVP | Audio track strips choose the hardware input. |
| F-MIX-13 | Plugin delay compensation | MVP | All signal paths are aligned for plugin-reported latency. |
| F-MIX-14 | Groups (VCA / linked faders) | v2 | Link fader, mute, solo, and other parameters across channels. |
| F-MIX-15 | Channel strip presets | v2 | Save and load the full chain of a channel strip. |
| F-MIX-16 | Loudness / RMS meters | v2 | LUFS and RMS metering on the master. |
| F-MIX-17 | Phase invert, gain trim | v2 | Per-channel input gain trim and polarity invert. |
| F-MIX-18 | Surround / immersive mixing | Later | Multi-channel and object-based mixing. |
| F-MIX-19 | Sidechain inputs | v2 | Route a channel to a plugin's sidechain input. |
| F-MIX-20 | Mixer filter views | v2 | Show only tracks, buses, or selected channels. |
| F-MIX-21 | Copy / paste channel strip | v2 | Copy a whole channel strip (inserts, sends, settings) and paste it onto other channels. |
| F-MIX-22 | MIDI learn | v2 | Click a control, move a hardware knob or fader, and the two are linked. |
| F-MIX-23 | Mixer selection helpers | v2 | Select all audio, instrument, bus, or muted channel strips. |

## 10. Plugins and built-in processors (PLG)

| ID | Feature | Tag | Summary |
|---|---|---|---|
| F-PLG-1 | VST3 hosting | MVP | Load VST3 effects and instruments (64-bit). |
| F-PLG-2 | Plugin scanning | MVP | Scan standard VST3 folders and user folders; out-of-process scan so a crashing plugin is blacklisted, not fatal. |
| F-PLG-3 | Plugin manager | MVP | List of plugins with enable/disable, rescan, and failed-plugin report. |
| F-PLG-4 | Plugin editor windows | MVP | Open a plugin's own GUI in a floating window; generic parameter UI if it has none. |
| F-PLG-5 | Plugin state save/restore | MVP | Plugin state (opaque chunk) is stored in the project and restored on load. |
| F-PLG-6 | Plugin presets | v2 | Save and load presets per plugin (VST3 .vstpreset). |
| F-PLG-7 | Plugin sandboxing | Later | Run plugins out of process so a crash doesn't take down the host. |
| F-PLG-8 | CLAP hosting | Later | Host CLAP plugins. |
| F-PLG-9 | Built-in instrument: basic sampler/synth | MVP | One original built-in instrument ("Weft Keys": a simple polyphonic subtractive synth with a few original presets) so instrument tracks work with no third-party plugins. |
| F-PLG-10 | Built-in effects (MVP set) | MVP | Original built-in channel EQ (4-band + HPF/LPF), compressor, delay, reverb (algorithmic), gain/utility. |
| F-PLG-11 | Built-in drum sampler | v2 | Sample-based drum instrument with original samples. |
| F-PLG-12 | Built-in effects (extended) | v2 | Chorus, flanger, distortion, limiter, gate, de-esser, tuner. |
| F-PLG-13 | Pattern generator (auto drummer) | Later | Generates drum patterns that follow song sections, with original sounds. |
| F-PLG-14 | Parameter automation from plugins | MVP | Any plugin parameter can be automated (see AUT). |
| F-PLG-15 | Plugin missing placeholder | MVP | If a plugin in a project is not installed, a placeholder keeps its state and slot so the project round-trips intact; the slot passes audio through. |
| F-PLG-16 | Copy / paste plugin settings | v2 | Copy one plugin's settings and paste them onto another instance of the same plugin. |
| F-PLG-17 | Macro controls panel | Later | A simplified panel of a few big controls mapped to the most useful parameters of a track's plugins. |

## 11. Automation (AUT)

| ID | Feature | Tag | Summary |
|---|---|---|---|
| F-AUT-1 | Show automation | MVP | A toggles automation display; each track can show one or more automation lanes beneath it. |
| F-AUT-2 | Automatable parameters | MVP | Volume, pan, mute, send levels, and every plugin parameter. |
| F-AUT-3 | Draw/edit automation points | MVP | Click a lane to add a point; drag points; delete; rubber-band select; move multiple. |
| F-AUT-4 | Curve shapes | MVP | Segments between points are linear by default; each segment can be curved (one bend parameter). |
| F-AUT-5 | Automation modes | MVP | Per-track mode: Read (default), Off, Touch, Latch, Write. See W-8. |
| F-AUT-6 | Live write from UI controls | MVP | Moving a fader/knob in Touch/Latch/Write records automation in real time. |
| F-AUT-7 | Live write from MIDI controller | v2 | Map a hardware control to a parameter and write automation with it. |
| F-AUT-8 | Region-based automation | Later | Automation stored in regions that moves with them. |
| F-AUT-9 | Move automation with regions | v2 | Option to move track automation under regions when moving regions. |
| F-AUT-10 | Thin automation | v2 | Reduce point density of written automation. |
| F-AUT-11 | Automation on buses and master | MVP | Bus and master channels have automation lanes too. |
| F-AUT-12 | Automation helpers | v2 | Add automation points at region borders; delete redundant points; delete all automation on a track. |

## 12. Tempo, meter, and global tracks (TMP)

| ID | Feature | Tag | Summary |
|---|---|---|---|
| F-TMP-1 | Tempo map | MVP | Tempo changes at positions; instantaneous (step) changes in MVP. |
| F-TMP-2 | Tempo ramps | v2 | Linear tempo ramps between tempo points. |
| F-TMP-3 | Time signature map | MVP | Meter changes at bar starts. |
| F-TMP-4 | Global tracks area | MVP | Lanes above the tracks for markers, tempo, and signature. |
| F-TMP-5 | Key signature track | v2 | Key changes used by scale features. |
| F-TMP-6 | Audio follows tempo | v2 | Audio regions marked "follow tempo" are time-stretched when the tempo changes. MVP: audio is anchored in absolute time (see D-TMP-1 in 03). |
| F-TMP-7 | Video track | Later | Show a video file in sync with the timeline. |
| F-TMP-8 | Tempo list editor | v2 | Table of tempo changes with editable position and BPM. |
| F-TMP-9 | Beat mapping | Later | Align the bar grid to a free-tempo recording by marking beats. |

## 13. Bounce and export (EXP)

| ID | Feature | Tag | Summary |
|---|---|---|---|
| F-EXP-1 | Bounce mix to WAV | MVP | Render master output over a range to WAV at 16/24/32-float bit, project or chosen sample rate. |
| F-EXP-2 | Bounce mix to MP3 | MVP | Render to MP3 (CBR 128/192/256/320 kbps, VBR optional) with ID3 title/artist tags. |
| F-EXP-3 | Offline (faster than real time) bounce | MVP | Render as fast as the CPU allows; real-time mode available for hardware/plugins that require it. |
| F-EXP-4 | Bounce range | MVP | Default range is the cycle range if on, else project start to end marker; editable. |
| F-EXP-5 | Dither | MVP | TPDF dither on when bouncing to 16-bit; off otherwise; user override. |
| F-EXP-6 | Normalize on bounce | v2 | Normalize to peak or LUFS target. |
| F-EXP-7 | Export stems / tracks | v2 | Export each track or bus as its own audio file. |
| F-EXP-8 | Export FLAC / AIFF / OGG | v2 | Additional formats. |
| F-EXP-9 | Export MIDI file | v2 | Export selected MIDI regions or all as a Standard MIDI File. |
| F-EXP-10 | Bounce in place | v2 | Render a track or regions to a new audio track. |
| F-EXP-11 | Tail handling | MVP | Bounce extends past the range end until output falls below −90 dBFS for 200 ms, up to 10 s (option "include tail", default on). |
| F-EXP-12 | Export region as audio file | v2 | Export the selected regions as audio files (with gain and fades applied). |
| F-EXP-13 | Notation and interchange export | Later | Export MusicXML and AAF for use in other applications. |

## 14. Library and browsers (LIB)

| ID | Feature | Tag | Summary |
|---|---|---|---|
| F-LIB-1 | File browser | MVP | Panel listing folders/files on disk with audio preview; drag files into the arrange view. |
| F-LIB-2 | Project media browser | MVP | List of all audio files used by the project, with usage count. |
| F-LIB-3 | Plugin/preset library | v2 | Browse instruments, effects, and channel strip presets by category. |
| F-LIB-4 | Loop browser with tagging | Later | Search a loop library by tags (genre, instrument, key, tempo) with tempo-synced preview. Weft ships no third-party loops. |
| F-LIB-5 | Audio preview follows tempo | Later | Previews of loops are stretched to project tempo. |
| F-LIB-6 | Clip launcher grid | Later | A grid of loopable clips and scenes that can be triggered live and recorded into the arrangement. |

## 15. UI, workspace, and preferences (UI)

| ID | Feature | Tag | Summary |
|---|---|---|---|
| F-UI-1 | Single main window | MVP | All core areas in one window with toggleable panels (see 04). |
| F-UI-2 | Detachable editor/mixer windows | v2 | Pop panels out to separate windows (multi-monitor). |
| F-UI-3 | Light and dark themes | MVP | Weft's own dark (default) and light themes. |
| F-UI-4 | HiDPI scaling | MVP | Correct rendering at 100–250% Windows scaling. |
| F-UI-5 | Keyboard shortcut editor | v2 | Rebind any command. MVP ships fixed defaults stored in a JSON file users may edit. |
| F-UI-6 | Screensets / saved layouts | Later | Save and recall window arrangements. |
| F-UI-7 | Audio device preferences | MVP | Driver type (WASAPI shared, WASAPI exclusive, ASIO), device, sample rate, buffer size, input/output channel names. |
| F-UI-8 | MIDI device preferences | MVP | Enable/disable MIDI inputs/outputs. |
| F-UI-9 | Command palette | v2 | Ctrl+Shift+P search over all commands. |
| F-UI-10 | Quick help / tooltips | MVP | Hover tooltips naming each control and its shortcut. |
| F-UI-11 | Accessibility: screen reader support | v2 | UI Automation (UIA) names and roles for all controls; keyboard navigation of all panels. |
| F-UI-12 | Localization | Later | Translatable UI strings. MVP is English, but strings are externalized from day one. |
| F-UI-13 | Control surface support | Later | Mackie Control / HUI hardware controllers. |
| F-UI-14 | Onboarding / first-run audio setup | MVP | First launch walks through choosing an audio device and tests output with a tone. |
| F-UI-15 | Zoom presets and per-track zoom | v2 | Save and recall three zoom levels; zoom a single track taller without changing others; vertical waveform zoom. |
| F-UI-16 | Undo history panel shortcut | v2 | Ctrl+Alt+Z opens the undo history (F-ARR-17). |
| F-UI-17 | Safe start | v2 | Holding Shift while launching starts Weft without loading third-party plugins, and holding Ctrl starts without opening a project. |

## 16. Collaboration (COL)

Detailed in `05-collaboration-spec.md`.

| ID | Feature | Tag | Summary |
|---|---|---|---|
| F-COL-1 | CRDT project document | MVP | All project state lives in a CRDT document; every edit is a CRDT change. |
| F-COL-2 | Sign in / identity | MVP | Users sign in to a relay with an account (email + password or OAuth); display name and color. |
| F-COL-3 | Share project / invite | MVP | Owner invites others by email or link; roles Owner, Editor, Viewer. |
| F-COL-4 | Real-time sync | MVP | Edits appear on other connected clients within 500 ms on a typical connection. |
| F-COL-5 | Offline editing and merge on reconnect | MVP | Edits made offline are kept and merged when reconnected. |
| F-COL-6 | Presence: who's online | MVP | Avatars of connected collaborators in the toolbar. |
| F-COL-7 | Presence: playheads and cursors | MVP | Each collaborator's playhead and mouse pointer (in arrange view and piano roll) shown in their color. |
| F-COL-8 | Presence: selections and active track | MVP | Collaborators' selected regions/notes and the track they're editing are outlined in their color. |
| F-COL-9 | Content-addressed media sync | MVP | Audio files are hashed (BLAKE3), uploaded once, and downloaded by peers on demand. |
| F-COL-10 | Media transfer status | MVP | Regions whose media is not yet local show a progress state; playback skips them silently until available. |
| F-COL-11 | Optional track locking | MVP | Lock a track to prevent others editing it; default off. |
| F-COL-12 | Timeline comments | MVP | Comments pinned to a time position (and optionally a track), with replies and resolve. |
| F-COL-13 | Shared markers | MVP | Markers sync like all other document content. |
| F-COL-14 | Built-in chat | MVP | Project chat panel with message history. |
| F-COL-15 | Version history | MVP | Named and automatic snapshots; browse and restore (restore creates a new version, never deletes history). |
| F-COL-16 | Per-user undo | MVP | Undo reverts only your own changes. |
| F-COL-17 | Follow mode | v2 | Follow a collaborator's view (scroll, zoom, open editor). |
| F-COL-18 | Change attribution | v2 | Hover a region to see who last changed it and when. |
| F-COL-19 | Branches / alternate mixes | Later | Fork the project document into a branch and merge back. |
| F-COL-20 | Missing plugin notice for collaborators | MVP | If a collaborator lacks a plugin used in the project, they see the placeholder and a list of required plugins. |
| F-COL-21 | Self-hosted relay | v2 | Run the relay server on one's own machine or server. |
| F-COL-22 | Real-time networked jamming | Later | Research item: low-latency audio between collaborators. See 05 §12 for latency analysis. |
| F-COL-23 | Voice chat | Later | Built-in voice channel for collaborators. |
| F-COL-24 | Share playback position ("play from here for everyone") | v2 | Start synchronized playback on all clients from one position (each user monitors locally). |

## 17. Licensing, updates, telemetry (SYS)

| ID | Feature | Tag | Summary |
|---|---|---|---|
| F-SYS-1 | About box with third-party licenses | MVP | Lists the product version and all open-source licenses. |
| F-SYS-2 | Crash reporter | v2 | Opt-in crash report upload. |
| F-SYS-3 | Auto-update | v2 | Check for updates and install. |
| F-SYS-4 | Opt-in telemetry | Later | Anonymous usage statistics, off by default. |

---

## Tag counts

278 features in total: 149 MVP, 86 v2, 43 Later (36 added on 2026-10-14 after the reference coverage check in `07-reference-coverage.md`). The MVP list is long because many entries are small (a button, a shortcut); the Phase 2 milestone plan groups them into the eight MVP build milestones Ian listed plus a collaboration track.

## Decisions in this file

- **D-INV-1.** MVP includes one original built-in instrument and a small effect set (F-PLG-9, F-PLG-10). Without them, a fresh install could not make sound on an instrument track or test inserts without third-party plugins, which would make the acceptance tests depend on external software.
- **D-INV-2.** Take folders are MVP for capture (F-REC-5), but comping (F-REC-6) is v2. Recording in cycle mode must do something sensible from day one; comping is a large UI feature.
- **D-INV-3.** Tempo changes are step-only in MVP (F-TMP-1); ramps are v2. Ramps complicate tick-to-sample conversion and CRDT merge semantics of the tempo map.
- **D-INV-4.** Plugin sandboxing (F-PLG-7) is Later, but scanning is out of process from MVP (F-PLG-2) since scan-time crashes are the most common failure.
- **D-INV-5.** Track locking (F-TRK-16, F-COL-11) is MVP but off by default, per the requirement that merging is the default.
- **D-INV-6.** Pause (F-TRN-3) moved from v2 to MVP after the 2026-10-21 functionality audit: Ctrl+Space stops without returning to the play start, and pressed while stopped plays on from the playhead.
