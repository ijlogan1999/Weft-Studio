# Weft Studio: Behavioral Specification Pack

**Working product name:** Weft Studio ("Weft" for short). A weft is the thread woven across a loom's warp; the name reflects many people weaving one project together. The name is provisional and should get a trademark search before any public release.

**Target platform:** Windows 10 (22H2) and Windows 11, x64. ARM64 Windows is a later target.

**Status:** Phase 1 deliverable. This pack contains no code.

## Clean-room provenance

This pack was written under the following rules, and every later phase must keep them:

1. Sources used: general digital audio workstation (DAW) and audio engineering knowledge, the kind of workflow information found in any vendor's public user guide, public tutorials, and reviews. Nothing was taken from any application binary, and no binary was decompiled, disassembled, or inspected.
2. No third-party code, sounds, loops, samples, icons, fonts, or UI artwork are referenced or reproduced. All bundled content (metronome click, demo project, icons) must be original or licensed under terms that allow redistribution, and its source must be recorded in `THIRD_PARTY.md` in the code repository.
3. Feature names in this pack are generic industry terms or Weft's own names. Where a well-known product uses a proprietary marketing name for a concept, this pack uses a descriptive name instead (for example "virtual drummer" is not used; "pattern generator" is). The UI must not use another vendor's trademarks.
4. Layout descriptions in `04-ui-spec.md` are functional: they say what regions exist and what they do, not how another product draws them. Weft's visual identity (palette, iconography, typography, spacing) is defined in its own design system, separate from this pack.

## Files

| File | Contents |
|---|---|
| `01-feature-inventory.md` | Every user-facing feature, tagged **MVP**, **v2**, or **Later**, with stable IDs (`F-xxx`). |
| `02-workflows.md` | Step-by-step behavior of the core workflows (`W-xx`). |
| `03-data-model.md` | Project document schema: tracks, regions, takes, MIDI, automation, plugin chains, tempo/meter map, collaboration metadata. |
| `04-ui-spec.md` | Main window regions and their functional behavior. |
| `05-collaboration-spec.md` | CRDT document, relay server, presence, blob sync, comments, chat, version history. |
| `06-acceptance-tests.md` | Testable behaviors (`AT-xxx`) for every MVP feature. |
| `07-reference-coverage.md` | Internal gap analysis against a reference DAW's public user guide and key commands. It stays in the project workspace and is not part of this repository; nothing in it ships. |

## Conventions

- **IDs.** Features are `F-<area>-<n>`, workflows `W-<n>`, acceptance tests `AT-<area>-<n>`. Acceptance tests cite the features they cover. IDs are never reused once assigned.
- **Tags.** **MVP** ships in the first release. **v2** is the next major release. **Later** is acknowledged but unscheduled.
- **Time.** Musical positions are expressed in *ticks* at 960 PPQN (pulses per quarter note). Absolute positions are expressed in *samples* at the project sample rate. Bars and beats are 1-based in the UI (bar 1, beat 1) and 0-based in the data model.
- **Decisions.** Where the source material was ambiguous or silent, this pack picks the simplest sensible behavior and marks it with **Decision:** so it can be found and revisited. A list of all decisions is at the end of each file.
- **Keyboard shortcuts** are Weft's own defaults, chosen to follow common Windows conventions (Ctrl+Z undo, Ctrl+S save, Space play/stop). All are rebindable.
