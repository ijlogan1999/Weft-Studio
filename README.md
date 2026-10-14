# Weft Studio

Weft Studio is a free, open-source digital audio workstation for Windows, built
for making music together. Several people can edit the same project at once,
online or offline, and every change merges.

A weft is the thread woven across a loom; Weft weaves many people's ideas into one song.

> **Status:** early development. Milestone 0 (repository skeleton, build, CI) is in
> progress. Nothing here makes music yet. See [the milestone plan](docs/ARCHITECTURE.md#7-milestone-plan).

## Support Weft

Weft is free and always will be. If you'd like to support its development, you can
donate here: **(donation link coming soon)**

## What it will do

The first release covers the core of a professional DAW:

- Audio and MIDI tracks with multitrack recording, takes and punch-in
- An arrange view with regions, snapping, markers and undo
- A piano roll with quantize and velocity editing
- A mixer with buses, sends, built-in effects and VST3 plugins
- Automation lanes
- Bounce to WAV and MP3
- Real-time collaboration: shared projects, live cursors, comments, chat and version history

The full behavior is specified in [`docs/spec`](docs/spec/00-README.md), and the
design in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## Building

You need CMake 3.25+, a C++20 compiler (Visual Studio 2022 or newer on Windows, GCC 13 or
Clang 17 on Linux), Python 3, and a Rust toolchain (1.90 or newer, from
[rustup](https://rustup.rs)). CMake downloads JUCE, Catch2 and Corrosion at
configure time; versions are pinned in [`third_party/cpp-deps.json`](third_party/cpp-deps.json).

```sh
# Windows (newest installed Visual Studio)
cmake --preset vs
cmake --build --preset vs
ctest --preset vs

# Linux (Ninja). JUCE needs: libasound2-dev libfreetype-dev libfontconfig1-dev
# libx11-dev libxcomposite-dev libxcursor-dev libxext-dev libxinerama-dev libxrandr-dev
cmake --preset dev
cmake --build --preset dev
ctest --preset dev

# Rust parts (the CRDT shim and the relay server)
cargo test --workspace
```

The `headless` preset skips JUCE and builds only the core, engine and CLI.

## Repository layout

| Path | Contents |
|---|---|
| `core/` | Document model on the CRDT, time maps, routing, media (no UI, no audio drivers) |
| `engine/` | Real-time audio engine and device layer, including the `NullDevice` used in tests |
| `app/` | The JUCE desktop application |
| `cli/` | `weft-cli`, a headless driver for engine and sync tests |
| `crates/weft-crdt/` | C API over the Automerge CRDT, linked into the app |
| `relay/` | Collaboration relay server (Rust) |
| `tests/` | Unit, engine and plugin tests, plus the fixture VST3 plugins |
| `docs/` | Spec, architecture and milestone reports |

## License

Weft Studio is licensed under the [GNU Affero General Public License v3](LICENSE).
Third-party components and their licenses are listed in [THIRD_PARTY.md](THIRD_PARTY.md).

VST is a registered trademark of Steinberg Media Technologies GmbH. ASIO is a
trademark of Steinberg Media Technologies GmbH.
