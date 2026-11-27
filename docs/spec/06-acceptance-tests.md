# 06 · Acceptance Tests

Testable behaviors for every MVP feature in `01-feature-inventory.md`. Each test cites the feature IDs it covers; the coverage table at the end proves every MVP feature has at least one test.

## Test levels

| Tag | Level | Runs where |
|---|---|---|
| **[U]** | Unit test (pure model/DSP logic) | CI on any OS |
| **[E]** | Engine integration test: headless audio engine with a *null/offline audio device*, rendering to buffers and comparing samples | CI on any OS |
| **[S]** | Sync integration test: two or more headless clients and a real relay process on localhost | CI on any OS |
| **[UI]** | UI automation (scripted input on the real window, e.g. via an automation hook or Windows UI Automation) | CI on Windows runner |
| **[HW]** | Requires a real audio/MIDI device or Windows driver (WASAPI, ASIO, VST3 GUI) | Windows machine; manual or hardware-in-the-loop |

## Shared fixtures

- **Null device:** 48 kHz, 2 in / 2 out, 256-sample buffer, round-trip latency reported as 512 samples (256 in + 256 out). Input can be fed from a WAV file or generator; output is captured.
- **Fixture plugins:** `TestGain` (VST3 effect, gain parameter, 0 latency), `TestLatency` (VST3, reports 1,000 samples latency and delays by exactly that), `TestSynth` (VST3 instrument that outputs a sine at the note's frequency with amplitude = velocity/127), `TestCrashScan` (crashes during scan). All built from scratch in the test tree.
- **Signals:** `impulse` (single sample of 1.0), `sine1k` (1 kHz, −6 dBFS), `click440` (440 Hz, 10 ms).
- **Tolerances:** sample-position checks are exact unless stated; level checks ±0.01 dB for gain math, ±0.1 dB for plugin/pan/fade tests.
- **Project P0:** new project, 48 kHz, 120 BPM, 4/4. At 120 BPM, 1 beat = 24,000 samples, 1 bar = 96,000 samples, 1 tick = 25 samples.

Notation: *Given* / *When* / *Then*.

---

## 1. Project (PRJ)

**AT-PRJ-1** [U][UI] · F-PRJ-1 · *Given* the app is open, *when* the user creates a new project at 48 kHz, *then* the document has sampleRate 48000, one tempo point (tick 0, 120 BPM), one signature point (bar 0, 4/4), one audio track named "Audio 1", one master track, and nothing else.

**AT-PRJ-2** [U] · F-PRJ-1 · New project at each of 44.1, 48, 88.2, 96 kHz stores that rate; any other rate is rejected.

**AT-PRJ-3** [E] · F-PRJ-3, F-PRJ-5 · *Given* a project with 2 audio tracks, 1 instrument track, 3 regions, 10 notes, 1 bus, 1 send, 1 built-in reverb insert with non-default parameters, 1 automation lane with 3 points, and 2 markers, *when* saved, closed, and reopened, *then* the reopened document is field-for-field equal (excluding LOCAL ONLY fields), and an offline bounce of 4 bars is bit-identical before and after.

**AT-PRJ-4** [U] · F-PRJ-3 · Save is atomic: *given* a fault injected after writing 50% of the temp file, *then* the previous `project.automerge` is intact and opens.

**AT-PRJ-5** [UI] · F-PRJ-3 · After an edit, the title shows `*`; after Ctrl+S it does not.

**AT-PRJ-6** [E] · F-PRJ-4 · Save As with "Copy media" creates a new folder whose `Media/` contains every referenced hash; the original folder is unchanged; the new project opens with no missing media.

**AT-PRJ-7** [E] · F-PRJ-4 · Save As without copying media references the original files; opening it shows no missing media while the original exists.

**AT-PRJ-8** [E] · F-PRJ-5 · *Given* a project whose `Media/` is missing one referenced file, *when* opened, *then* open succeeds, the affected region is flagged missing, and Locate… on a folder containing a file with a matching hash restores it (regardless of its filename).

**AT-PRJ-9** [U] · F-PRJ-5 · Opening a document with `schemaVersion` greater than supported fails with a message naming the required version and does not modify the file.

**AT-PRJ-10** [UI] · F-PRJ-6 · After opening 11 different projects, File › Recent lists the 10 most recent, newest first; a deleted project shows as unavailable and is removed when clicked.

**AT-PRJ-11** [E] · F-PRJ-7 · *Given* unsaved edits, *when* the process is killed 2 s after the last edit, *then* reopening offers recovery, and Recover yields a document equal to the pre-kill state.

**AT-PRJ-12** [E] · F-PRJ-7 · Choosing Discard on recovery leaves the last saved state and moves the log to an archive file.

**AT-PRJ-13** [U][UI] · F-PRJ-9 · Project Settings changes name and record bit depth; the next recording uses the new bit depth. Sample rate is editable while the project has no media and disabled afterwards.

**AT-PRJ-15** [U][UI] · F-TMP-10 · The project key starts at C major; typing "f#m" in the control bar's key display sets F♯ minor as one undo step "Set Key" and nonsense changes nothing; Project Settings offers the 24 keys and sets B♭ major; names and sharps/flats follow D-DM-20; an exported MIDI file carries the key signature event, and importing a MIDI file into an empty project takes the file's key (as it takes the tempo, D-AT-1).

**AT-PRJ-16** [U] · F-PRJ-9, D-UI-42 · Project count-in takes none, 1 to 6 bars or 1 or 2 beats and refuses 7 bars or 3 beats; the pan law takes 0, -3, -4.5 or -6 dB as one undo step "Set Pan Law"; the MIDI input filter takes the six kinds' bits as one undo step "Set MIDI Input Filter"; note-offs and velocity-0 note-ons always pass; all three survive saving and reopening.

**AT-PRJ-17** [E] · D-UI-42 · Recording at bar 5 with a 3-bar count-in starts at bar 2, with 6 bars at bar -1, with 2 beats of 4/4 one half note before, and with 1 beat of 6/8 one eighth before; no count-in starts recording at once. The project's count-in default reaches the transport unclamped.

**AT-PRJ-18** [E] · D-UI-42 · A mono source at centre plays at -6.02 dB each side with the -6 dB law (and 0.75 at pan +0.5, linear), -4.52 dB with -4.5, 0 dB with 0 (and +3 dB hard left), and -3.01 dB with -3; a hard pan is unity under -3, -4.5 and -6; a stereo source's balance is the same under any law.

**AT-PRJ-19** [E] · D-UI-42 · With pitch bend and program change ignored, a recorded pass keeps its note and volume controller but neither ignored event, and the instrument never hears them. With notes ignored, a new note-on is dropped while the note-off of a note held before still reaches the instrument.

**AT-PRJ-20** [UI] · D-UI-42 · Project Settings lists nine count-in choices ending "2 beats", shows the pan law as "-3 dB (equal power)", and sets 2 beats, -6 dB and ignore pitch bend; reopening it shows them. Navigate › Count-in lists "6 Bars" and "1 Beat"; choosing 1 Beat, Shift+K turns it off and back to 1 beat ("Count-in: 1 beat."); the control bar's More menu sets 4 bars.

**AT-PRJ-21** [U] · D-UI-47 · Setting the project's notes and a track's notes are undo steps "Project Notes" and "Track Notes"; setting the same text again, or notes on a missing track, is refused. Undo and redo restore them; they survive saving and reopening; clearing the project's notes removes them from the file.

**AT-PRJ-14** [UI] · L-PRJ-8 · File › Delete Project lists the recent projects except the open one; choosing one asks first, then moves its folder to the system Trash and removes it from Open Recent; Cancel leaves it; the open project can't be deleted.

## 2. Transport (TRN)

**AT-TRN-1** [E] · F-TRN-1 · *Given* P0 with a region at bar 2, playhead at bar 1, *when* Play, *then* the first non-silent output sample is at sample 96,000 (bar 2) relative to play start, ± output latency accounted for in the capture.

**AT-TRN-2** [E] · F-TRN-1 · Stop with "return to start on stop" on returns the playhead to the play start position; with it off, the playhead stays where it stopped.

**AT-TRN-3** [UI] · F-TRN-1 · Space toggles play/stop when focus is in the tracks area, and types a space when focus is in a text field.

**AT-TRN-4** [E] · F-TRN-2 · R with no armed tracks starts playback and shows "No tracks armed" in the status bar without creating regions.

**AT-TRN-5** [U] · F-TRN-4 · Home sets the playhead to tick 0; End sets it to the end marker, or the end of the last region when none.

**AT-TRN-6** [U] · F-TRN-5 · Period from bar 3 beat 2 moves to bar 4 beat 1; comma from bar 3 beat 2 moves to bar 3 beat 1; comma at bar 3 beat 1 moves to bar 2 beat 1; comma at bar 1 stays at bar 1.

**AT-TRN-7** [E] · F-TRN-6 · *Given* cycle bar 2–3 (96,000–192,000 samples) and playhead at bar 1, *when* playing 3.5 cycle lengths' worth of samples, *then* output sample positions wrap from 191,999 to 96,000 with no gap or duplicated samples (compare against a rendered reference of the region content).

**AT-TRN-8** [E] · F-TRN-6 · A cycle shorter than one audio buffer (e.g. 100 samples at 256 buffer) wraps correctly within the buffer.

**AT-TRN-9** [E] · F-TRN-7 · Metronome on, 4/4, 120 BPM: clicks start at samples 0, 24,000, 48,000, 72,000, 96,000…; the click at each bar start has a higher pitch or level than other beats; in 6/8 accents fall every 6 eighth notes.

**AT-TRN-10** [E] · F-TRN-7 · Metronome "record only": silent during play, audible during record; "play only" the reverse; level −12 dB produces click peaks 12 dB below the 0 dB setting.

**AT-TRN-11** [E] · F-TRN-8 · Count-in 1 bar, record at bar 3: clicks sound for one bar before bar 3, and the recorded region starts exactly at bar 3 with no count-in clicks in the file.

**AT-TRN-12** [E] · F-TRN-8 · Count-in 2 bars from bar 1 starts the click 2 bars before tick 0 (pre-roll into negative time) and recording starts at tick 0.

**AT-TRN-13** [U][UI] · F-TRN-9 · Typing 90 in the tempo field sets the initial tempo point to 90.00; values < 20 or > 999 are clamped; with a later tempo change at bar 9 and playhead at bar 10, editing the display changes the bar-9 point.

**AT-TRN-14** [U][UI] · F-TRN-10 · Editing the signature display at bar 1 to 3/4 changes the initial signature point; bar 2 now starts at tick 2,880.

**AT-TRN-15** [U] · F-TRN-11 · At 120 BPM 4/4, tick 4,080 displays as `2.1.2.0` (bars.beats.division.ticks, division = 1/16; bar 2 starts at tick 3,840) and time `0:00:02.125`.

**AT-TRN-16** [UI] · F-TRN-11 · Typing `5 3` into the position display moves the playhead to bar 5 beat 3; typing `0:30` into the time display moves it to 30 s.

**AT-TRN-17** [E] · F-TRN-16 · With a fixture plugin that burns 80% of the buffer period, the CPU meter reads 75–90%; forcing a missed deadline increments the dropout counter.

**AT-TRN-19** [U] · F-TRN-20 · With regions selected from bar 3 beat 2 to bar 6 beat 1, U sets the cycle range to bar 3 – bar 7 (rounded outward to bars) and turns cycle on; with nothing selected, U does nothing.

**AT-TRN-20** [UI] · D-UI-17 · Stopped at bar 4 with Cycle off, Stop moves the playhead to tick 0. With the cycle at bars 3–5 and the playhead at bar 7, Stop goes to bar 3 and Stop again to tick 0; Space starts playback at bar 3; numpad 0 stops it and the playhead is back at bar 3. With Cycle off, Play starts at the playhead. The Navigate menu lists Stop.

**AT-TRN-21** [E][UI] · D-W1-11 · At 120 BPM in 4/4 with Division on, clicks fall every 6,000 samples and a division is softer than a beat; with Bar off the click at sample 0 has the pitch of a beat; with only Bar on, clicks fall at 0 and 96,000 only; with Only During Count-in on, a one-bar count-in clicks four times and the bar after it is silent; Tone Low clicks lower than Normal and High higher. The Navigate menu's Metronome Clicks submenu turns each option on and the engine's settings follow.

**AT-TRN-22** [UI] · D-UI-35 · With the cycle at bars 3–4 (off) and a region at bars 5–6: Ctrl+Home goes to bar 3, Ctrl+End to bar 4; with the region selected Shift+Home goes to bar 5 and Shift+End to bar 7; with nothing selected Go to Selection Start says so. Shift+Space plays from bar 5; Stop and Go to Cycle Start stops at bar 3; Play from Previous Bar plays from bar 2; Stop and Go to Beginning stops at tick 0; Play from Left Window Edge plays from the view's left edge and Stop and Go to Last Locate Position returns there. Ctrl+Shift+Right moves the cycle to bars 4–5, Double makes it 4–6, Halve 4–5, Ctrl+Shift+Left back to 3–4, and undo undoes one move. The Navigate menu has Go To, Play From, Stop And and Cycle Range; Play's right-click menu lists Play from Cycle Start and picking it plays from bar 3.

**AT-TRN-18** [UI] · F-TRN-17 · With follow on and zoom showing 4 bars, playing past the right edge scrolls the view so the playhead stays visible; with follow off, the view does not scroll.

## 3. Recording (REC)

**AT-REC-1** [E] · F-REC-1, F-REC-15 · *Given* an armed mono audio track fed `sine1k` on input 1, record 2 bars from bar 1, *then* a 24-bit 48 kHz mono BWF file exists in `Media/` named by hash, and the region at tick 0 has length 192,000 ± 1 buffer samples.

**AT-REC-2** [E] · F-REC-1, F-REC-9 · Latency compensation: feed the output's metronome click back into the input with the null device's 512-sample round trip; the recorded click aligns with the metronome click position within ±1 sample. With a user offset of +100 samples it shifts by exactly 100.

**AT-REC-3** [E] · F-REC-1 · Recording shorter than 100 ms leaves no region and no file.

**AT-REC-4** [E] · F-REC-1 · Recording over an existing region places the new region on top (higher z), and the old region remains intact and becomes audible again if the new region is moved away.

**AT-REC-5** [E] · F-REC-2 · Four armed tracks on inputs 1, 2, 1+2 (stereo), 1, recording 1 bar, produce four files of identical length starting at the same sample, and one undo step removes all four regions.

**AT-REC-6** [HW] · F-REC-2 · With a real ASIO interface, 8 tracks record simultaneously at 48 kHz / 64-sample buffer for 10 minutes with no dropouts on the reference machine (Phase 2 defines it). **Decision D-AT-3** fixes the reference machine.

**Decision D-AT-3 (reference machine):** Windows 11 (23H2 or later), a 4-core/8-thread x64 desktop CPU at 3 GHz or more from 2019 or later, 16 GB RAM, the project on an SSD, Windows' "Best performance" power mode, and a USB audio interface with at least 8 inputs and its maker's ASIO driver. The test passes when the CPU meter's tooltip shows 0 dropouts and 0 disk underruns after the 10 minutes and every take plays back with no gaps.

**AT-REC-7** [E] · F-REC-3 · *Given* an instrument track with `TestSynth`, *when* MIDI note 60 vel 100 arrives at sample 48,000 and its note-off at 72,000 during recording from tick 0, *then* the recorded region contains one note at tick 1,920, length 960, velocity 100, and the region spans bar 1 (0–3,840).

**AT-REC-8** [E] · F-REC-3 · A note held at stop is closed at the stop position; CC64 held at stop gets a value-0 event appended; pitch bend and CC events are recorded with their channel.

**AT-REC-9** [E] · F-REC-3 · Recording MIDI with no events played creates no region.

**AT-REC-10** [E] · F-REC-4 · Monitoring Auto: armed and stopped → input audible at output; armed and playing (not recording) → not audible; recording → audible; not armed → not audible. Monitor On → always audible; Off → never.

**AT-REC-11** [E] · F-REC-5 · Cycle bar 1–2, record audio for 3.5 cycles, *then* the track has one take folder spanning bar 1–2 with 4 takes (the partial ≥ 1 beat), the 4th active, all referencing one file at offsets 0, 96,000, 192,000, 288,000 (plus latency shift).

**AT-REC-12** [E] · F-REC-5 · A final partial pass shorter than 1 beat is discarded (3 takes from 3.1 cycles where 0.1 cycle < 1 beat).

**AT-REC-13** [U][UI] · F-REC-5 · Clicking take 2 in an expanded folder makes it active; playback renders take 2's audio only.

**AT-REC-14** [E] · F-REC-7 · Cycle MIDI recording (merge default): pass 1 plays kick on beats 1/3, pass 2 snare on 2/4 → one region with all four notes.

**AT-REC-15** [E] · F-REC-7 · Linear MIDI recording over an existing region (replace default) creates a new region on top; in merge mode, notes go into the existing region.

**AT-REC-16** [E] · F-REC-8 · Autopunch bar 3–4, recording from bar 1 to bar 6: the new region spans bar 3 − 10 ms to bar 4 + 10 ms with 10 ms fades; audio outside the punch range in the output comes from the original region.

**AT-REC-17** [E] · F-REC-14 · An armed track fed `sine1k` at −6 dBFS shows a meter peak of −6 dBFS ± 0.1 while stopped.

**AT-REC-18** [U] · F-REC-15 · Recording bit depth 16 / 24 / 32-float produces WAV files with those formats; 32-float preserves samples > 1.0.

**AT-REC-19** [U] · F-REC-16 · With mocked free disk space equal to 9 minutes of the armed tracks' format, R shows the warning; Cancel doesn't start; Record anyway does. At 11 minutes, no warning.

**AT-REC-20** [E][UI] · F-REC-17 · With keyboard MIDI mode on (Ctrl+K) and an instrument track selected, pressing A sends note-on C3 (60) and releasing sends note-off; K sends C4 (72); X then A sends C4; C lowers velocity by 20 (clamped 1–127); notes are recorded when recording. With the mode off, letter keys run their normal commands and number keys jump to markers. Holding a key does not retrigger the note (key repeat ignored).

**AT-REC-21** [UI] · F-REC-18, L-REC-7 · Ctrl+. while recording stops, removes the take (its Record step is undone, so Redo brings it back) and returns the playhead to where recording started; when not recording it does nothing and says so.

**AT-REC-22** [E] · L-MREC-6, D-W2-4 · With MIDI Recording › Replace on, a one-bar pass from bar 1 deletes the existing notes on its track inside that bar, deletes a region left empty inside it, keeps notes after it, and adds the new region (even with Merge on); one undo restores it all.

**AT-REC-23** [E] · D-UI-41 · A track with monitoring On plays its input; with software monitoring off it doesn't; with Monitor only the selected track it doesn't while another track is selected, and does once it is selected.

**AT-REC-24** [E] · D-W1-12 · Playing from bar 1 with an armed track and a 2-bar count-in set, Record Toggle half a bar in starts recording at once without moving the playhead back; Record Toggle a bar later stops recording and playback goes on; the recording starts half a bar in, at least a bar long; stopping returns to bar 1.

**AT-REC-25** [E] · D-W1-12 · With Replace on, recording a bar from half a bar into a 2-bar audio region leaves its first half-bar, the recording, and its last half-bar, in one undo step "Record"; undo restores the 2-bar region.

**AT-REC-26** [E] · D-W1-13 · A note played at beat 3 while playing (not recording) is captured onto the instrument track as a region at bar 1 with the note at tick 1920, one beat long, undo step "Record"; an audio track is refused; a second capture finds nothing; playing again starts over.

**AT-REC-27** [UI] · D-W1-12 to D-W1-14 · Navigate lists Record Toggle (Alt+R), Capture as Recording (Ctrl+Shift+R) and Replace (unticked). Replace from the command lights the control bar's Replace button, and MIDI Recording › Replace turns it off again. Capture with nothing played says "Nothing to capture". With the auto record enable setting on (shown in Settings and kept), selecting an instrument track arms it and selecting another moves the arm to it.

## 4. Tracks (TRK)

**AT-TRK-1** [E] · F-TRK-1 · A mono audio track's region is output equally to L and R at center pan (−3 dB each per pan law); a stereo track preserves channel separation.

**AT-TRK-2** [E] · F-TRK-2, F-PLG-9 · An instrument track with Weft Keys and a MIDI region with one note produces non-silent output during the note and silence (< −90 dBFS) 2 s after its release.

**AT-TRK-3** [E] · F-TRK-4 · A bus receiving two sends sums them; the bus has no region lane and rejects region drops.

**AT-TRK-4** [U][UI] · F-TRK-6 · New Track dialog with count 3 creates "Audio 2", "Audio 3", "Audio 4" below the selected track; Delete removes the track, its regions, its automation, and sends targeting it if it is a bus (after confirmation); Duplicate copies name ("Audio 2 copy"), strip, and settings but no regions; "include regions" copies regions with new IDs.

**AT-TRK-5** [U][UI] · F-TRK-7 · Dragging track 3 above track 1 makes `trackOrder` [3,1,2]; the mixer order matches.

**AT-TRK-6** [UI] · F-TRK-8 · Double-clicking a name, typing "Vox", Enter renames; Esc cancels; empty name reverts.

**AT-TRK-7** [U] · F-TRK-9 · Setting track color to swatch 7 colors regions without their own color; regions with an override keep it; a newly recorded region inherits swatch 7 (null override).

**AT-TRK-8** [E] · F-TRK-10, F-MIX-4 · Mute on a track silences it and its sends; solo on track A silences B and C but keeps A's bus send audible (D-W6-1); soloing A and B plays both; Ctrl+click solo on C leaves only C soloed. The header and mixer strip buttons show the same state.

**AT-TRK-9** [S] · F-TRK-10 · Solo is local: client 1 soloing a track does not change client 2's document or output; mute is shared and appears on client 2.

**AT-TRK-10** [U][UI] · F-TRK-11 · Record enable toggles arm state, is stored in local view state (not the document), and is disabled on bus/master tracks.

**AT-TRK-11** [UI] · F-TRK-12 · Dragging a track's lower edge resizes it (min 24 px, max 400 px); Ctrl+Shift+wheel resizes all tracks.

**AT-TRK-12** [U][S] · F-TRK-16, F-COL-11 · See AT-COL-20 to AT-COL-23.

**AT-TRK-13** [E] · F-TRK-17, F-MIX-12 · Setting an audio track's input to channel 2 records input 2; setting output to Bus 1 routes it to Bus 1 instead of Master; an instrument track with MIDI filter channel 2 ignores channel-1 notes.

**AT-TRK-14** [UI] · L-TRH-3 · A track at the default height shows a volume slider and a pan knob on the header's second line; moving them sets the channel's volume and pan (one undo step per drag), the slider's bottom is −inf, and changes made in the mixer move them.

**AT-TRK-16** [U][UI] · L-TRH-10, D-W3-7 · Hide Track removes the track's header, lane and mixer strip while it stays in the track order; a region dragged down one row from the track above lands on the track below the hidden one, and a marquee skips it; the master can't be hidden; View › Show Hidden Tracks brings the row and the strip back with Unhide Track in its menu; hiding is one undo step and saved with the track.

**AT-TRK-17** [UI] · D-UI-19 · With a track selected, Shift+M mutes it (undo step "Mute Track") and unmutes it, S solos and unsolos it, Shift+R record-enables it and back. Ctrl+Alt+I adds one instrument track below the selected track and selects it; Ctrl+Alt+A adds an audio track the same way; each is one undo step; the Track menu lists all five commands.

**AT-TRK-18** [UI] · W-5, D-UI-19 · The + button above the track headers (inside the ruler's height, tooltip naming New Track and its shortcut) opens the New Track dialog; so does a double-click in the header column below the last track; a double-click on a header does not.

**AT-TRK-19** [UI] · F-TRK-10, D-UI-25 · With three audio tracks, Ctrl+click on track 2's Mute mutes it and unmutes tracks 1 and 3 in one undo step ("Mute Only"); Alt+click on track 1's Mute mutes all three ("Mute All") and Alt+click again unmutes them; Alt+click on Solo solos every track and again clears them; Ctrl+click on Record arms only that track. Pressing track 1's Mute and dragging over tracks 2 and 3 mutes all three in one undo step ("Mute Tracks"), and releasing doesn't toggle track 1 back; a swipe from a lit Solo turns the swiped ones off.

**AT-TRK-20** [UI] · F-TRK-6, W-1a, D-UI-32 · With a bus "Drums" and 2 device inputs, the New Track dialog set to 2 mono audio tracks, Input 1, Ascending, output Drums, Input monitoring and Record enable creates tracks on inputs 1 and 2, both out to Drums, monitoring On and record-enabled, as one undo step ("New Tracks"). Stereo lists Input 1-2; for an instrument track the input is disabled and the instrument menu starts with Weft Keys. The header-area menu is New Track, New Audio Track, New Instrument Track, Delete Unused Tracks, and its New Instrument Track adds one. Delete Unused Tracks with one instrument track holding a region leaves just that track and the bus (step "Delete Unused Tracks"); run again it changes nothing and says "No unused tracks."; undo brings the tracks back.

**AT-TRK-21** [UI] · D-UI-34 · With a mono audio track on input 1, out to bus "Drums", monitoring On and selected, Ctrl+Shift+N adds a mono audio track right below it on input 2, out to Drums, monitoring On, selected, as one undo step ("New Track"); from a track on the device's last input the new track has no input; Alt+click on + adds one too; with the bus selected nothing is added. From an instrument track with an instrument, the new track has the same instrument. Search and Select Track: "vocal LEAD" finds only "Lead Vocal", "drums" finds the bus, the master is never listed, and choosing a match selects it. The Track menu lists both commands.

**AT-TRK-22** [UI] · D-UI-37 · With two audio tracks and an instrument track, the optional header buttons are hidden until turned on in Configure Track Header (from the header menu, the header column menu or Studio); then each audio header shows On, M, S, R, I, H left to right, and the instrument header shows On but not I. A click on On turns the track off ("Turn Track Off"); Ctrl+click on another turns only that one on ("Turn On Only"); Alt+click sets every track ("Turn All Tracks Off"). Turn All Tracks On/Off turns them all on, then all off; a swipe down the On column is one undo step ("Turn Tracks On/Off") and leaves Mute alone. I switches monitoring On then back to Auto; H hides the track. Turning Volume off hides the slider and keeps pan; Track Numbers off moves the name left and Track Icons on moves it right. Dragging the column edge 60 px widens the headers and moves the lanes by 60 px, a long drag stops at 480 px, and a double-click returns to 208 px. The Track menu ends with Turn All Tracks On/Off and Configure Track Header (AT-TRK-4).

**AT-TRK-23** [E] · D-W3-13 · On an instrument track with a region transposed +2 and notes on channel 3, track parameters Transpose +12, Velocity +10, Key limit 48-76, Velocity limit 50-127, Out channel 5 and Delay 100 ms play note 60 at 100 as 74 at 110 on channel 5, 100 ms late, and 40 as 54; 30, 70 and a note at velocity 20 don't play; the stored notes are unchanged ("Track Parameters"). No Transpose drops the region's +2, and a 480-tick delay plays the first note half a beat late. A crossed key limit, a 961-tick delay, out channel 17 and an audio track are refused; undo is one step; the parameters survive a save. Live input on device "USB Keys": a note from it plays transposed and offset on channel 5, one from another device doesn't reach the track, the computer keyboard does, and notes outside the limits don't play.

**AT-TRK-24** [UI] · D-W3-13 · The inspector shows track parameters for an instrument track and not an audio track; MIDI In lists All and the connected devices. A Transpose drag is one undo step ("Track Parameters") and the slider follows undo. Key limit high 40 then low 60 makes both 60. Delay 25.5 ms, then the ticks unit sets it to 0 with a ±960 range, and 240 reads "240". Out channel 10 and No Transpose are set; MIDI In "Pads" with channel 2 sets the track's input, and once Pads is unplugged the menu still shows it for that track.

**AT-TRK-25** [U] · D-W6-17 · A summing stack of C and A: main "Stack 1" before A, then A, C (in track order); both play into it and it plays to Verb, where A did; A can't join another stack; closed, the rows are the main, B and Verb. Flatten sends A and C to Verb; undo brings the stack back, and undoing its open, close and creation restores the order and A's output. A folder stack of B and C leaves their outputs, refuses being an output or send target, and flattening restores the order.

**AT-TRK-15** [E] · L-TRH-7 · Turn Track Off (track header menu) on an audio or instrument track: it plays nothing and its sends go quiet, it records nothing while armed, the header reads OFF; Turn Track On restores it; buses and the master can't be turned off; each is one undo step.

## 5. Arrange (ARR)

**AT-ARR-1** [UI] · F-ARR-1 · The ruler shows bar numbers that match the signature map; Ctrl+wheel zooms around the pointer position (the bar under the pointer stays under it within 2 px).

**AT-ARR-2** [U] · F-ARR-2 · With snap 1/16 (240 ticks), dragging a region whose start is 1,000 by +130 ticks lands at 1,200 (start snapped, D-W3-1); with Alt held it lands at 1,130; snap Off lands at 1,130.

**AT-ARR-3** [U] · F-ARR-2 · Triplet grid 1/8T = 320 ticks; smart snap at a zoom where one bar is 40 px uses 1 bar.

**AT-ARR-4** [UI] · F-ARR-3 · Click selects one; Shift+click toggles; rubber-band selects all regions it intersects across tracks; Ctrl+A selects all; click on empty deselects.

**AT-ARR-5** [U] · F-ARR-4 · Moving two selected regions (track 1 at bar 1, track 2 at bar 3) by +1 track and +2 bars lands them on tracks 2 and 3 at bars 3 and 5; a MIDI region dragged onto an audio track stays on its original track.

**AT-ARR-6** [U] · F-ARR-4 · Dragging below the last track creates a new track of the region's type; Esc during drag cancels with no document change.

**AT-ARR-7** [U] · F-ARR-5 · Ctrl+drag copies (new IDs, same media/sequence copy); Ctrl+C, move playhead to bar 9, select track 3, Ctrl+V pastes the leftmost copied region at bar 9 on track 3, others keep relative offsets.

**AT-ARR-8** [U][E] · F-ARR-6 · Trimming an audio region's left edge right by 4,800 samples increases `sourceOffset` by 4,800 and the region's audio at a given timeline sample is unchanged; extending the right edge past the source end stops at the source end; MIDI regions can extend past their last note.

**AT-ARR-9** [U][E] · F-ARR-7 · Ctrl+T at bar 2 on an audio region bar 1–3 yields two regions bar 1–2 and bar 2–3, same media, offsets 0 and 96,000; playback output is sample-identical to before the split. Fade-in stays on the left piece, fade-out on the right.

**AT-ARR-10** [U] · F-ARR-7 · Splitting a MIDI region with a note crossing the split keeps the note whole in the left region (D-W3-3); the right region gets a new sequence (D-DM-3a); editing a note in one does not affect the other.

**AT-ARR-11** [U][E] · F-ARR-8 · Joining two MIDI regions on one track yields one region containing all notes at unchanged absolute positions. Joining two contiguous pieces of one audio file at original offsets yields one region without rendering; joining non-contiguous audio renders a new file whose playback equals the original pair's output (± 1 LSB at 24-bit).

**AT-ARR-12** [U] · F-ARR-9 · Delete removes selected regions and leaves later regions in place; media files remain on disk.

**AT-ARR-13** [E] · F-ARR-10 · A 4,800-sample linear fade-in on a constant 1.0 signal yields sample values i/4800 for i in 0..4799 (± 1e-6); equal-power yields sin(π/2 · i/4800); S-curve is monotonic and symmetric about the midpoint. Fade lengths shrink proportionally when the region is trimmed shorter than their sum.

**AT-ARR-14** [E] · F-ARR-11 · Two overlapping regions with a 9,600-sample crossfade: at the overlap midpoint, the summed gain of two correlated equal signals with equal-power curves is +3 dB relative to a single signal ± 0.1 dB (equal-power property), and there are no discontinuities (max sample-to-sample delta below threshold on a sine).

**AT-ARR-15** [E] · F-ARR-12 · Region gain −6.02 dB halves the amplitude (± 0.01 dB); −inf silences; +24 dB is the max.

**AT-ARR-16** [E] · F-ARR-13 · Looping a 1-bar MIDI region to the end of bar 4 plays its notes 4 times; ending the loop at bar 3 beat 3 plays 2.5 iterations with notes past bar 3 beat 3 not played; editing a note in the source changes all iterations.

**AT-ARR-17** [E] · F-ARR-14 · M on a region silences it in playback and bounce; M again restores.

**AT-ARR-18** [U] · F-ARR-15 · Renaming a region and setting its color store `name` and `color` on the region only; null color inherits track color.

**AT-ARR-19** [U] · F-ARR-16 · For each edit type (create/move/trim/split/join/delete/fade/gain/mute region, add/move/delete note, quantize, add plugin, change send, add automation point, tempo change), the sequence edit → undo yields a document equal to the original, and → redo equals the edited state.

**AT-ARR-20** [U] · F-ARR-16 · A drag consisting of 200 mouse-move events produces exactly one undo step.

**AT-ARR-21** [U][UI] · F-ARR-18 · ; adds a marker named "Marker N" at the playhead; renaming, moving (snapped), deleting work; clicking a marker moves the playhead to it.

**AT-ARR-22** [UI] · F-ARR-21 · Z with two regions selected zooms so both fill ≥ 80% of the visible width; with nothing selected, zooms to show the whole project.

**AT-ARR-23** [UI] · F-ARR-22 · T opens the tool menu; choosing pencil and clicking an empty instrument track lane creates an empty 1-bar MIDI region at the snapped position; scissors splits at click; glue joins the clicked region with the selected; eraser deletes; Esc returns to pointer.

**AT-ARR-24** [U] · F-ARR-23 · Alt+Right with grid 1/16 moves selected regions +240 ticks; Alt+Left at tick 100 clamps to 0.

**AT-ARR-25** [E] · F-ARR-26 (D-DM-4, D-ARR-7) · Dropping a 44.1 kHz stereo MP3 at bar 3 on an audio track creates a region at bar 3 whose media is a 48 kHz WAV (converted), hashed, in `Media/`; dropping 3 files below the last track creates 3 new tracks; unsupported files are rejected with a message naming them; a 6-channel file becomes 6 mono tracks; a WAV cut to a third of its length imports that third and the message says it's shorter than its header.

**AT-ARR-26** [E] · F-ARR-27 · Dropping a type-1 SMF with 3 tracks and a tempo of 100 BPM creates 3 instrument tracks with Weft Keys and the notes at correct tick positions (converting the file's PPQN to 960); the project tempo is unchanged unless the project is empty, in which case it adopts the file's tempo map. **Decision D-AT-1** recorded here.

**AT-ARR-27** [U][UI] · F-ARR-28 · With markers at bars 5, 9, 17 and the playhead at bar 10, Alt+, goes to bar 9 and Alt+. goes to bar 17; pressing 1 goes to the first marker by position (bar 5); a number with no marker does nothing.

**AT-ARR-28** [U] · F-ARR-29 · Ctrl+R with a 2-bar region at bar 3 selected creates a copy at bar 5 with a new ID and the same content; repeating ×3 in the dialog creates copies at bars 5, 7, 9; a multi-region selection repeats as a block after its last end; one undo step.

**AT-ARR-29** [E] · L-RGN-12 · Reverse Regions (Edit menu, audio region menu) writes the region's part of its file backwards into `Media/` and plays it from the start: the first sample is the old last one; position, length, gain and fades stay; one undo step "Reverse"; reversing again plays forwards; MIDI regions are refused.

**AT-ARR-30** [E] · L-RGN-6, D-W3-5 · Region Delay +100 ms plays a MIDI region's notes 4,800 samples later at 48 kHz (−10 ms: 480 earlier) and an audio region's first sample 10 ms late; the region doesn't move; values outside −500…+500 ms are refused.

**AT-ARR-31** [UI] · D-UI-18 · With region A of A, B, C selected, Ctrl+Shift+I selects B and C only; with C's track hidden and nothing selected it selects A and B; Ctrl+Shift+A leaves nothing selected; the Edit menu lists both commands.

**AT-ARR-32** [UI] · D-W3-8 · With A and B selected, a Mute-tool click on A mutes both as one undo step and leaves C alone; a click on unselected C mutes only C; an Eraser-tool click on B deletes A and B.

**AT-ARR-33** [UI] · D-W3-10 · Dragging a region mostly sideways and one row down with Shift held moves it in time and keeps its track; dragging mostly down with Shift moves it one track down and keeps its position.

**AT-ARR-34** [UI] · D-W3-9 · The right-click menu of an empty audio lane has Import Audio... and no Create MIDI Region; an instrument lane has Create MIDI Region and no Import Audio...; choosing Create MIDI Region at bar 3 makes a one-bar MIDI region there, selected.

**AT-ARR-35** [UI] · D-UI-20 · With markers at bars 3 and 7 and the global tracks shown, the ruler has no flags; hidden, it has one flag per marker at the marker's position inside the ruler, and clicking the second moves the playhead to bar 7.

**AT-ARR-36** [U][UI] · F-ARR-28, D-UI-23 · A marker's right-click menu is Rename Marker..., Color, Delete Marker; choosing the third swatch stores palette colour 2 (undo step "Marker Color"), No Color clears it, a palette index outside 0–23 is refused, Delete removes the marker. Empty marker-lane space offers Add Marker Here and adds at the snapped position. In the tempo lane empty space offers Add Tempo Change Here; a change offers Set Tempo... and Delete Tempo Change, with Delete disabled on the first tempo. In the signature lane a change offers Edit and Delete (Delete disabled on the first), empty space Add Time Signature Here.... There is no menu on the ruler itself.

**AT-ARR-37** [UI] · F-ARR-28, D-UI-26 · With twelve markers, Ctrl+Numpad 1 goes to the eleventh and Ctrl+Numpad 0 to the tenth; Ctrl+Numpad 9 with only twelve does nothing. Ctrl+click on Next Bar from bar 1 goes to the first marker and Ctrl+click on Previous Bar goes back to the one before; a plain click moves one bar. Alt+Delete with the playhead just after a marker deletes that marker (undo step "Delete Marker"); with no marker at or before the playhead nothing changes and the status says why. The Navigate menu lists Rename Marker... and Delete Marker.

**AT-ARR-38** [UI] · F-ARR-21, D-UI-27, D-UI-28 · With regions at bars 1, 3 and 5 on one track and the bar-3 region selected, Right selects the bar-5 region and Left twice the bar-1 region; Left at the first does nothing; with nothing selected and the playhead at bar 2, Right selects the bar-3 region. Z fits the selected region; Z again fits the whole project; Z after changing the selection fits the new selection.

**AT-ARR-39** [UI] · D-UI-36 · Text tool: clicking an audio region and entering "Lead take" renames it. Fade tool: a drag from 10% to 25% of the region's width sets a fade in of about a quarter of its length and no fade out (step "Fade"); a drag from its right edge to the middle sets a fade out. Gain tool: dragging up 24 px makes the gain +6 dB (step "Region Gain"); undo makes it 0 dB. Quantize tool: clicking a MIDI region with a note 250 ticks into the bar and the quantize grid at 1/16 moves the note to 240. Zoom tool: a box from bar 3 to bar 4 shows exactly bars 3–4; a click doubles the zoom and Alt+click halves it. Next Tool after Quantize is Pointer and Previous Tool goes back. The Tool menu lists Pointer, Pencil, Scissors, Glue, Eraser, Mute, Text, Zoom, Fade, Gain, Quantize with keys T P I J E M N Z A G Q.

**AT-ARR-40** [UI] · D-W3-11 · With MIDI regions A (bar 1) and B (bar 4) on one track: Nudge Value › Beat then Nudge Right moves B one beat; Tick then Nudge Left moves it back one tick. With Bar, Lengthen makes A two bars (step "Region Length") and Shorten one again. Shuffle Left moves B to A's end (step "Shuffle") and again does nothing; Shuffle Right moves A to end at B's start; A alone at half a bar shuffles left to tick 0. With Beat, Ctrl+Alt+Left slips A's content a beat (sequence offset +480, start unchanged, step "Slip Region"); an audio region slips left to source offset 24,000 and right back to 0, and can't slip right past 0. View › Grid Lines turns the grid off and the menu shows it unticked.

**AT-ARR-41** [UI] · D-UI-38 · With 13 tracks: the horizontal zoom slider shows the zoom on its log scale and moving it sets the zoom; the vertical slider at 80 makes every track 80 px; the sliders sit left of +. Ctrl+Down makes the tracks 100 px and Ctrl+Up 80 px. Save as Zoom 1 at bar 5, then Zoom In, Zoom Vertical In and a scroll to the start: Ctrl+Alt+1 restores the zoom, bar 5 and 80 px; Recall Zoom 2 unsaved does nothing. Navigation: Back gives 100 px, Back again 80 px at the zoomed-in level, Forward 100 px. Alt+Page Down moves the view one width right; Page Left twice returns to the start and stops there. Page Down scrolls the tracks and Page Up back to the top. Zoom Selected Track makes the selected track taller and leaves the others; again restores it. Ctrl+drag of a header's lower edge by 20 px sets every track 20 px taller.

**AT-ARR-42** [UI] · D-W3-12 · On Keys (regions at bars 1, 3 (muted) and 5) and Bass (bars 3-4), with the bar-1 and Bass regions the same colour: Shift+F from the bar-3 region selects it, bar 5 and Bass; with the cycle at bars 3-4, Shift+L selects the bar-3 and Bass regions; Select Muted Regions selects the bar-3 one; Select Same-Colored from Bass adds bar 1; on Keys, Select Last picks bar 5 and Select First bar 1; Shift+Right adds bar 3; from bar 5 Shift+Left adds bar 3. Down selects Bass, Up Keys. Move Region to Playhead and Select Next with the playhead at bar 4 moves the bar-3 region there and selects the bar-5 one; Undo moves it back. With the playhead at bar 4 Set Region End makes the Bass region one bar long ("Set Region End"), and Set Region Start starts it at bar 4; with the playhead outside it nothing happens. With the cycle from bar 3½ to 4½, Split Regions at Cycle makes Bass three regions as one undo step ("Split at Cycle"). Scroll to Selection brings a region at bar 201 into view.

**AT-ARR-44** [U] · D-UI-45 · Sections are added as one undo step "Add Section", named Intro then Verse; one inside another or before the start is refused. Rename (not to nothing), Section Color and Resize Section work, a resize onto another section is refused; sections survive saving and reopening; Delete Section removes one.

**AT-ARR-45** [UI] · D-UI-44, D-UI-45 · Configure Global Tracks lists the five tracks and the commands; showing Key and Arrangement gives five lanes, Arrangement on top and Key at the bottom, and moves the tempo lane down. Hiding Tempo removes its lane; after clicking the Key lane, Show Single Global Track shows only it; Marker Track Only shows only markers and then restores the rest. Typing F# minor in the Key lane's menu at bar 5 adds a change the control bar shows after it; dragging it to bar 7 is one undo step "Move Key Change". In the Arrangement lane, Add Section Here at bar 2 makes an 8-bar section, Rename makes it "Verse", dragging its end to bar 6 resizes it, and clicking it selects the region inside and goes to bar 2; Add Arrangement Section adds the next one after it. Dragging the first section past the second shows the drop at the second's end and moves it and its region there as "Move Section"; Ctrl-dragging it makes "Copy Section", with a third section and a copied region. With Markers protected, Add Marker does nothing and says the track is protected, the marker's menu is disabled, and dragging the marker moves only the playhead; clicking the lock unprotects it. Create Time Signature Change at bar 3 beat 2 sets 3/4 from bar 4; Create Key Change sets E♭ major from the playhead. The track choice survives a restart.

**AT-ARR-46** [U] · D-UI-45 · With Intro (bars 1-4) and Verse (5-8), a drop inside a section is refused and a drop at the section's own edge changes nothing. Moving Intro to the end of Verse is one undo step "Move Section": Verse starts at bar 1, the region crossing their edge is split, and the regions, marker and key change go with their section; undo restores all of it. Copying Verse to the start is "Copy Section": a twin Verse at bar 1 with copies of both its regions, everything else one Verse later; undo removes them.

**AT-ARR-47** [U] · D-UI-48 · A marker's text is set as undo step "Marker Text"; the same text again, or text for a missing marker, is refused; it survives saving and reopening; undo clears it.

**AT-ARR-43** [UI] · D-UI-39 · With markers Intro (bar 1), Verse (bar 3), Chorus (bar 5) and a selected region "Bridge" at bar 7, Shift+; adds a marker "Bridge" at bar 7. With the playhead at bar 4 Set Cycle to Marker sets bars 3-5 with Cycle on and the playhead unmoved; Set Cycle to Next Marker sets bars 5-7 and moves the playhead to bar 5; Previous goes back to bars 3-5. From bar 8, Set Cycle to Marker runs from bar 7 to the project end. With Cycle off at bar 2, Ctrl+Alt+Shift+. goes to bar 3 and sets bars 3-5, Cycle still off; Previous Marker and Set Locators goes to bar 1 with bars 1-3. Go to Marker Number 3 goes to bar 5; 9 does nothing.

## 6. Audio editor (EDA)

**AT-EDA-1** [UI] · F-EDA-1 · Double-clicking an audio region opens the audio editor showing the full source with the used part highlighted; trimming in the editor with snap off changes the region by single samples at maximum zoom; the change is reflected in the arrange view.

**AT-EDA-2** [UI] · L-ATE-3, D-W3-6 · With View › Snap Edits to Zero Crossings on, dragging a region edge in the audio editor lands on the nearest sample, within 10 ms, where the summed signal crosses zero (or the file's start or end); with none in reach the edge goes where it was dragged.

**AT-EDA-3** [E] · D-UI-49 · Inverting frames 100 to 200 of a file changes only those and moves both regions playing it to the new file (keeping its name), one undo step "Edit File: Invert"; undo plays the old file. Normalize with nothing selected brings the peak to 1.0; Change Gain -6 dB halves, Silence zeroes, Fade In ramps, Remove DC Offset centres and Reverse flips a selection. Deleting 1000 frames before region b moves b back 1000 frames and leaves a; deleting inside a shortens it; deleting all of b is refused. Pasting 500 copied frames over 100 moves b 400 frames on. Trim keeps 2000 to 14000 (a and b follow; "Trim Audio File"); a trim that would empty a region is refused, as is one with nothing selected.

**AT-EDA-4** [UI] · D-UI-49 · The audio editor's zoom slider goes from the whole file to 16 px a sample; the amplitude scale reads 100%, 50%, 0 (25% with Wave Zoom; 0 dB, -6 dB, -inf in decibels) and the sample ruler starts at 0; Snap toggles the tracks area's snap. W turns on the Audio File Editor with Functions; dragging selects without trimming. Silence changes the region's file and undo restores it; copying 12000 frames and pasting at 24000 makes the file and region 60000 long and selects the pasted part; Cut takes it out again. Adjust Tempo Using Selection Length on 0.75 s at 120 BPM gives 160 BPM. Play starts and stops the preview. W hides the editor; Alt+double-click opens the region in an Audio Editor window with File on.

## 7. Piano roll (PRL)

**AT-PRL-1** [UI] · F-PRL-1 · Double-click a MIDI region opens the piano roll; the keyboard labels MIDI 60 as C3; selecting two regions on one track shows notes of both with boundary lines.

**AT-PRL-2** [U][UI] · F-PRL-2 · Pencil click at tick 1,000 with grid 1/16 creates a note at 960, length 240 (default), velocity 100, channel 1; the next note uses the last-used length and velocity. Creating a note past the region end extends the region to the next bar; clicking before region start does nothing.

**AT-PRL-3** [UI] · F-PRL-3 · Clicking piano key E3 selects all E3 notes; Shift+clicking key G3 adds G3 notes; rubber-band selects notes it touches.

**AT-PRL-4** [U] · F-PRL-4 · Up arrow transposes selected notes +1; Shift+Up +12; with one selected note at 120, Shift+Up moves none (clamped as a group, since 132 > 127); Alt+Right with grid 1/8 moves +480 ticks (D-UI-28).

**AT-PRL-5** [U] · F-PRL-5 · Dragging a note's right edge from length 240 to 500 with snap 1/16 yields 480; dragging the left edge keeps the end fixed; minimum length 1 tick with snap off.

**AT-PRL-6** [U] · F-PRL-6 · Delete removes selected notes; eraser click removes the clicked note.

**AT-PRL-7** [U] · F-PRL-7 · Ctrl+C three notes, move playhead, Ctrl+V pastes them at the playhead keeping relative offsets, with new IDs, into the region under the playhead on the shown track.

**AT-PRL-8** [U][UI] · F-PRL-8 · Dragging a velocity-lane bar sets that note's velocity; velocity tool drag of +20 on selected notes [50, 100, 120] yields [57, 107, 127] (it stops when the first note reaches the limit, D-W4-13), the same drag with Alt then yields [77, 127, 127], and with Alt+Shift a drag of −10 on the note at 100 sets all three to 90; typing 64 in the inspector with 3 notes selected sets all three; note color brightness is monotonic in velocity.

**AT-PRL-9** [E] · F-PRL-8 · With `TestSynth`, a note at velocity 64 outputs amplitude 64/127 ± 0.5%.

**AT-PRL-10** [U] · F-PRL-9, F-QNT-2 · Quantize 1/16, strength 100%: note starts [250, 470, 1,000] → [240, 480, 960]; lengths unchanged. Strength 50%: [245, 475, 980]. Tie at exactly half-grid (120) goes earlier (0).  A note that would quantize to before the region start is clamped to the region start.

**AT-PRL-11** [U] · F-PRL-9 · Swing 100% on 1/16: grid points 0, 240, 480, 720 become 0, 320, 480, 800 (off-beats shifted by g/3); swing 0% is straight; swing 50% shifts off-beats by 40 ticks.

**AT-PRL-12** [U] · F-PRL-9 · Quantizing twice at 100% equals quantizing once (idempotent). Quantize applies to all notes in shown regions when none selected.

**AT-PRL-13** [E] · F-PRL-11 · With audition on, clicking a note sends note-on/off to the instrument at its velocity, max 1 s; with audition off, nothing is sent.

**AT-PRL-14** [U][E] · F-PRL-12 · Drawing CC1 points at ticks 0 (value 0) and 3,840 (127) in the controller lane stores two CC events; playback sends CC1 0 at bar 1 and 127 at bar 2; pitch-bend lane values −8192..8191 are sent as 14-bit pitch bend.

**AT-PRL-15** [U][E] · F-PRL-15 · Ctrl+M mutes selected notes; muted notes are not sent at playback or bounce; Ctrl+M again unmutes.

**AT-PRL-16** [E] · D-W4-9 · A MIDI region with Transpose +12 and Velocity +20 plays C3 at 100 as C4 at 120 without changing the stored note; −96 and +99 clamp the played note to pitch 0 and velocity 127; 97 and −100 are refused; each change is one undo step; audio regions refuse both.

**AT-PRL-17** [UI] · F-PRL-5, D-W4-12 · With notes of length 240 (at 960) and 480 (at 1200) selected and snap 1/16, dragging the first note's right edge to 500 yields lengths 480 and 720, leaves an unselected note alone, and undoes in one step; the same drag with Shift to tick 2400 ends both notes at 2400; with Alt+Shift both get the grabbed note's new length; dragging the first note's left edge back by 240 moves both starts by 240 and keeps both ends.

**AT-PRL-18** [UI] · D-W4-10, D-W4-11 · Dragging a note two grid steps right and two semitones up with Shift moves it in time only; dragging it mostly upward with Shift changes its pitch only; without Shift both change. After clicking a note of length 480 and velocity 64, a pencil click draws a note of length 480 and velocity 64.

**AT-PRL-19** [UI] · F-PRL-4, D-UI-28 · With notes at ticks 0 (pitch 60), 480 (64) and 480 (60) selected one at a time, Right steps 0/60 → 480/60 → 480/64 and Left steps back; with no note selected and the playhead at tick 240, Right selects 480/60. Alt+Right moves the selected note one grid step; Alt+Left moves it back.

**AT-PRL-20** [U][UI] · D-W4-14 · Notes at ticks 0, 480 and 1200 (all 240 long, pitch 60) and 480 (pitch 64): Legato on the first three makes them 480, 720 and 240 long (480/64 unselected stays 240); a chord at 0 and 0 ends together at the next start. Remove Overlaps shortens a 960-long note at 0 to 480 when the same pitch starts at 480, and leaves it alone when only another pitch does. With the playhead at 120, Note Start to Playhead makes a 0–240 note 120–240 and leaves a note at 480; Note End to Playhead (from the original notes) leaves the note at 480 alone and makes the note at 0 end at 120. Lengthen adds one snap step; Shorten never goes below one tick. Select All Following from a selected note at 480 selects every note starting at 480 or later. Each edit is one undo step with its name.

**AT-PRL-21** [U][UI] · D-W4-15 · Scale Quantize with C Major moves C#3 to C3 (halfway goes down), F#3 to F3, and leaves E3; with A Minor Pentatonic A#3 goes to A3, B3 to C4 and C#4 to C4; pitch stays in 0–127. The piano roll's second toolbar row shows the regions' names, the selected note's velocity (dragging it sets every selected note as one undo step "Note Velocity"), the Scale root and Scale menus with Scale Q, and the info line names the pitch and bar position under the pointer.

**AT-PRL-22** [E] · D-W4-16 · Notes at velocity 100 and 40 play at 82 and 52 with Dynamics 2:1 ("Region Dynamics"), and both at 74 with Fixed and Velocity +10; 301% is refused. Gate 50% plays a 480-tick note for 240, Fixed for 30, and Legato ends it where the next note starts while the last keeps its length; 201 is refused. A note that runs 720 ticks past the region's end stops there, and with Clip Length off plays its whole length. On a 1/16 grid with Q-Range +30, a note 10 ticks late moves and one 60 late stays; with −30 the opposite. Q-Flam +20 spreads a three-note chord low to high 20 ticks apart, −20 high to low; a range over 3840 is refused. A 480-tick delay plays the first note half a beat late, 961 is refused, and setting milliseconds turns ticks off. The settings survive a save.

**AT-PRL-23** [E] · D-W4-16 · A note at 250 (length 480, pitch 60, velocity 100) in a region with 1/16 quantize, Transpose +2, Velocity +10, Gate 50% and a 120-tick delay: Apply All Except Delay writes it as 240, 240, 62, 110, resets those parameters, keeps the delay and plays exactly as before ("Apply All Except Delay"); Apply All Parameters then moves it to 360 and resets the delay ("Apply All Parameters"); two undos bring back the original note and settings.

**AT-PRL-24** [UI] · D-W4-16 · With a MIDI region selected the inspector shows Dynamics Off, Gate Time Off, Clip Length ticked, delay in ms and no Reverse. Dynamics 2:1, Gate Legato and Clip Length off set the region. Q-Range and Q-Flam are disabled until a quantize value is chosen, then set it keeping the grid. The ticks unit sets the delay to 0 with a ±960 range, and 240 reads "240". Edit › MIDI Region Parameters lists Apply All Parameters Permanently and Apply All Except Delay; the second resets the region's quantize and dynamics and keeps the delay. An audio region shows Reverse unticked; ticking it reverses the region ("Reverse") and it shows ticked.

**AT-PRL-25** [U] · D-UI-48 · Changing a controller event's position, number, value and channel is one undo step "Change Event"; an unchanged event, a value of 128, channel 17, a position before the region or a missing event is refused; undo restores it.

## 8. Quantize (QNT)

**AT-QNT-1** [U][E] · F-QNT-1 · Setting region quantize 1/8 at 100% plays notes at quantized positions while stored positions are unchanged; setting Off restores original timing in playback; Apply Quantize bakes positions (stored positions equal quantized) and sets region quantize Off, in one undo step.

**AT-QNT-2** [U] · F-QNT-1 · Region quantize combines with destructive quantize as specified: destructive edits change stored positions, and region quantize is applied on top at playback.

**AT-QNT-3** [U] · F-QNT-2 · Q in the piano roll is one undo step named "Quantize".

## 9. Mixer (MIX)

**AT-MIX-1** [UI] · F-MIX-1 · The mixer shows one strip per track in track order, then buses, then Master pinned on the right; selecting a strip selects its track.

**AT-MIX-2** [E] · F-MIX-2 · Fader at −6.02 dB halves amplitude; −inf silences; max +6 dB (higher values are refused, AT-MIX-14); Alt+click resets to 0 dB; volume changes during playback ramp over ≤ 1 buffer with no step discontinuity (max delta check).

**AT-MIX-3** [E] · F-MIX-3 · Mono source, pan center: L and R each at −3.01 dB; hard left: L at 0 dB, R silent; hard right: inverse. Stereo source with balance −100 silences R and leaves L at 0 dB.

**AT-MIX-4** [E] · F-MIX-5 · `sine1k` at −6 dBFS shows meter peak −6.0; a sample of 1.1 lights the clip indicator, which stays lit until clicked; peak hold displays the max for 2 s.

**AT-MIX-5** [E] · F-MIX-6 · Insert `TestGain` (+6 dB) then built-in EQ: output equals EQ(gain(x)); swapping order changes processing order; bypass passes audio unchanged (bit-exact); removing the plugin restores the dry signal; a 16th insert is refused (max 15).

**AT-MIX-6** [E] · F-MIX-7 · Instrument slot hosts Weft Keys or `TestSynth`; replacing the instrument preserves the track's regions and inserts.

**AT-MIX-7** [E] · F-MIX-8 · A post-fader send at 0 dB to Bus 1 with track fader −6 dB delivers the signal at −6 dB to the bus; switching to pre-fader delivers 0 dB regardless of fader; send level −inf delivers silence; a 9th send is refused; a second send to the same bus from one channel is refused.

**AT-MIX-8** [U][UI] · F-MIX-9 · Choosing New Bus from a send creates "Bus 1" with output Master, fader 0 dB, a send at 0 dB, and a bus track in the tracks area; removing the bus asks for confirmation, removes sends to it, and reroutes channels that output to it to Master.

**AT-MIX-9** [U] · F-MIX-10 · Routing track A → Bus 1 → Bus 2 is allowed; then setting Bus 2's output to Bus 1, or adding a send from Bus 2 to Bus 1, is disabled with "Would create a feedback loop".

**AT-MIX-10** [S] · F-MIX-10 · Concurrent edits creating a cycle (client 1: Bus 1 → Bus 2, client 2: Bus 2 → Bus 1, both offline, then reconnect) converge to the same state on both clients, with exactly one edge (the later by CRDT order) shown disabled, and audio output contains no feedback.

**AT-MIX-11** [E] · F-MIX-11 · All track outputs sum at the master; master fader −6.02 dB halves the bounce amplitude; a master insert processes the full mix.

**AT-MIX-12** [E] · F-MIX-13 · Track A with `TestLatency` (1,000 samples) and track B with none, both playing an impulse at bar 2: the two impulses arrive at the master at the same sample. Removing `TestLatency` keeps them aligned. A send from A to a bus is also aligned with B.

**AT-MIX-13** [E] · F-MIX-13 · Changing compensation during playback produces no discontinuity larger than the 10 ms crossfade allows.

**AT-MIX-14** [U] · F-MIX-1 · Volume above +6 dB, pan beyond ±1 and region gain above +24 dB are refused with a message, not clamped; values below −inf dB store as −inf.

**AT-MIX-15** [E][UI] · F-MIX-6, D-W6-9 · An insert with a parameter set and bypass on, moved to another channel's first slot, leaves its channel and arrives with the same id, parameter and bypass ahead of the insert already there (undo step "Move Plug-in", one undo puts it back). Copied instead, the original stays and the target gets a new instance with the same settings at the end when the slot index is past it (step "Copy Plug-in"); a copy within one strip works. A channel with 15 inserts refuses both. In the mixer, dragging a slot onto another strip's slot moves the plug-in and its window stays open; Ctrl+drag copies; Alt+click removes without opening the editor.

**AT-MIX-16** [U][UI] · F-TRK-10, D-W6-1, D-UI-31 · With track A output to a bus and track B on its own, soloing A leaves A and the bus audible and B silenced (`audibleWithSolo`), and soloing the bus instead leaves A audible too; B's Mute button in the header and the mixer carries the silenced-by-solo tint, A's and the bus's don't; clearing the solo clears the tint. In the mixer Alt+click on a Mute mutes every strip but the master.

**AT-MIX-17** [UI] · F-MIX-6, D-W6-10 · Shift+click on a filled insert slot bypasses it without opening its window. Pressing the first slot's bypass dot bypasses it; dragging across the next slot's dot and another channel's first dot bypasses them too, a free slot is skipped, and it is one undo step ("Bypass Plug-ins") that undo reverses for all three. Insert Before › Weft Delay on slot 2 puts the delay in slot 2 and the plug-in that was there in slot 3.

**AT-MIX-18** [UI] · D-W6-13 · A strip's peak number reads "-inf", then "+6.0" in red after a peak of 2.0 (linear), stays at "+6.0" after a lower one, and a click on it gives "-inf" and turns the clip light off; it sits right of the volume readout. With two tracks muted, Mute Off for All unmutes both as one undo step ("Mute Off for All"), and with none muted does nothing. With two tracks soloed, Clear/Recall Solo clears them, then brings both back; after a new solo, it brings back only the new one. With regions on two instrument tracks selected, Solo Tracks of Selected Regions solos exactly those two; with nothing selected it does nothing. Shift+I sets the selected audio track's monitoring On, and again Auto. Copying a strip with -6 dB, pan 0.3 and Weft EQ (Low Cut on) and pasting it on another track gives that track -6 dB, pan 0.3 and its own Weft EQ (a new id) with Low Cut on, as one undo step ("Paste Channel Strip Setting"); Undo takes it all back.

**AT-MIX-19** [UI] · D-W6-14 · With an audio track, instrument track Keys out to bus Drums, Drums sending to bus Verb, and a hidden audio track: Tracks (the default) shows audio, Keys, Verb, Drums; All adds the hidden track after Keys. With Bus off the buses go; Alt+click on Inst leaves only Keys; turning Audio and Bus back on shows all five. Single with Keys selected shows Keys, Verb and Drums; selecting the audio track shows only it; the master stays. Cycle Through Mixer Views goes to Tracks, then All, then Single. The buttons sit above the strips.

**AT-MIX-20** [U] · D-W6-15 · Tracks A, B and C in a new group "Group 1" with B at -10 dB: A's volume to -6 dB puts B at -16 and C at -6, one undo step, and a track outside stays; B to 0 dB stops A at +6. Mute links by default, pan doesn't; with pan, colour, automation mode and hide ticked and mute unticked, B's pan +0.25 moves A to -0.25, A's colour, mode and Hide reach C, and A's unmute leaves B muted. With send slot 1 linked, A's send from -10 to -4 dB moves B's from -20 to -14. The override, the group turned off, and groups turned off each leave B unmuted when A mutes. H hides the members' rows (shown again with groups off); deleting the group frees its tracks, undo puts them back. New groups take the lowest free number.

**AT-MIX-21** [U][E] · D-W6-16 · New VCA is "VCA 1" at 0 dB; a track assigned to it, VCA -6 dB, mute, rename "Drums"; the master can't be assigned; deleting it unassigns the track and undo brings both back. Playing a 0.5 constant through a track at -6.02 dB on a VCA at -6.02 dB gives 0.125; the VCA's mute gives silence; unassigned, 0.25.

**AT-MIX-22** [UI] · D-W6-15, D-W6-16 · New Group from A's Group slot shows "1 Group 1"; Alt+click on B's slot puts B in it (ticked in its menu). A's fader to -6 dB moves B's to -6; with the override, A to -12 leaves B. Soloing A solos B. The Groups window lists the group, chosen, Volume on and Pan off; ticking Pan and Send 2 sets them; renaming to "Drums" and ticking H hide both rows; Ctrl+Shift+1 shows them; Ctrl+Shift+2 fails (no group 2); Shift+G turns groups off (the window's box clears and mute stops linking) and on; Alt+click selects the members; the command closes the window. Create Group for Selected Tracks puts the selected track in group 2. New VCA from A's VCA slot makes "VCA 1" with A on it in one undo step ("New VCA") and a strip reading "1 track"; its fader, Mute and name change the VCA; Shift+click on the inspector's VCA slot shows it in place of the output strip; Delete VCA frees A and removes the strip.

**AT-MIX-23** [E] · D-W6-17 · A track playing a 0.5 constant in a folder stack whose main is at -6.02 dB plays 0.25; the main's mute gives silence. Flattened and put in a summing stack at -6.02 dB, it plays 0.25.

**AT-MIX-24** [UI] · D-W6-17 · Create Folder Stack from B selects a folder main above it with a disclosure triangle (B has none). The triangle closes it: B's header and strip go, and the main's strip has the disclosure button and no output or VCA slot. Follow Track Stacks off shows B's strip, the main before B with the tracks. Create Summing Stack from C routes C to it; closing it, then Alt on the folder's disclosure, opens both. Open/Close Track Stack from B closes the folder and selects its main. B's inspector shows the folder main on the right; soloing the main solos B; Flatten Stack on the summing main sends C to the master; Flatten Stack on A fails.

## 10. Plugins (PLG)

**AT-PLG-1** [HW] · F-PLG-1, F-PLG-4 · On Windows, a set of at least 5 widely used free VST3 effects and 2 free VST3 instruments (list fixed in Phase 2) load, process audio, open their editors in floating windows, and close without error. **Decision D-AT-4** fixes the list.

**Decision D-AT-4 (VST3 list):** effects: TDR Nova (dynamic EQ), Valhalla Supermassive (reverb/delay), MeldaProduction MCompressor (from the free bundle), Airwindows Consolidated, and Voxengo SPAN (analyser); instruments: Surge XT and Dexed. All are free, 64-bit VST3 and widely used; the latest release on the day of the pass is the one tested. A plug-in that stops being free or available is replaced by another free one of the same kind and the swap is noted in the pass record.

**AT-PLG-2** [E] · F-PLG-1 · `TestGain` loaded via the VST3 host applies its gain parameter; `TestSynth` responds to MIDI; parameter changes from the host arrive with correct sample offsets.

**AT-PLG-3** [E] · F-PLG-2 · Scanning a folder containing `TestGain`, `TestSynth`, and `TestCrashScan` lists the first two and marks `TestCrashScan` as failed without crashing the host; a rescan skips the failed plugin unless "Retry failed" is chosen.

**AT-PLG-4** [UI] · F-PLG-3 · The plugin manager lists plugins with name, vendor, format, status; disabling one removes it from insert menus; Rescan picks up a newly added plugin.

**AT-PLG-5** [UI] · F-PLG-4 · A plugin with no editor (fixture) shows a generic UI of sliders for each parameter, which change the parameter.

**AT-PLG-6** [E] · F-PLG-5 · Setting `TestGain` to +3.5 dB, saving, and reloading restores +3.5 dB (state chunk round trip); state over 64 KB is stored as a blob and restored.

**AT-PLG-7** [E] · F-PLG-9 · Weft Keys: each preset produces non-silent output for MIDI 60; 16-voice polyphony (17th note steals the oldest); output never exceeds 0 dBFS for 8 simultaneous notes at velocity 127.

**AT-PLG-8** [E] · F-PLG-10 · Built-in EQ: a +6 dB peak at 1 kHz Q 1 raises a 1 kHz sine by 6 ± 0.2 dB and a 100 Hz sine by < 0.5 dB; HPF at 100 Hz attenuates 50 Hz by ≥ 9 dB (12 dB/oct). Compressor: ratio 4:1, threshold −20 dB, on a steady −8 dBFS sine gives −17 ± 0.5 dBFS after settling. Delay: 500 ms delay produces an echo at exactly 24,000 samples at 48 kHz. Reverb: impulse response decays below −60 dB within the set decay time ± 20%. Gain utility: +6.02 dB doubles amplitude.

**AT-PLG-9** [E] · F-PLG-10 · All built-in effects report 0 latency (or their actual latency) and produce no NaN/Inf/denormal slowdowns when fed silence for 60 s followed by full-scale noise.

**AT-PLG-10** [E] · F-PLG-14 · An automation lane for `TestGain`'s gain parameter changes the plugin's output over time per the lane values (see AT-AUT-6).

**AT-PLG-11** [E] · F-PLG-15 · Opening a project referencing a VST3 class ID not installed creates a placeholder that passes audio unchanged, shows the plugin's name, and on save writes the original state bytes unchanged (byte-identical).

**AT-PLG-12** [UI] · F-PLG-4, D-UI-24 · An insert effect's window has a Bypass button in a strip above the editor: pressing it bypasses the insert and lights the button; bypassing from the mixer slot's menu puts the button out again.

**AT-PLG-13** [UI] · F-PLG-4, D-W6-11 · With no track selected Ctrl+Shift+P only reports that a track is needed. On an audio track an empty search lists every effect and no instrument; " dEL " finds only Weft Delay; "weft del" finds it too; "zzz" finds nothing. Searching "eq" and pressing Enter adds Weft EQ to the track's first insert slot. On an instrument track the list also has the instruments, and choosing one sets the track's instrument; an instrument can't be added to an audio track. The Mix menu lists Search and Add Plug-in....

**AT-PLG-14** [UI] · D-W6-12 · On Weft EQ, before anything is saved the window shows "Settings" with Compare and Paste off, and its slider list can be widened. Saving at 0.8 as "Bright" (the name asked for) shows "Bright"; changing to 0.5 shows "Bright *" and turns Compare on; Compare loads 0.8, a second press gives 0.5 back. With "Air" saved at 0.5 the list is Air, Bright; > from an unsaved change loads Bright (one undo step "Load Setting"), > again wraps to Air, < goes back to Bright, and Undo returns to 0.5. The Settings menu lists its commands, then Air and Bright with Bright ticked. Copy on one EQ and Paste on another EQ gives the second the value ("Paste Setting"). Reset Setting returns the parameter to its default ("Reset Setting") and clears the name. Save Setting as Default at 0.7: a newly inserted Weft EQ starts at 0.7, and Recall Default Setting brings 0.7 back. Deleting the loaded "Bright" leaves only Air. V hides every plug-in window, V again shows them; with none open it does nothing.

## 11. Automation (AUT)

**AT-AUT-1** [UI] · F-AUT-1 · A toggles automation view; each track header shows parameter (default Volume) and mode (default Read) selectors; "+" adds another lane.

**AT-AUT-2** [U] · F-AUT-2, F-AUT-11 · The parameter selector lists Volume, Pan, Mute, each send's level, and every parameter of every plugin on the track; bus and master tracks offer the same.

**AT-AUT-3** [U][UI] · F-AUT-3 · Clicking the lane adds a point at the snapped time and clicked value; dragging moves it; Shift-drag constrains to one axis; rubber-band + Delete removes points.

**AT-AUT-4** [E] · F-AUT-3, F-AUT-4 · Volume lane points (bar 1, 0 dB) and (bar 2, −inf normalized 0) with linear curve: the applied normalized value at bar 1.5 is the midpoint; before bar 1 holds the first value; after bar 2 holds the last. With curve +0.5 the midpoint value differs from linear and the segment remains monotonic.

**AT-AUT-5** [E] · F-AUT-2 · Pan automation from −1 to +1 over one bar moves a mono sine from L to R; mute automation silences between its on/off points; send-level automation changes the bus input level.

**AT-AUT-6** [E] · F-AUT-2, F-PLG-14 · Automating `TestGain` from 0 to +6 dB at a point mid-buffer applies the change at the correct sample offset (±1 block for parameters without sample-accurate support, exact for VST3 parameter queues).

**AT-AUT-7** [E] · F-AUT-5 · Mode Off: automation ignored, static value used. Read: values follow the lane; moving the fader during playback snaps back on release and writes nothing.

**AT-AUT-8** [E] · F-AUT-5, F-AUT-6 · Touch: scripted fader touch from bar 2 to bar 3 writes points in that range only; after release, the value glides back to existing automation within 200 ms.

**AT-AUT-9** [E] · F-AUT-5, F-AUT-6 · Latch: touch at bar 2, release at bar 3, stop at bar 5: points written from bar 2 to bar 5 holding the release value after bar 3.

**AT-AUT-10** [E] · F-AUT-5, F-AUT-6 · Write: playing bar 1–4 replaces all existing points in bar 1–4 with the fader's values; after stop the mode is Latch; the pass is one undo step.

**AT-AUT-11** [U] · F-AUT-6 · Writing a constant value for 4 bars produces at most 2 points (thinning, D-W8-2); writing a slow ramp produces points no more often than every 50 ms.

**AT-AUT-12** [E] · F-AUT-11 · Master volume automation affects the bounce; bus pan automation affects the bus output.

**AT-AUT-13** [E] · F-AUT-11, D-W8-6 · A project with steep volume, pan, send and master volume automation (a 0 dB to −inf fade over 5 ms) bounces identically at block sizes 64, 512 and 2048, and the bounce matches live playback at a 240-sample device buffer to within 1e-6.

**AT-AUT-14** [UI] · D-W8-7 · With two audio tracks: Set All Tracks to Automation Touch sets both ("Automation Mode"), then Latch, Read and Off; Off again does nothing. On the selected track Off/Read goes Read then Off, Latch/Read goes Read then Latch, and the other track stays Off. Choosing Touch in the second header with Alt held sets both to Touch. With two lanes shown, × on the first leaves Pan; closing that leaves Volume. With no automation Cycle Through Used Parameters does nothing; with volume and pan points it shows Pan, then Volume. With two volume points selected Up adds 0.01 to each and Shift+Down takes 0.1 from each, and the selected track doesn't change.

## 12. Tempo and signature (TMP)

**AT-TMP-1** [U] · F-TMP-1 · Tempo map 120 BPM at tick 0, 60 BPM at tick 3,840: `sample(3,840)` = 96,000; `sample(4,800)` = 96,000 + 960 × 50 = 144,000; the inverse maps back exactly.

**AT-TMP-2** [E] · F-TMP-1 · With the map above, metronome clicks occur at 0, 24k, 48k, 72k, 96k, 144k, 192k samples; a MIDI note at tick 4,800 plays at sample 144,000; an audio region starting at tick 4,800 starts at sample 144,000 and plays at its own speed (D-TMP-1).

**AT-TMP-3** [U][UI] · F-TMP-1, F-TMP-4 · Ctrl+click in the tempo lane at bar 5 adds a point; dragging it vertically changes BPM in 0.01 steps; the initial point can't be deleted or moved.

**AT-TMP-4** [U] · F-TMP-3 · Signature 4/4 then 3/4 at bar index 2 then 6/8 at bar index 4: bar starts (ticks) = 0, 3,840, 7,680, 10,560, 13,440, 16,320 (6/8 bar = 2,880).

**AT-TMP-5** [U] · F-TMP-3 · Inserting a signature change does not change any region's tick position (D-DM-10).

**AT-TMP-6** [UI] · F-TMP-4 · G toggles the global tracks; marker, tempo, and signature lanes share the timeline zoom/scroll with the tracks area.

**AT-TMP-7** [S] · F-TMP-1 · Two clients concurrently add tempo points at the same tick with different BPM: both converge to the point with the greater Id, and a conflict badge is shown on both.

**AT-TMP-8** [UI] · F-TMP-1, L-TMP-3 · Alt-dragging a tempo change at bar 5 to bar 7 leaves bar 5 as it was and adds a change at bar 7 with the same BPM; a plain click without moving sideways adds nothing; undo removes the copy in one step.

**AT-TMP-9** [UI] · L-TMP-9, D-W11-1 · Five Tap Tempo presses 500 ms apart set 120 BPM, then four 400 ms apart set 150 BPM as one more run; each run is one undo step; a single tap changes nothing.

**AT-TMP-11** [U] · D-UI-44 · With the project in D major, a key change can't go at the start; G major from bar 9 is one undo step "Add Key Change" and a second change at the same place is refused; the key is D major before bar 9 and G major from it. Editing the key at bar 11 edits that change, and at bar 2 the project key. Moving the change to bar 5 is "Key Change"; it can't land on another change. Key changes survive saving and reopening; Delete Key Change removes one and undo restores it.

**AT-TMP-12** [E] · D-UI-44 · A MIDI region from bar 3 to bar 7, with G major from bar 2, A minor from bar 4 and C major from bar 9, exports with two key events: G major at its start and A minor one bar in.

**AT-TMP-10** [UI] · F-TMP-1, D-UI-30 · Dragging the tempo change at bar 5 two bars right and 20 px up with Shift held moves it to bar 7 and keeps its tempo; dragging it mostly upward with Shift changes its tempo and keeps it at bar 5; without Shift both change.

## 13. Bounce and export (EXP)

**AT-EXP-1** [E] · F-EXP-1 · Bouncing 4 bars of P0 with one region to WAV 24-bit 48 kHz gives a file of 384,000 samples (tail off) whose samples equal the master output from a real-time capture (± 1 LSB).

**AT-EXP-2** [E] · F-EXP-1 · WAV 16-bit, 24-bit, 32-float and sample rates 44.1/48/96 kHz each produce valid files readable by a reference decoder (libsndfile) with the right header; 44.1 kHz output from a 48 kHz project has duration equal within 1 sample.

**Decision D-AT-2 (reference decoders):** libsndfile isn't one of Weft's dependencies, so AT-EXP-2 reads WAVs back with JUCE's WAV reader and AT-EXP-3/4 decode MP3s with Symphonia (a Rust decoder that honours the LAME gapless header). Neither shares code with Weft's writers.

**AT-EXP-3** [E] · F-EXP-2 · MP3 320 kbps CBR of a 10 s sine is decodable by a reference decoder, duration 10 s ± 1 frame (after encoder delay compensation/LAME gapless header), spectral peak at the sine frequency, and ID3 Title/Artist tags match the dialog.

**AT-EXP-4** [E] · F-EXP-2 · VBR quality 2 produces a valid MP3 with a Xing/LAME header.

**AT-EXP-5** [E] · F-EXP-3 · Offline bounce of a 3-minute project with built-in plugins completes in less than 3 minutes on CI hardware and is bit-identical across two runs (D-W9-2).

**AT-EXP-6** [E] · F-EXP-3 · A plugin flagged as realtime-only forces real-time mode with a notice.

**AT-EXP-7** [U] · F-EXP-4 · Default range: cycle on → cycle range; cycle off with end marker → 0 to end marker; no end marker → 0 to end of last region.

**AT-EXP-8** [E] · F-EXP-5 · 16-bit bounce with dither Auto adds TPDF noise (RMS within expected bounds, ~−101 dBFS) to digital silence-plus-tone; 24-bit Auto adds none (bit-exact with the truncated float mix); dither Off at 16-bit produces truncation only.

**AT-EXP-9** [E] · F-EXP-11 · Region ending at bar 4 with a 2 s reverb tail, include tail on: the file extends past bar 4 until the output stays below −90 dBFS for 200 ms, and no longer than 10 s past the range end; with include tail off, it ends exactly at bar 4.

**AT-EXP-10** [E] · F-EXP-1 · A mix peaking at +2 dBFS reports "Bounce clipped (peak +2.0 dBFS)"; 32-float WAV keeps samples > 1.0; 24-bit clips at full scale.

**AT-EXP-11** [UI] · F-EXP-1 · Bouncing to an existing filename asks Replace / Keep both; Keep both writes "name (2).wav".

**AT-EXP-12** [E][UI] · L-EXP-2 · Export Regions as Files (File menu and the region menu) writes a selected audio region to `<region name>.wav` in the chosen folder, at the project rate and record bit depth, as it plays with its gain and fades but without the track strip; characters not allowed in file names become `_`; an existing name gets " (2)"; nothing selected says "Select regions to export."

**AT-EXP-13** [E][U] · L-EXP-3 · A MIDI region exports to `<region name>.mid`, type 1 at 960 PPQN, starting at the region start: notes as they play (transpose and velocity offset applied, muted notes left out, cut at the region end), controllers, and the tempo and meter at the region start plus tempo changes inside it. Files Weft writes read back identically.

**AT-EXP-14** [E] · D-W9-7, D-W9-8 · Normalize On brings a −12 dBFS sine to a 0 dBFS peak and the notice says "Normalized by +12.0 dB."; Overload Protection Only leaves it alone but brings a +2 dBFS mix down to 0 dBFS without a clip warning. Mono writes one channel of (L + R) / 2; Split writes `name.L.wav` and `name.R.wav` equal to the stereo file's sides.

**AT-EXP-15** [E] · D-W9-11 · With a 50 ms burst at the end of a 1-bar range into a 250 ms delay, the plain bounce is silent 150–300 ms in and the 2nd cycle pass bounce has the echo there, at the same length. A real-time-only plug-in makes Automatic real time; Offline stays offline and says so; a real-time bounce drops the second pass with a note.

**AT-EXP-16** [E] · D-W9-8 · AIFF and FLAC bounces at 16, 24 and 32 bits read back (through the platform decoders) as stereo, the range's length, at 16, 24 and 24 bits; with no encoder the bounce fails saying it can't write the format. An MP3 bounce of a 96 BPM project has a TBPM frame of "96".

**AT-EXP-17** [E] · D-W9-9 · Exporting one of two tone tracks renders only its tone, offline even when real time was asked for; the mix has both.

**AT-EXP-18** [E] · D-W9-10 · Three MIDI regions on two tracks export as one file with a track per project track in track order, named after them, notes timed from the earliest region, the tempo at that start; it parses back; nothing MIDI selected gives no tracks.

**AT-EXP-19** [UI] · D-W9-7 to D-W9-11 · The File menu lists the three exports. The dialog maps Mode, Normalize, Channels, AIFF, FLAC and the 2nd cycle pass onto the settings, turns the tail off with the second pass and the second pass off in real time; New Folder makes a folder inside the current one and selects it. Export Track refuses the master and takes the selected track; Export All Tracks lists the unmuted audio and instrument tracks, names each file after its track, and one bounce run writes them all ("Exported N files."). Export Selection as MIDI File needs MIDI regions selected and writes a file.

## 14. Library (LIB)

**AT-LIB-1** [UI] · F-LIB-1 · The file browser lists folders and supported audio/MIDI files; clicking an audio file previews it; dragging it into the arrange view behaves as AT-ARR-25.

**AT-LIB-3** [UI] · D-UI-33 · In the file browser, after showing a folder and then its subfolder, Back returns to the folder and Forward to the subfolder; going somewhere else after Back leaves nothing to go forward to. Home shows the home folder and Project the project's folder. Searching "TON" lists tone.wav and the folders but not beat.mid; "tone mid" lists no files; clearing it brings beat.mid back. With the volume at -12 dB, Play on the selected tone.wav (a 0.5 peak sine) previews it at a peak of 0.126 and the button reads Stop; Stop ends the preview.

**AT-LIB-4** [UI] · D-UI-40 · In the Media tab, Add Audio File with kick.wav and pad.wav adds both, no regions, as one undo step ("Add Audio File"), and selects them. Add to New Tracks with kick selected and the playhead at bar 2 makes a track "kick" with its region at bar 2 ("Add to Tracks"); with that track selected and the playhead at bar 5, Add Region for both files places them one after another there ("Add Region"). Show All Regions lists the three regions under their files; clicking pad's region selects it; the triangle closes kick. The path line shows the selected file. Play previews pad and reads Stop; Play again stops. Rename makes kick "Kick 1" ("Rename Audio File"). Delete File on pad removes it and its region as one undo step ("Delete File"), and undo brings both back. Select Unused selects nothing and says "No unused audio files."; once pad's region is gone it selects pad, and Delete removes it; a file regions still play can't be removed on its own.

**AT-LIB-2** [UI][U] · F-LIB-2 · The project media browser lists every media hash referenced with original name, duration, and usage count; deleting all regions using a file shows count 0.

## 15. UI and preferences (UI)

**AT-UI-1** [UI] · F-UI-1 · Toggling Inspector, Side panel, Mixer, Editor, Global tracks shows/hides each panel; the tracks area always remains visible; panel layout persists after restart.

**AT-UI-2** [UI] · F-UI-3 · Switching theme changes all panels without restart; every text/background token pair meets WCAG AA contrast (4.5:1) in both themes (checked from the token table).

**AT-UI-3** [UI][HW] · F-UI-4 · At 100%, 150%, 200%, and 250% Windows scaling, text and waveforms render crisply (no bitmap scaling) and hit targets scale; moving the window between monitors with different scaling re-renders at the new scale.

**AT-UI-4** [HW] · F-UI-7 · On Windows: WASAPI shared, WASAPI exclusive, and ASIO (with ASIO4ALL or a hardware driver) can each be selected; buffer sizes offered match the driver; the reported latency is shown; changing the device while playing stops transport, reopens, and resumes cleanly.

**AT-UI-5** [E] · F-UI-7 · With the null device, changing buffer size 64 ↔ 1024 and sample rate keeps engine output correct (AT-TRN-1 still passes).

**AT-UI-6** [HW][E] · F-UI-8 · Disabling a MIDI input in preferences stops its notes reaching tracks; enabling restores; hot-plugging a USB MIDI device makes it appear without restart (HW).

**AT-UI-7** [UI] · F-UI-10 · Every toolbar, transport, mixer, and header control has a tooltip with its name and (if any) shortcut; a test walks the accessibility tree and fails on controls without names.

**AT-UI-8** [UI] · F-UI-14 · First launch (no settings file) shows Audio Setup; Test plays a tone (captured on the null device: 1 kHz at −18 dBFS for 1 s); Continue saves settings; second launch skips the dialog. With no openable device, the app opens in No-audio mode with a banner.

**AT-UI-9** [UI] · F-UI-1 · Selecting a track shows, in the inspector, its channel strip beside the strip of its output (Master, or the bus it outputs to); with no track selected neither shows. Volume, pan, mute, inserts and sends changed in the inspector change the mixer's strip and the reverse, and undo reverts either. The EQ thumbnail is flat with no EQ; clicking it adds a Weft EQ and opens it; its curve peaks at +12 dB at 1 kHz for a +12 dB peak band there, and is flat when the EQ is bypassed. The inspector's and the mixer's meters both see a channel's peak (D-UI-16).

**AT-UI-10** [UI] · F-UI-1, D-UI-22 · With two MIDI regions selected whose transpose differs (0 and +5) the inspector's name reads "2 regions", Transpose shows "—" and Velocity shows its value; setting Transpose to −12 sets both and undoes in one step; Mute mutes both. With an audio region and a MIDI region selected, the gain and transpose controls are hidden and setting Delay to 20 ms sets both.

**AT-UI-11** [UI] · F-TRN-8, D-UI-21 · In a 1400 px window the control bar shows the More button; its menu lists the three count-in choices and a CPU line, and choosing "2-bar count-in" sets the count-in to 2 bars. At 2200 px the More button is hidden. Navigate › Count-in lists Off, 1 Bar and 2 Bars; Shift+K turns a 2-bar count-in off and back on at 2 bars.

**AT-UI-12** [UI] · F-UI-10, D-UI-29 · The Tool menu shows each tool's key (T, P, I, J, E, M, and N, Z, A, G, Q since D-UI-36); with it open, P picks the Pencil and M the Mute tool; a key that isn't a tool's (W) picks nothing; with Ctrl held nothing is picked.

**AT-UI-13** [UI] · D-UI-41 · The Settings window opens with Welcome, 10 recent, software monitoring on, Dark and C3. Middle C as C4 names note 60 "C4" at once and saves the settings; C3 again names it "C3". Light makes the theme light; View › Dark Theme then sets the appearance to Dark. Software monitoring off and Monitor only the selected track reach the engine; 5 recent limits Open Recent; Wide playhead and the start-up choice apply and save; Help tags off turns tooltips off. The saved settings read back unchanged. A dropout puts "The audio engine couldn't keep up…" in the status bar, and nothing once that's turned off. File › Settings... has Ctrl+,.

**AT-UI-14** [UI] · D-UI-43 · The control bar's customize menu lists every item ticked, the master choices, the time and tempo formats, the big displays, Apply Defaults, Save As Default and Revert. Unticking Tempo and then Key hides them and Revert shows Key again; after Save As Default, hiding the CPU reading and choosing Apply Defaults shows it again. The master volume slider sets the master's fader to -6 dB; Output Meter replaces it and None hides both. At 4.5 s the time reads 216000 as samples and 00:00:04:12 at 25 fps, and the tempo reads 120 without decimals. View › Control Bar hides the bar; View › Toolbar shows the eight default buttons under it, adding Split at Cycle puts it second and Restore Defaults removes it; the layout survives a restart. Two big beats displays and a big time display open as three windows showing the control bar's position and time, and the performance meter opens at twice its size.

**AT-UI-15** [UI] · D-UI-46 · Recall Screenset 2, hide the inspector, Screenset 1 brings it back and 2 hides it again; Screenset 1x then 3 gives screenset 13 without moving to a marker; the screensets survive being written out and read back. After two renames, Undo History lists "Start" first and "Rename Track" last, current; clicking two rows up undoes both (the steps stay listed) and clicking the last redoes them; asking again doesn't open a second window. Open Mixer, Open Piano Roll and Region Inspector Float open windows holding those views; Close Window closes the inspector's. Cycle Through Window Views goes from none to Mixer, and backwards twice to Audio Editor. Close Project gives a new project with one track.

**AT-UI-16** [UI] · D-UI-47 · View › Notes opens the side panel on the Notes tab showing "Audio 1"; both notes are read-only until Edit, and Done saves them (the last undo step is "Track Notes"). Selecting another track while editing saves what was typed for the first and shows the second's empty notes. Text Size large makes the text bigger. The mixer's Notes button shows the track's notes on its strip (not on the master's); Track Notes in the Mixer hides them again. Notes again closes the side panel.

**AT-UI-17** [UI] · D-UI-48 · List Editors opens the Lists tab on the Event list, which lists the one region (at 2.1.1.0, 2.0.0.0 long) while none is selected; double-clicking it lists its note (Note, C3, 100, 0.0.1.0) and controller (2.3.1.0, Control, 1, 64). Typing D3, 90, 0 1 0 0 and 2 2 1 0 changes the note; 200, or a position before the region, is refused. Hiding Notes leaves the controller. + adds a note at 3.1.1.0 and a pitch bend; deleting both is one undo step. Clicking a note selects it in the piano roll; Display Level shows the regions. The Marker list adds a marker at 3.1.1.0, renames it Chorus and moves it to 5 1 1 0; clicking it moves the playhead there; its text is saved when the text area is hidden. The Tempo list adds a change at bar 5 at 120 and types 90 (bar 1 stays 120); 5000 is refused. The Signature list shows 4/4 and the project key; it adds 3/4 at bar 3 and an F# minor key change there and sets the project key to G. Open Event List twice gives one window; Open Tempo List shows the Tempo list; Open Marker Text shows the marker's text.

## 16. Collaboration (COL)

**AT-COL-1** [U] · F-COL-1 · Every user edit in AT-ARR-19's list produces exactly one CRDT change; replaying the change log from empty reproduces the document.

**AT-COL-2** [S] · F-COL-1 · Property test: three clients each apply 1,000 random edits from the full edit vocabulary with random network partitions and delivery orders; after all changes are exchanged, all three documents (after read-time repair) are identical and pass all invariants in 03.

**AT-COL-3** [S] · F-COL-2 · Sign up, sign in, wrong password rejected; 6th failed attempt in a minute is rate-limited; access token expires after 15 min and is refreshed transparently.

**AT-COL-4** [S] · F-COL-3 · Owner shares a local project: document and media upload; invite link with role Editor lets user B join and edit; Viewer C can read, comment, and chat, and the relay rejects C's change to a region with `forbidden`; Owner removes B and B's socket closes with a read-only banner.

**AT-COL-5** [S] · F-COL-4 · On a localhost relay with 50 ms injected RTT, a region move on client A appears on client B within 500 ms in 95% of 200 trials.

**AT-COL-6** [S] · F-COL-4 · A fader drag of 2 s on client A emits ≤ 41 changes (20/s + final) and B's final value equals A's final value.

**AT-COL-7** [S] · F-COL-5 · Client A goes offline, makes 50 edits; B makes 50 edits online; A reconnects; both converge; A's status goes "Offline – 50 changes pending" → "Synced".

**AT-COL-8** [S] · F-COL-5 · Concurrent edit cases from 05 §4 table, each as its own test: different regions moved (both apply); same region moved by both (converge to one); move-track vs. move-time (both apply); delete vs. edit (deleted, toast on editor); different notes added (all kept); same note pitch vs. velocity (both apply); plugin inserted at same slot (both kept, same order on both clients); delete track vs. record onto it (Recovered track, one only).

**AT-COL-9** [S] · F-COL-5 · Killing client A's process with un-ACKed changes and restarting re-sends them; relay restart loses no ACKed change.

**AT-COL-10** [S][UI] · F-COL-6 · When B joins, A's toolbar shows B's avatar within 1 s; when B disconnects, it disappears within 45 s; after 2 min without input, B shows as idle.

**AT-COL-11** [S][UI] · F-COL-7 · B's pointer over the arrange view appears on A at the same bar/track regardless of A's zoom; pointer updates ≤ 20 Hz; B's playhead appears on A while B plays and its extrapolated position is within 50 ms of B's actual position.

**AT-COL-12** [S][UI] · F-COL-8 · B selects two regions: A sees them outlined in B's color; B focuses track 3: A sees B's badge on track 3's header; B records on track 3: A sees the red recording badge.

**AT-COL-13** [U] · F-COL-9 · Importing the same WAV on two clients yields the same `b3:` hash; hash matches the BLAKE3 reference vector for a fixture file.

**AT-COL-14** [S] · F-COL-9 · A records a region: the document change arrives at B before the upload completes; B's region shows "downloading"; after upload, B downloads, verifies the hash, and playback includes the audio. A second project of the same owner referencing the same hash triggers no re-upload (HEAD hit).

**AT-COL-15** [S] · F-COL-9 · An interrupted upload resumes from the last completed 8 MB chunk; a download whose bytes don't match the hash is discarded and retried once, then reported.

**AT-COL-16** [S] · F-COL-9 · User D, not a member of the project, requesting a blob by hash gets 403 even with a valid token.

**AT-COL-17** [S][E] · F-COL-10 · While a blob is half downloaded, playback plays the downloaded prefix and is silent for the rest of the region; the region shows progress; the status bar shows transfer counts. Uploads pause while recording.

**AT-COL-18** [E] · F-COL-10 · Bouncing with undownloaded media shows "N regions have media not yet downloaded" with Wait / Bounce anyway.

**AT-COL-19** [S] · F-COL-11 · Locking is off by default: two clients can edit the same track concurrently with no prompts.

**AT-COL-20** [S][UI] · F-COL-11, F-TRK-16 · A locks track 1: B's UI disables editing of track 1's fields, mute, strip, regions (including dragging regions onto it), automation, and take folders, with tooltip "Locked by A"; B can still solo track 1 locally.

**AT-COL-21** [S] · F-COL-11 · B's offline edits to track 1 made before seeing A's lock are merged on reconnect, and A gets a notice.

**AT-COL-22** [S] · F-COL-11 · The Owner breaks A's lock after confirmation and A receives a notice; a lock expires 24 h after its holder's last presence (clock mocked).

**AT-COL-23** [U] · F-COL-11 · Lock state round-trips through save/load and syncs as a document field.

**AT-COL-24** [S][UI] · F-COL-12 · A adds a comment at bar 9 on track 2; B sees the pin and the thread in the Comments tab; B replies; A resolves; filters Open/Resolved/Mine/Mentions show correct subsets; deleting track 2 leaves the comment at bar 9 on the timeline.

**AT-COL-25** [S] · F-COL-12 · A comment mentioning @B creates a notification for B on next connect; only the author can edit their comment.

**AT-COL-26** [S] · F-COL-13 · Markers created, renamed, moved, and deleted on A appear on B; concurrent renames converge.

**AT-COL-27** [S][UI] · F-COL-14 · Chat messages from A and B appear on both in the same order; messages sent offline appear after reconnect; "bar 17" in a message is a link that moves the reader's playhead to bar 17; messages over 4,000 characters are refused.

**AT-COL-28** [S] · F-COL-15 · Save Version "Mix 1" creates a tag; further edits; previewing "Mix 1" renders the old state read-only; Restore creates a new change that makes the current document equal to "Mix 1" (content comparison) on all clients, adds "Restored 'Mix 1'", and undo of the restore returns to the pre-restore state.

**AT-COL-29** [S] · F-COL-15 · An automatic version is created when a user with changes closes the project, and none when they made no changes.

**AT-COL-30** [S] · F-COL-16 · A moves region R (bar 1 → 3); B then changes R's gain; A undoes: R returns to bar 1 and B's gain change remains. A moves R to bar 3; B moves R to bar 5; A undoes: R stays at bar 5 (field no longer holds A's value, D-COL-6). A edits R; B deletes R; A undoes: nothing happens and the status bar explains why.

**AT-COL-31** [S] · F-COL-20 · Client B lacking a plugin that A inserted sees a placeholder, hears passthrough, gets a Missing Plugins notice listing it and who added it; B moving the track's fader does not alter the plugin's stored state (byte-identical on A).

## 17. System (SYS)

**AT-SYS-1** [UI][U] · F-SYS-1 · Help › About shows the version and lists every third-party component with its license text; a build check fails if any linked dependency is missing from the license manifest.

## 18. Real-time safety and engine robustness (cross-cutting)

These protect non-functional requirements stated in the brief and apply to all audio features.

**AT-RT-1** [E] · all audio features · Running the engine with an instrumented allocator and lock detector on the audio thread, through a scripted session (play, record, edit regions during playback, add/remove plugins, receive remote changes, automate), records zero allocations, zero lock acquisitions, and zero file I/O calls on the audio thread.

**AT-RT-2** [E] · all audio features · 10,000 random document edits applied during playback produce no glitches (no buffer misses in the null device's deadline simulation) and no crashes.

**AT-RT-3** [E] · F-TRN-1 · The engine handles buffer sizes 32 to 2,048 and odd sizes (e.g. 441) with correct output.

---

## Coverage table (every MVP feature → tests)

| Feature | Tests | | Feature | Tests |
|---|---|---|---|---|
| F-PRJ-1 | AT-PRJ-1, 2 | | F-MIX-1 | AT-MIX-1 |
| F-TRN-20 | AT-TRN-19 | | F-REC-17 | AT-REC-20 |
| F-ARR-28 | AT-ARR-27, 37 | | F-ARR-29 | AT-ARR-28 |
| F-PRJ-3 | AT-PRJ-3, 4, 5 | | F-MIX-2 | AT-MIX-2 |
| F-PRJ-4 | AT-PRJ-6, 7 | | F-MIX-3 | AT-MIX-3 |
| F-PRJ-5 | AT-PRJ-3, 8, 9 | | F-MIX-4 | AT-TRK-8 |
| F-PRJ-6 | AT-PRJ-10 | | F-MIX-5 | AT-MIX-4 |
| F-PRJ-7 | AT-PRJ-11, 12 | | F-MIX-6 | AT-MIX-5 |
| F-PRJ-9 | AT-PRJ-13, 16–20 | | F-MIX-7 | AT-MIX-6 |
| F-TRN-1 | AT-TRN-1, 2, 3, AT-RT-3 | | F-MIX-8 | AT-MIX-7 |
| F-TRN-2 | AT-TRN-4 | | F-MIX-9 | AT-MIX-8 |
| F-TRN-4 | AT-TRN-5 | | F-MIX-10 | AT-MIX-9, 10 |
| F-TRN-5 | AT-TRN-6 | | F-MIX-11 | AT-MIX-11 |
| F-TRN-6 | AT-TRN-7, 8 | | F-MIX-12 | AT-TRK-13 |
| F-TRN-7 | AT-TRN-9, 10 | | F-MIX-13 | AT-MIX-12, 13 |
| F-TRN-8 | AT-TRN-11, 12 | | F-PLG-1 | AT-PLG-1, 2 |
| F-TRN-9 | AT-TRN-13 | | F-PLG-2 | AT-PLG-3 |
| F-TRN-10 | AT-TRN-14 | | F-PLG-3 | AT-PLG-4 |
| F-TRN-11 | AT-TRN-15, 16 | | F-PLG-4 | AT-PLG-1, 5 |
| F-TRN-16 | AT-TRN-17 | | F-PLG-5 | AT-PLG-6 |
| F-TRN-17 | AT-TRN-18 | | F-PLG-9 | AT-TRK-2, AT-PLG-7 |
| F-REC-1 | AT-REC-1, 2, 3, 4 | | F-PLG-10 | AT-PLG-8, 9 |
| F-REC-2 | AT-REC-5, 6 | | F-PLG-14 | AT-PLG-10, AT-AUT-6 |
| F-REC-3 | AT-REC-7, 8, 9 | | F-PLG-15 | AT-PLG-11 |
| F-REC-4 | AT-REC-10 | | F-AUT-1 | AT-AUT-1 |
| F-REC-5 | AT-REC-11, 12, 13 | | F-AUT-2 | AT-AUT-2, 5, 6 |
| F-REC-7 | AT-REC-14, 15 | | F-AUT-3 | AT-AUT-3, 4 |
| F-REC-8 | AT-REC-16 | | F-AUT-4 | AT-AUT-4 |
| F-REC-9 | AT-REC-2 | | F-AUT-5 | AT-AUT-7, 8, 9, 10 |
| F-REC-14 | AT-REC-17 | | F-AUT-6 | AT-AUT-8, 9, 10, 11 |
| F-REC-15 | AT-REC-1, 18 | | F-AUT-11 | AT-AUT-2, 12 |
| F-REC-16 | AT-REC-19 | | F-TMP-1 | AT-TMP-1, 2, 3, 7, 10 |
| F-TRK-1 | AT-TRK-1 | | F-TMP-3 | AT-TMP-4, 5 |
| F-TRK-2 | AT-TRK-2 | | F-TMP-4 | AT-TMP-3, 6 |
| | | | F-TMP-10 | AT-PRJ-15 |
| F-TRK-4 | AT-TRK-3 | | F-EXP-1 | AT-EXP-1, 2, 10, 11 |
| F-TRK-6 | AT-TRK-4 | | F-EXP-2 | AT-EXP-3, 4 |
| F-TRK-7 | AT-TRK-5 | | F-EXP-3 | AT-EXP-5, 6 |
| F-TRK-8 | AT-TRK-6 | | F-EXP-4 | AT-EXP-7 |
| F-TRK-9 | AT-TRK-7 | | F-EXP-5 | AT-EXP-8 |
| F-TRK-10 | AT-TRK-8, 9, 19, AT-MIX-16 | | F-EXP-11 | AT-EXP-9 |
| F-TRK-11 | AT-TRK-10 | | F-LIB-1 | AT-LIB-1, 3 |
| F-TRK-12 | AT-TRK-11 | | F-LIB-2 | AT-LIB-2 |
| F-TRK-16 | AT-COL-20 to 23 | | F-UI-1 | AT-UI-1, 9 |
| F-TRK-17 | AT-TRK-13 | | F-UI-3 | AT-UI-2 |
| F-ARR-1 | AT-ARR-1 | | F-UI-4 | AT-UI-3 |
| F-ARR-2 | AT-ARR-2, 3 | | F-UI-7 | AT-UI-4, 5 |
| F-ARR-3 | AT-ARR-4 | | F-UI-8 | AT-UI-6 |
| F-ARR-4 | AT-ARR-5, 6 | | F-UI-10 | AT-UI-7, 12 |
| F-ARR-5 | AT-ARR-7 | | F-UI-14 | AT-UI-8 |
| F-ARR-6 | AT-ARR-8 | | F-COL-1 | AT-COL-1, 2 |
| F-ARR-7 | AT-ARR-9, 10 | | F-COL-2 | AT-COL-3 |
| F-ARR-8 | AT-ARR-11 | | F-COL-3 | AT-COL-4 |
| F-ARR-9 | AT-ARR-12 | | F-COL-4 | AT-COL-5, 6 |
| F-ARR-10 | AT-ARR-13 | | F-COL-5 | AT-COL-7, 8, 9 |
| F-ARR-11 | AT-ARR-14 | | F-COL-6 | AT-COL-10 |
| F-ARR-12 | AT-ARR-15 | | F-COL-7 | AT-COL-11 |
| F-ARR-13 | AT-ARR-16 | | F-COL-8 | AT-COL-12 |
| F-ARR-14 | AT-ARR-17 | | F-COL-9 | AT-COL-13, 14, 15, 16 |
| F-ARR-15 | AT-ARR-18 | | F-COL-10 | AT-COL-17, 18 |
| F-ARR-16 | AT-ARR-19, 20 | | F-COL-11 | AT-COL-19 to 23 |
| F-ARR-18 | AT-ARR-21 | | F-COL-12 | AT-COL-24, 25 |
| F-ARR-21 | AT-ARR-22 | | F-COL-13 | AT-COL-26 |
| F-ARR-22 | AT-ARR-23 | | F-COL-14 | AT-COL-27 |
| F-ARR-23 | AT-ARR-24 | | F-COL-15 | AT-COL-28, 29 |
| F-ARR-26 | AT-ARR-25 | | F-COL-16 | AT-COL-30 |
| F-ARR-27 | AT-ARR-26 | | F-COL-20 | AT-COL-31 |
| F-EDA-1 | AT-EDA-1 | | F-SYS-1 | AT-SYS-1 |
| F-PRL-1 | AT-PRL-1 | | F-QNT-1 | AT-QNT-1, 2 |
| F-PRL-2 | AT-PRL-2 | | F-QNT-2 | AT-PRL-10, AT-QNT-3 |
| F-PRL-3 | AT-PRL-3 | | | |
| F-PRL-4 | AT-PRL-4, 19 | | | |
| F-PRL-5 | AT-PRL-5 | | | |
| F-PRL-6 | AT-PRL-6 | | | |
| F-PRL-7 | AT-PRL-7 | | | |
| F-PRL-8 | AT-PRL-8, 9 | | | |
| F-PRL-9 | AT-PRL-10, 11, 12 | | | |
| F-PRL-11 | AT-PRL-13 | | | |
| F-PRL-12 | AT-PRL-14 | | | |
| F-PRL-15 | AT-PRL-15 | | | |

## Decisions in this file

| ID | Decision |
|---|---|
| D-AT-1 | Dropping a MIDI file into an empty project adopts its tempo map; otherwise the project tempo is kept. |
| D-AT-2 | Reference decoders for bounce tests: JUCE (WAV) and Symphonia (MP3). |
| D-AT-3 | The AT-REC-6 reference machine: a 2019-or-later 4-core/8-thread PC, 16 GB, SSD, Windows 11, an 8-input USB interface with its own ASIO driver. |
| D-AT-4 | The AT-PLG-1 list: TDR Nova, Valhalla Supermassive, MCompressor, Airwindows Consolidated, SPAN; Surge XT and Dexed. |
