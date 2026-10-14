# Weft Studio: notes for contributors and coding agents

- Clean-room project. Work from `docs/spec` and `docs/ARCHITECTURE.md` only. Never use another vendor's code, binaries, sounds, icons or artwork, and never use another DAW's product name in code, UI, comments or docs.
- When the spec is ambiguous, pick the simplest sensible behavior, record it as a `Decision:` in the relevant spec file (or `D-ARCH-n` in ARCHITECTURE.md §9), and continue.
- Every acceptance criterion (`AT-xxx` in `docs/spec/06-acceptance-tests.md`) gets a test; tag Catch2 tests with their level (`[U]`, `[E]`) and name them after the AT id once one applies.
- Audio-thread code (`AudioCallback::process` and anything it calls) must not lock, allocate, do I/O or throw.
- Dependencies come only from `third_party/cpp-deps.json` (C++) or Cargo. After changing either, run `python3 tools/third_party.py --write` and commit `THIRD_PARTY.md`.

## Build and test

```sh
cmake --preset dev && cmake --build --preset dev && ctest --preset dev   # Linux/Ninja
cmake --preset vs && cmake --build --preset vs && ctest --preset vs      # Windows/VS 2022
cargo test --workspace && cargo fmt --all --check && cargo clippy --workspace -- -D warnings
```

Linux can't run WASAPI/ASIO or Windows UI tests; those run on the Windows CI runner or need real hardware (`[HW]`).
