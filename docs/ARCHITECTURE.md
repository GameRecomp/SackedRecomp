# Architecture

## Overview

```
 your game files            recompiler (Rust, recomp/)                    build (CMake, C11)
 ─────────────────          ──────────────────────────────────            ─────────────────────────────
 CD image,       ─┐
 installer, zip  ├─ extract ─► orig/<game>/<variant>/                     generated/<game>/<variant>/*.c
 (archive.org)   ─┘                   │                                             │
                                     ▼                                              ▼
                    load ─► discover ─► lift ─► emit C  ──────────────►  + runtime/  + hle/  + platform/
                    rc-pe   rc-discover rc-x86  rc-emit-c                 + games/<game>/ (shell, patches)
                            ▲                                                       │
                     games/<game>/game.toml (hashes, seeds, patches)                ▼
                                                                          native executable per game
```

The recompiler runs **ahead of time**. It turns each pinned executable (and each DLL marked
`lift`) into C. The C build links that code against the runtime. The runtime provides the
guest memory, the dispatcher and the HLE implementations of every imported API. At run time the game
reads its own data files from the user's install. We never convert or redistribute assets.

## Pipeline (recomp/)

| Stage       | Crate          | Job |
|-------------|----------------|-----|
| identify    | `rc-manifest`  | Load `games/*/game.toml`, match files by SHA-256 against all variants. |
| load        | `rc-pe`        | Parse the PE: sections, imports (by name and ordinal), exports, relocations, Rich header. Refuse any binary whose hash is not in a manifest. |
| extract     | `rc-media`     | Read CD images (ISO 9660, cooked or as raw 2352-byte sectors: `.bin`, CloneCD `.img`, Alcohol `.mdf`), the installers on them or in a zip (NSIS 1.9x; Inno Setup 3.0.6.1, 4.2.2, 4.2.6 and GOG's 5.5/5.6 with its slices and Galaxy parts; InstallShield 5 and 6 cabinets with spanned volumes; Windows Installer packages with an MSZIP cabinet), without third-party tools. Every file is checked (Adler-32, CRC-32, MD5, SHA-1 or the manifest hash). The installer's registry entries become `registry-install.txt`, the release's name `recomp-variant.txt`. `recomp download` fetches a release from archive.org first. |
| discover    | `rc-discover` | Build the code map (`recomp discover`), because **no executable has relocations**. Recursive descent from the entry point, exports and manifest seeds, with `call`/`jmp [IAT]` imports and `switch` jump tables (bounded by `cmp`/`ja` or `and`, with index byte tables). Then candidates, each trial-decoded in a transaction that rolls back on implausible code: immediates and data constants that point into code (for DLLs exactly the relocated values), vtables from constructor stores, SEH scope tables and C++ EH `FuncInfo`. Then gap filling (padding, strings, unreferenced code). No-return functions are a fixed point over whole runs. Output: functions with byte ranges, labels, jump tables, vtables, data/padding/dead/unknown ranges, imports called, CPU features and the coverage report. DLLs can be rebased first (manifest `base`). `recomp codediff` compares two builds function by function. |
| lift        | `rc-x86` → `rc-ir` | Decode with `iced-x86` and turn each instruction into IR statements plus one terminator, using discovery's per-instruction `Flow` (import slots and jump tables already resolved). Integer, string, x87 (fast mode) and MMX instructions are modelled (MMX as 64-bit registers of their own and lane functions in `rc/mmx.h`); unmodelled ones (SSE) become named traps, not build failures. Labels that the program takes as values (SEH handlers, code pointers into a function) get an entry of their own: the owner's body lifted again, entered at the label. Shared with TropicoRecomp. |
| emit        | `rc-emit-c` | Write one C function per guest function (`<binary>_00401000`), grouped as `code_XXXX.c` per 64 KiB of guest address space, with `funcs.h` and the sorted table `table.c`. Registers and flags are C locals. A function entered above its lowest instruction starts with a `goto` to its entry. Output is deterministic. Shared with TropicoRecomp. |
| verify      | `rc-difftest` (dev only) | Every instruction form the lifter models, plus string-instruction and whole-program fixtures, run from random states on the real CPU in 32-bit mode (`tools/difftest/ref32.c`, freestanding, built with `clang -m32`) and as lifted C on the runtime (`tools/difftest/harness.c`). `--coverage EXE` checks that every form a binary uses is tested. Shared with TropicoRecomp (without its native and WebAssembly back ends). |
| convert     | `rc-convert`, `rc-emit-native`, `rc-emit-wasm`, `rc-web` | End to end without a compiler: medium → identify → lift → native code (Cranelift) or WebAssembly into a shell template (Windows, Linux) or the browser's storage, the user-facing path (`recomp convert --system`, the converter page). |
| CLI         | `rc-cli` (`recomp`) | `identify`, `info`, `list`, `extract`, `download`, `discover`, `codediff`, `lift`, `convert`, `template`. |

## Runtime model (runtime/)

`runtime/`, `hle/` and `platform/` are TropicoRecomp's, shared with it (decision log). This project adds: one
dispatch table per lifted binary (`rc_add_lifted`), relocation of a lifted DLL loaded at another base, the lifted
binaries in `rc_config.lifted`, SHA-256, and the game shell (`rc_shell`: `runtime/src/game.c`). Changes to the shared
files are kept general, so they can go back.

- **Start-up** (`rc_game_main` → `rc_run`): the shell finds the install (`--game-dir`, `$RC_GAME_DIR`), hashes the
  executable and the DLLs beside it and picks the lifted binaries whose SHA-256 match (a shell links every build lifted
  for its game). `rc_run` maps the lifted DLLs (relocated to their manifest `base`), then the executable, binds imports
  to the DLLs' exports or to HLE, sets up stack, TIB and PEB, runs each DLL's `DllMain` and calls the entry point.
- **Guest memory**: a 4 GiB reservation (on 64-bit hosts) with a virtual-memory and heap layer (`rc/vm.h`). Guest
  address `a` is host `rc_mem_base + a`; `rc/mem.h` has the accessors. Image base `0x400000` is fixed, since there are
  no relocations, so the game's own addresses are used as they are.
- **CPU state**: `rc_cpu` (`rc/cpu.h`, TropicoRecomp's layout) holds the eight GPRs (`r[8]`, also named
  `eax`…`edi` for hand-written code), one byte per arithmetic flag plus `df`, the x87 in the fast mode (`rc/x87.h`:
  host doubles, top, control/status/tag words) and the `fs` base (TEB). Lifted functions take `rc_cpu *` and keep
  registers and flags in C locals inside a function. They write them back at calls, returns and indirect transfers.
  `cpuid` reports a fixed P6-class CPU without MMX or SSE, and `rdtsc` a 1 GHz counter.
- **Stack**: the guest stack lives in guest memory, so `esp` is a real guest pointer. Calling conventions
  (`stdcall`, `cdecl`, `thiscall`) therefore need no special handling.
- **Calls**: direct calls are C calls; transfers to another function's entry are tail calls. Indirect calls and
  jumps (vtables, function pointers, the indirect jumps discovery leaves to run time) go through
  `rc_dispatch(cpu, target)`: patches first, then the sorted tables of the lifted binaries, then the reserved import
  addresses. An unknown target is a fatal error that names the address, and gets fixed in the manifest. A trap
  (`rc_trap`) names the instruction and its address.
- **PE loader** (`runtime/src/process.c`): maps headers and sections of an exe or DLL and binds every import slot.
  The executables have no relocations and always load at `0x400000`. The lifted DLLs do have relocations, and two of
  them collide (STLport and every Vorbis DLL prefer `0x10000000`). Each lifted DLL that collides gets a fixed base in
  the manifest (Sacked's `ogg.dll`: `0x10200000`); the recompiler applies its relocations before discovery and
  lifting, and the loader applies the same ones when it maps the DLL there. Lifted DLLs load after the lifted DLLs
  they import from. A section's file bytes are mapped to the end of its last page, as Windows does.
- **HLE**: every imported function has the same signature as lifted code (`void f(rc_cpu *)`). It reads its
  arguments from the guest stack and pops them (`stdcall`). Every import gets a reserved guest address, so
  `call [IAT]` lands in `rc_dispatch`, which calls the registered HLE function, or stops with
  `unimplemented import dll!Name` if there is none. `--check` lists what is missing without running the game, and
  `RC_TRACE=imports` logs every call.
- **COM**: DirectX interfaces are COM objects that live in guest memory. Their vtables hold reserved import
  addresses named after the methods (`IDirectDraw::Blt`), so the game's own vtable calls work unchanged and a missing
  method stops with its name. `CoCreateInstance` makes the classes HLE modules register (`rc_com_register`).
- **x87**: a fast mode on host doubles first. An exact 80-bit mode is added only if a game depends on it (both engines
  are MSVC 6 builds that set 53-bit precision in places **(to verify)**).
- **Threads**: the games use few threads (audio, network). They are modelled as cooperative fibers first,
  and switch to real threads only if needed.

## HLE layer (hle/)

There is one directory per Windows DLL. Every module is shared by all games.

| Module      | Backed by                      | Used by | Notes |
|-------------|--------------------------------|-----------|-------|
| `kernel32`  | libc / POSIX / Win32           | game, editor | VFS (case-insensitive, `\` → `/`, save redirection), heap, time, threads, TLS, file mappings, `GetPrivateProfile*` |
| `user32`    | platform (SDL3)                | game, editor | game: one window with a menu bar, message queue from host input, cursors, `MessageBox`; programs without DirectDraw (the editor): a desktop framebuffer (`RC_DESKTOP=WxH`) with a window tree, per-thread queues, hooks, menus, dialogs and the standard controls |
| `gdi32`     | platform + SDL3_ttf            | game, editor | object table (brushes, pens, fonts, bitmaps, DIB sections, palettes, regions), DCs with clipping, ROP3 blits, lines and shapes, text |
| `advapi32`  | per-user registry file         | both games | `Reg*` on the program's own registry: what the installer wrote (`registry-install.txt` in the game folder, from `recomp extract`), then `registry.txt` in the user directory |
| `ole32`     | runtime                        | game, editor | `CoInitialize(Ex)`/`CoCreateInstance` → the COM classes HLE modules register (`rc_com_register`) |
| `version`, `shell32` | runtime               | game, editor | version info of our own modules, `ShellExecute` → no-op |
| `ddraw`     | platform framebuffer           | both games | DirectDraw 7 COM, 16-bit surfaces (800x600), software blits, page flipping → SDL texture |
| `dinput`    | platform input                 | both games | DirectInput 7: keyboard (buffered) and mouse (relative motion, buttons) |
| `dsound`    | SDL3 audio                     | both games | DirectSound buffers fed into the platform's PCM streams (the music thread's stream, effects) |
| `winmm`     | runtime clock + files          | both games | `timeGetTime`; `mmio` for reading RIFF (WAV) files, with the buffered access |
| `msvcrt`    | runtime + kernel32             | Sacked, the Vorbis DLLs | MSVCRT.dll and MSVCP60.dll (Visual C++ 6): start-up, heap, strings, printf/scanf on guest arguments, stdio on guest `FILE`s over kernel32, the x87 helpers, threads; C++ exceptions (`__CxxFrameHandler`, `_CxxThrowException`, `_except_handler3`), RTTI, `std::string` |
| `mss32`, `wsock32`, `dmusic`, `dshow`, `dplay`, `dpnet`, `comctl32`, `comdlg32` | – | – | shared with the sister projects, not used here |

Lifted instead of reimplemented: STLport (`stlport_vc645.dll`), the Vorbis DLLs (`ogg.dll`, `vorbis.dll`,
`vorbisfile.dll`) and the Dutch Sacked's Visual C++ 7.1 runtime (`msvcr71.dll`, `msvcp71.dll`). They are shipped
with the games and have a narrow API below them (kernel32, MSVCRT), so lifting them is less work and more faithful
than reimplementing them. MSVCRT.dll is a system DLL the games do not ship: it is reimplemented.

## Platform layer (platform/)

This is a thin C API over SDL3, SDL3_ttf and SDL3_mixer (TropicoRecomp's): window, frame upload with integer or
aspect scaling, input events, text, audio (samples, tracks, PCM streams) and settings. It also has a `null` backend
for tests and CI (`RC_WITH_SDL3=OFF`). Nothing above `platform/` includes SDL headers. Scripted runs
(`RC_INPUT`, `RC_SCREENSHOT`, `RC_FRAMES`, `RC_CLOCK=virtual`) are in `hle/frames.c`.

## Games (games/<id>/)

| File          | Purpose |
|---------------|---------|
| `game.toml`   | Manifest: variants (installers, media), pinned binaries with role, treatment and (for colliding DLLs) `base`; per binary a `[binary.discover]` table with seeds, no-return functions, data ranges and jump tables; later patches. |
| `CMakeLists.txt` | Calls `rc_add_game(<id> TITLE … EXES …)` from `cmake/lift.cmake`. That generates the shell's `main()` from `cmake/game_main.c.in`, and the shell calls `rc_game_main()` in `runtime/src/game.c` (arguments, locating the install, later hash check and boot). The executable is `sacked-<id>` (`sacked-sacked`, `sacked-crazyschool`), and `sacked-<id>-<variant>` for every
other release in `orig/<id>/`. |
| `patches.c` *(none yet)* | Hooks at guest addresses, such as skipping the CD check, larger resolutions or fixes. Every patch is opt-in or documented. |

## Generated code (generated/, git-ignored)

The layout is `generated/<game>/<binary>-<variant>/` (for example `generated/sacked/gefeuert.exe-cd-de/`):
`codemap.json` from discovery, and from `recomp lift` `code_XXXX.c`, `funcs.h`, `table.c` (`<symbol>_table`) and
`binary.c` (the `rc_generated_binary` record: name, variant, hash, base, entry, table). `recomp lift` also rewrites
`generated/<game>/set.c`, the `rc_generated_set` of every binary lifted for the game so far. All binaries of a game
link into one executable. Each gets its own symbol prefix (from its directory name, e.g. `gefeuert_exe_cd_de_`),
and the shell picks the ones whose SHA-256 matches the user's copy. `cmake/lift.cmake` compiles everything below
`generated/<game>/` with the project's full warning set and defines `RC_HAVE_GENERATED`. It is
derived from copyrighted code, so it is **never committed**, never uploaded and never put into issues. Builds
regenerate it locally from the user's own files.

## Repository layout

```
.github/workflows/   CI: audit, Rust (fmt/clippy/test), C (Linux/Windows/macOS), the converter site
web/                 the converter page and the player (built by tools/build-web)
cmake/               lift.cmake: rc_add_game(), linking generated code when present; game_main.c.in
docs/                PLAN, ARCHITECTURE, DEVELOPMENT, TARGETS, RESEARCH
games/               one directory per game: game.toml, CMakeLists.txt, scenarios/
hle/                 Win32/DirectX/middleware reimplementations, one subdirectory per DLL
platform/            SDL3 backend + null backend
recomp/              Rust workspace: the recompiler and the `recomp` CLI
runtime/             guest memory, CPU state, dispatcher, HLE registry, logging, tests
tools/               audit.sh, git hooks, dev helpers
orig/                YOUR game files (git-ignored)
generated/           lifted C (git-ignored)
build/               CMake build trees (git-ignored)
```
