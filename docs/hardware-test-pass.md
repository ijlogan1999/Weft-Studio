# Hardware test pass

The [HW] acceptance tests need a real Windows PC, audio interface and MIDI keyboard, so CI can't run
them. This is the checklist for a person running them before a release. Use the installer from the
latest green CI run on `main` (the `WeftStudio-windows-installer` artifact of the "Windows installer
(Release)" job), or your own Release build. Both include ASIO.

Record each pass in a copy of the table at the end: the date, the Weft version (Help > About), the
machine, the interface and driver versions, and a note for anything that failed.

## Before you start

- The machine meets the reference spec for AT-REC-6 (D-AT-3 in spec 06): Windows 11 23H2 or later, a
  2019-or-later 4-core/8-thread x64 CPU at 3 GHz or more, 16 GB RAM, SSD, Windows' "Best performance"
  power mode, and a USB interface with 8 or more inputs and its maker's ASIO driver.
- Install Weft with the installer. It should put Weft Studio in the Start Menu and on the desktop, and
  Settings > Apps should list it with an uninstaller.
- Hovering the CPU meter in the control bar shows the dropout and disk underrun counts used below.

## AT-REC-6: 8 tracks for 10 minutes at 64 samples

1. Audio & MIDI Settings: driver ASIO, the interface, 48 kHz, buffer 64.
2. Create 8 mono audio tracks on inputs 1 to 8 and arm them all. Feed signal to every input (a
   test-tone generator, or microphones in the room).
3. Record from bar 1 for 10 minutes, then stop.
4. Pass when the CPU meter's tooltip shows 0 dropouts and 0 disk underruns and each of the 8 takes
   plays back for the whole 10 minutes with no gaps or clicks.

## AT-PLG-1: the fixed VST3 list

Install the latest free release of each (D-AT-4): TDR Nova, Valhalla Supermassive, MeldaProduction
MCompressor, Airwindows Consolidated, Voxengo SPAN, Surge XT and Dexed. Then:

1. Open the Plug-in Manager and press Rescan. All seven are listed and none is marked failed.
2. On an audio track playing a loop, insert each effect in turn. The sound changes as expected (SPAN
   shows a spectrum), and its editor opens in a floating window, moves and closes.
3. Put each instrument on an instrument track and play notes from the MIDI keyboard and from a MIDI
   region. Open and close its editor.
4. Save, close and reopen the project. Every plug-in comes back with its settings.
5. Pass when nothing errors, freezes or crashes and Windows' Event Viewer shows no Weft crash.

## AT-UI-3: display scaling

1. At 100%, 150%, 200% and 250% (Settings > System > Display > Scale), open a project with audio and
   MIDI regions. Text, waveforms, meters and icons are sharp, not blurred, and buttons scale with the
   text.
2. With two monitors at different scales, drag the window from one to the other. It redraws sharply at
   the new scale once it lands.

## AT-UI-4: drivers

1. In Audio & MIDI Settings, select WASAPI shared, then WASAPI exclusive, then ASIO (the interface's
   driver, or ASIO4ALL). Each one opens and plays the Test tone.
2. The buffer sizes offered match what the driver allows (compare with the interface's own control
   panel), and the round-trip latency line shows a figure.
3. While playing, change the device or the buffer size. Playback stops, the device reopens, and
   pressing Play resumes cleanly.

## AT-UI-6: MIDI inputs

1. In Audio & MIDI Settings, untick the keyboard under MIDI inputs. Its notes no longer reach an armed
   instrument track. Tick it again and they do.
2. With Weft open, unplug the USB MIDI keyboard and plug it back in (or plug in a second one). It shows
   up under MIDI inputs and plays the track without restarting Weft.

## Installer

1. Run the installer over an existing install. It removes the old version first and keeps your
   projects and settings.
2. Uninstall from Settings > Apps. The program folder and shortcuts are gone; projects stay.

## Record

| Date | Version | Machine and interface | REC-6 | PLG-1 | UI-3 | UI-4 | UI-6 | Installer | Notes |
|---|---|---|---|---|---|---|---|---|---|
| | | | | | | | | | |
