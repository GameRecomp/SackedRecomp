# SackedRecomp for developers

SackedRecomp is a **static recompilation** of Limbic Entertainment's *Sacked: Your Last Day* (*Gefeuert! – Dein
letzter Tag*, 2003) and *Crazy School – Schulverweis!* (2003/04). It translates the original 32-bit x86 Windows
executables ahead of time into portable C (or native code, or WebAssembly) and runs them on a small runtime that
reimplements the parts of Windows and DirectX the games call, on top of SDL3. It is the fourth project on the
toolchain of [TropicoRecomp](https://github.com/GameRecomp/TropicoRecomp),
[CulturesRecomp](https://github.com/GameRecomp/CulturesRecomp) and
[HoMM4Recomp](https://github.com/GameRecomp/HoMM4Recomp), and shares their recompiler, runtime, HLE and platform
code.

Every medium and executable we support is pinned by SHA-256 in [`TARGETS.md`](TARGETS.md) and
`games/<game>/game.toml`. All of them are abandonware archived on archive.org; `recomp download` fetches them.

## Quick start

Requirements: CMake ≥ 3.25, Ninja, a C11 compiler, Rust (stable), curl, and SDL3 (with SDL3_ttf and SDL3_mixer)
for a window and sound. A full build of every release compiles about 250 MB of generated C: a few minutes on
8 cores.

```sh
# Recompiler toolchain (Rust)
cargo build --release --manifest-path recomp/Cargo.toml
cargo test --manifest-path recomp/Cargo.toml

# The games' media from archive.org (any language: sacked de en hu pl nl ru, crazyschool de ru pl)
recomp/target/release/recomp download --dest orig sacked de
recomp/target/release/recomp download --dest orig crazyschool de

# Identify and unpack them → orig/<game>/<variant>/, binaries verified (the build does this itself)
recomp/target/release/recomp identify orig/
recomp/target/release/recomp extract orig/sd-gefeg.bin

# Runtime, HLE and the game shells (C); configuring unpacks and lifts what is missing
CMAKE_PREFIX_PATH=<your SDL3 prefix> cmake --preset dev
cmake --build --preset dev
ctest --preset dev

# The games
build/dev/games/sacked/sacked-sacked --game-dir orig/sacked/cd-de
build/dev/games/crazyschool/sacked-crazyschool --game-dir orig/crazyschool/cd-de
cmake --build --preset dev --target sacked-sacked-ru      # another release: a target of its own
build/dev/games/sacked/sacked-sacked --check --game-dir orig/sacked/cd-de    # imports the HLE layer lacks
RC_TRACE=imports|files|threads|ddraw  RC_TRACE_SEH=1                         # traces

# Scripted runs: headless, on the virtual clock, compared with the accepted run
tools/run-scenario sacked firstlevel
tools/run-scenario crazyschool firstlevel

# Without a compiler, as players do: a Linux build from the German Sacked
tools/build-shell out/Sacked.AppImage sacked
recomp/target/release/recomp template --game sacked --system linux --shell out/Sacked.AppImage --out templates/Sacked-linux.zip
recomp/target/release/recomp convert --system linux --templates templates --download sacked --language de

# The converter page with the browser player (needs emsdk's environment for the web shells)
tools/build-web-shell out/web sacked && tools/build-web-shell out/web crazyschool
tools/build-web out/site "" out/web

# Windows without Windows (Docker): MinGW-w64 cross build, warnings as errors, tests under Wine
tools/check-windows
```

## Documentation

| File | Contents |
|------|----------|
| [`PLAN.md`](PLAN.md) | Goals, milestones, order of work, risks, decision log |
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | Pipeline, runtime model, HLE layer, repository layout |
| [`TARGETS.md`](TARGETS.md) | Every supported medium and binary, with hashes and download URLs |
| [`RESEARCH.md`](RESEARCH.md) | Findings from analysing the media and the binaries |
| [`CONTRIBUTING.md`](../CONTRIBUTING.md) | Rules, especially regarding copyrighted material |
| [`CLAUDE.md`](../CLAUDE.md) | Short orientation for a new working session: state, conventions, next steps |
