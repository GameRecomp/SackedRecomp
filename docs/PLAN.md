# Plan

## Goals

1. **Two games of Limbic Entertainment's 2D engine run natively** from their original executables:
   *Sacked: Your Last Day* / *Gefeuert! – Dein letzter Tag* (2003) and *Crazy School – Schulverweis!*
   (2003/04), in every language archived on archive.org: Sacked in German, English, Hungarian, Polish, Dutch and
   Russian, Crazy School in German, Russian and Polish (see [TARGETS.md](TARGETS.md)). They share one engine, so
   one runtime and one set of fixes serves all of them.
2. **Platforms: Windows, Linux and the browser (WebAssembly).** macOS is built in CI but not offered
   (`platforms.json`, as in the sister projects). **No Android build**: the Android project, its packaging and
   its converter target are not taken over.
3. **SDL3 is the only platform dependency** (SDL3, SDL3_ttf, SDL3_mixer), through the shared `platform/` layer.
4. **Same architecture and the same code** as TropicoRecomp, CulturesRecomp and HoMM4Recomp: the Rust recompiler
   (`recomp/`), the C11 runtime, HLE and platform layers, the manifests, the converter (CLI and browser page), CI
   and the packaging. Shared code stays general, so fixes can travel between the four repositories.
5. **Faithful.** The game logic is the original code, at the original resolution. **No resolution or widescreen
   patch** (the sister projects' `widescreen.c`-style patches are not planned here). Window scaling stays what the
   shared platform layer does for every game.
6. **Abandonware download built in.** Both games are offered on archive.org. The converter (CLI and browser page)
   fetches the medium of the chosen language from there itself (the page offers the languages as flags), checks
   it against the pinned SHA-256 and converts it; a local copy of the same file is accepted too.

Non-goals: shipping game code or data, remastering assets, resolution/widescreen patches, Android, other
releases than the pinned ones until their hashes are known.

## Starting point

This repository (SackedRecomp, first named GefeuertRecomp) is the fourth project on the same toolchain. Everything below "the game" is already written:

| Piece | Source | Work here |
|-------|--------|-----------|
| `rc-pe`, `rc-manifest`, `rc-discover`, `rc-ir`, `rc-x86`, `rc-emit-c`, `rc-difftest` | HoMM4Recomp | C++ EH `FuncInfo` reached through an **imported** `__CxxFrameHandler`; small binaries |
| `rc-media` (Inno 5.5/5.6, LZMA, ISO 9660, InstallShield 5, zip) | HoMM4Recomp | **raw 2352-byte CD images** (bin, CloneCD img, mdf), **NSIS (1.9x)**, **Inno Setup 3.0.6.1, 4.2.2, 4.2.6**, **Windows Installer + cabinet (MSZIP)**, **InstallShield 6** (spanned volumes), installers read from inside a CD image or zip |
| `rc-emit-native`, `rc-emit-wasm`, `rc-convert`, `rc-web` | HoMM4Recomp | two games, **download from archive.org**, no Android target |
| `runtime/` (memory, CPU, x87, dispatch, loader, SEH, threads, clock, host files) | HoMM4Recomp | C++ exceptions thrown and caught across lifted code and HLE |
| `hle/` kernel32, user32, gdi32, advapi32, ole32, ddraw (+D3D7), dinput, dsound, winmm, … | HoMM4Recomp | **new `msvcrt` and `msvcp60` modules**, `DirectInputCreateEx`, mmio, keyboard layout calls, a private font |
| (lifted) | – | the Dutch build's own `msvcr71.dll`/`msvcp71.dll`, lifted like the Vorbis DLLs |
| `platform/` SDL3 + null | all three | none expected |
| `web/`, `tools/`, `.github/workflows` | HoMM4Recomp | rename, two games, language flags that download the medium, no Android |

**HoMM4Recomp is the template** (the newest of the three; it contains CulturesRecomp's and TropicoRecomp's
shared code plus SEH dispatch, the two-file converter and the browser player). The code is copied at a pinned
commit (decision log), not vendored as a submodule.

The games, measured from the German archive.org images (details and the other languages in
[RESEARCH.md](RESEARCH.md)):

- **Sacked** (German CD): `sd-gefeg.bin` is a raw CD image (MODE2/2352) with an NSIS installer ("Limbic Installer (NSIS)",
  an old NSIS build that 7-Zip does not open). `Gefeuert.exe`: PE32, MSVC 6, base `0x400000`, no relocations,
  `.text` 0.39 MiB. It links the C runtime **dynamically** (`MSVCRT.dll`, 56 imports) and STLport
  (`stlport_vc645.dll`, shipped, 20 imports), plays music through `vorbisfile.dll` → `vorbis.dll` → `ogg.dll`
  (shipped). DirectDraw 7, DirectInput 7, DirectSound, GDI text with its own `International.ttf`.
- **Crazy School**: `CrazySchool.iso` (ISO 9660) with an Inno Setup **3.0.6.1** installer (isx, zlib).
  `crazyschool.exe`: PE32, MSVC 6, base `0x400000`, no relocations, `.text` 0.30 MiB, C runtime **statically
  linked**. The same DirectDraw 7 / DirectInput 7 / DirectSound / Ogg Vorbis stack, other builds of the three
  Vorbis DLLs.
- Neither game uses Direct3D, Bink, Miles or the network. Neither checks for the CD.

Measured against HoMM4Recomp's HLE layer, the system imports the two executables still lack are few:
`GetKeyboardLayout`, `MapVirtualKeyExA`, `ToAsciiEx`, `mmioOpenA`, `mmioRead`, `mmioAscend`,
`AddFontResourceA`, `DirectInputCreateEx`. The large new piece is `MSVCRT.dll` (used by Sacked's executables,
STLport and all six Vorbis DLLs) with the C++ exception machinery.

## Order of work

```
 M0 ─ M1 ─ M2 ─ M3 ─┬─ M4a Crazy School boots (static CRT) ─┐
                    └─ M4b msvcrt + C++ EH → Sacked boots ───┴─ M5 ─ M6 ─ M7 ─ M9 release
                                                                    └─ M8 converter, packages, browser
```

Both games go through M0–M3 together: the binaries are small, and the work is shared. **Crazy School leads from
M4**, because it needs no new C runtime: it proves the shared stack (DirectDraw 7, DirectInput 7, DirectSound,
Vorbis DLLs) on this engine while `msvcrt` is written for Sacked. From M5 on, every engine fix serves both.
M8 starts early in a reduced form (CLI convert with download for Linux), as in HoMM4Recomp.

## Milestones

Status: ✅ done · 🔶 in progress · ⬜ not started

| #   | Milestone                          | Status | Exit criterion |
|-----|------------------------------------|--------|----------------|
| M0  | Foundation                         | ✅ | Shared code imported from HoMM4Recomp and renamed; Android and Heroes IV parts removed; CMake and Cargo build; CI and audit hook; `games/sacked/game.toml` and `games/crazyschool/game.toml` pin every medium and binary; `recomp identify` recognises them. |
| M1  | Read the inputs                    | ✅ | `recomp extract` unpacks every pinned medium of both games (nine), and the installers inside them, into `orig/<game>/<variant>/` without third-party tools: the Inno installers byte-identical to innoextract 1.9, the Windows Installer package identical to cabextract, NSIS complete by its header's own file list, InstallShield 6 by its MD5s, every binary verified by hash. |
| M2  | Find the code                      | ⬜ | Coverage report for all 20 pinned binaries: 100 % classified, every leftover settled by analysis or a manifest hint. |
| M3  | Lift and build                     | ⬜ | Every function lifts; no trap in reachable code; the generated C compiles warning-free (gcc, clang, MSVC in CI) and links into `sacked-sacked` and `sacked-crazyschool`. |
| M4a | Crazy School boots                 | ⬜ | CRT start-up, registry, files, threads; the game creates its window and sets a DirectDraw 7 mode. |
| M4b | C runtime and Sacked boots         | ⬜ | `hle/msvcrt` and `hle/msvcp60` serve every import of the binaries that link them; C++ exceptions are thrown and caught (unit tests); Sacked reaches its window and display mode. |
| M5  | Picture and input                  | ⬜ | Both main menus render; mouse and keyboard (DirectInput 7 and window messages) work; text in `International.ttf`; windowed and full screen. |
| M6  | Sound and music                    | ⬜ | DirectSound effects and speech (WAV through mmio), Ogg music through the lifted Vorbis DLLs. |
| M7  | In game                            | ⬜ | Every level of both games can be played through (German first, then each language starts and plays a level); high scores and progress are saved and loaded; scripted soak runs (`tools/run-scenario`) without trap. |
| M8  | Converter and platforms            | ⬜ | `recomp convert` and the browser page download (or take) the medium of the chosen language and produce Windows and Linux builds and a browser build; all start and reach a level. |
| M9  | **Release v0.1**                   | ⬜ | Both games played through on Linux, Windows and in the browser; release notes with the known issues. |

### M0 – Foundation

- [x] Copy from HoMM4Recomp at a pinned commit (decision log): `recomp/` (all crates), `runtime/`, `hle/`,
      `platform/`, `cmake/`, `third_party/`, `tools/`, `web/`, `.github/`, `.clang-format`, `.editorconfig`,
      `.gitattributes`, `CMakePresets.json`, `CONTRIBUTING.md`
- [x] **Not** taken over: `android/`, `tools/build-android`, the Android CI jobs and release assets, the APK
      choice on the page and in `rc-convert`; `games/heroes4/` with `widescreen.c` and its patch points
- [x] Shared modules the games do not use (`dmusic`, `dshow`, `dplay`, `mss32`, `wsock`, D3D7, Bink support)
      stay in the tree, so the code stays the same as in the sister projects
- [x] Rename: project `sackedrecomp`, CLI help, window titles, user directory (`sackedrecomp/<game>`), web
      page, AppImage names, release asset names (`SackedRecomp-converter-<system>-<arch>`); `platforms.json`
      `android: false`
- [x] `games/sacked/game.toml` and `games/crazyschool/game.toml`: every medium with its archive.org download
      URL, size and SHA-256, every binary with role and treatment (below); `rc_add_game(sacked …)` →
      `sacked-sacked`, `rc_add_game(crazyschool …)` → `sacked-crazyschool`
- [x] `README.md`, `CLAUDE.md` adapted from HoMM4Recomp
- [ ] `docs/ARCHITECTURE.md`, `docs/DEVELOPMENT.md` adapted (with M3, when the build is known)
- [x] `tools/audit.sh`, `tools/hooks/` (pre-commit, commit-msg) enabled; `.gitignore` covers `orig/`,
      `generated/`, `*.bin`, `*.iso`, `*.cue`, the games' data extensions
- [x] CI as in the sister projects (not run: nothing pushed yet) (audit, Rust fmt/clippy/test, C build and tests with the null platform on
      Linux, Windows, macOS; web build), without Android
- [x] Icons (`tools/icons/sacked.svg`, `tools/icons/crazyschool.svg`, our own drawings, no game art)

Binary treatment:

| Game | File | Role | Treatment | Why |
|------|------|------|-----------|-----|
| both | `Gefeuert.exe`, `sacked.exe` (×3), `Ontslagen.exe`, `crazyschool.exe` (×2), `CRAZYSCHOOL.exe` | main | lift | the games: eight builds |
| sacked | `stlport_vc645.dll` | library | lift | shipped with the game; C++ library code the game calls into (2062 exports) |
| both | `ogg.dll`, `vorbis.dll`, `vorbisfile.dll` | library | lift | shipped; narrow API below them (MSVCRT or a static CRT); three different builds, each pinned |
| sacked/nl | `msvcr71.dll`, `msvcp71.dll` | library | lift | shipped with the Dutch build; only kernel32 below them |
| sacked | `MSVCRT.dll`, `MSVCP60.dll` | system | **hle** (new) | Windows system DLLs, not shipped; must be replaced |
| – | `DirectX/DX81*.exe`, launchers, installer engines, `uninstall.exe`, wallpapers | – | skip | DirectX redistributable, CD launchers, InstallShield engine files, uninstaller |

All Vorbis DLLs and STLport have the image base `0x10000000`: they are rebased by the manifest's `base`
(the shared mechanism HoMM4Recomp uses for `binkw32.dll` and `Mp3dec.asi`). Lifting the Vorbis DLLs instead of
replacing `vorbisfile` with a native decoder keeps the code faithful and avoids reimplementing
`ov_open_callbacks`, which calls back into the game.

### M1 – Read the inputs

- [x] `rc-media`: raw CD images (`rawcd`): a file of 2352-byte sectors (sync pattern, Mode 1 data at offset 16,
      Mode 2 Form 1 at 24) is read as the ISO 9660 image it holds: `.bin` (MODE2), CloneCD `.img` (MODE1),
      Alcohol `.mdf` (MODE1); the `.cue`/`.ccd`/`.sub`/`.mds` files are not needed
- [x] Nested media: an installer read from inside a CD image (shared readers for several files of one image) or
      from inside a zip (read into memory), without unpacking to disk
- [x] `rc-media`: **NSIS 1.9x** (`nsis`): first header, raw-deflate blocks, the header's sections and entries
      (opcodes 14 create directory / output directory, 21 extract, 50 write registry, 42 shortcut, 61 uninstaller,
      1 return), string variables (`0xF3` = `$INSTDIR`). Verified by the header's own list (every block used,
      every deflate stream ending at its block end) and the binaries' hashes; four installers (de, en, hu, pl)
- [x] `rc-media`: **old Inno Setup** (`inno3`): 3.0.6.1 (isx, zlib, Adler-32), 4.2.2 and 4.2.6 (LZMA1 solid
      chunks, MD5, the 4.1.8 call filter). Byte-identical to innoextract 1.9 for all three installers
- [x] `rc-media`: **Windows Installer** (`cfb`, `msi`, `cab`): compound file, string pool, `_Columns`, the File,
      Component, Directory, Media and Registry tables; external cabinet with MSZIP folders (own window handling on
      miniz_oxide). Identical to cabextract's output for the Russian Crazy School
- [x] `rc-media`: **InstallShield 6** (`iscab`): 0x57-byte file descriptors, 64-bit sizes, MD5 per file, linked
      files, files spanning `DATA1.CAB`/`DATA2.CAB`; version 5 kept working
- [x] Registry entries the installers write → `registry-install.txt` (NSIS: `HKLM\SOFTWARE\Limbic
      Entertainment\Gefeuert|Sacked` `Install_Dir`; Inno: `HKLM\SOFTWARE\LIMBIC\CRAZYSCHOOL` or
      `HKLM\Software\Limbic` `INSTALL_DIR`)
- [x] Manifests carry each medium's download URL; `recomp identify` recognises all nine media
- [ ] Ignored tests `orig_extract` for every medium in `orig/` (with M2's `orig_discover`)
- [ ] `sacked-<game> --check` lists unimplemented imports (with M3)

### M2 – Find the code

- [ ] Discovery on the eight game builds, `stlport_vc645.dll`, `msvcr71.dll`, `msvcp71.dll` and the nine Vorbis
      DLLs with the shared rules; for the DLLs with and without their relocations
- [ ] MSVC 7.1 builds (Polish Crazy School, Dutch Sacked, the VC 7.1 runtime): CRT shapes the MSVC 6 rules do not
      know yet (security cookie, `__SEH_prolog`, SSE2 paths in the CRT's math)
- [ ] C++ EH: in `Gefeuert.exe` the `FuncInfo` stubs end in `jmp [__imp___CxxFrameHandler]` (import) instead of
      a jump to a statically linked handler: extend the shared rule so catch and unwind funclets are found
      either way
- [ ] Bases for the rebased DLLs chosen so nothing collides; CPU features recorded (MMX/3DNow!/SSE paths in the
      Vorbis builds **(open)**)
- [ ] 100 % classified, no unknown byte; ignored test `orig_discover`

### M3 – Lift and build

- [ ] Every function lifts; traps only in unreachable code, each one explained
- [ ] `games/sacked/` and `games/crazyschool/` shells link every build lifted for the game
- [ ] Warning-free with gcc and clang locally, MSVC in CI; build time measured (expected: a few tens of MB of C,
      well under a minute)

### M4a – Crazy School boots

- [ ] CRT start-up of the static CRT (heap, environment, locale, `SetUnhandledExceptionFilter`, `IsBad*Ptr`)
- [ ] Registry reads under `HKLM\SOFTWARE\Limbic Entertainment\Crazy School`; files from the install folder
- [ ] `CreateThread`/`ExitThread`: what the thread does **(open)**
- [ ] The window is created and a DirectDraw 7 display mode set

### M4b – C runtime and Sacked boots

- [ ] `hle/msvcrt` (new, general, offered back to the sister projects): every import of the binaries that link
      it (≈ 120 functions in all), grouped as start-up (`__getmainargs`, `_initterm`, `_acmdln`, `__p__fmode`,
      `__p__commode`, `__set_app_type`, `_controlfp`, `_adjust_fdiv`, `_onexit`, `__dllonexit`, `exit`, …), heap
      (`malloc`…`realloc`, `operator new` with `_set_new_handler`/`_set_new_mode`), stdio on guest `FILE`s in
      MSVC's layout (`_iob` and `__pioinfo` are **data** exports STLport reads directly), strings and ctype,
      formatting (`sprintf`, `_snprintf`, `_vsnprintf`, `sscanf`, `_ecvt`, `_fcvt` with MSVC's rules), math
      including the x87-register helpers (`_ftol`, `_CIpow`, `_CIacos`, `_CIsinh`, `_CIcosh`), `qsort` with a
      guest callback, `setlocale` (C locale), threads (`_beginthreadex`, `_endthreadex`), `_errno`
- [ ] C++ runtime in HLE: `_CxxThrowException` (exception `0xE06D7363` through the shared SEH dispatch),
      `__CxxFrameHandler` (MSVC 6 `FuncInfo` 0x19930520: unwind map, try blocks, catch matching by
      `ThrowInfo`/`CatchableType`, catch funclets entered as lifted labels, rethrow, destruction of the exception
      object), `_except_handler3` (scope tables), `__RTDynamicCast`, `type_info` and `exception` members (their
      vtables in HLE-owned guest memory), `terminate`, `_purecall`
- [ ] `hle/msvcp60`: `std::bad_alloc` constructor, copy constructor and destructor
- [ ] Unit tests for the C++ EH paths (throw/catch by type, by base class, `catch (...)`, rethrow, unwinding of
      locals) with small lifted fixtures
- [ ] Sacked reaches its window and sets a display mode

### M5 – Picture and input

- [ ] The display modes both games set (depth, palettes from `CS_*.PAL`, 8-bit vs 16-bit) **(open)**; blits,
      colour keys and locks the engine uses
- [ ] `DirectInputCreateEx` with `IDirectInput7`/`IDirectInputDevice7` for keyboard and mouse
- [ ] `GetKeyboardLayout`, `MapVirtualKeyExA`, `ToAsciiEx` (text entry for names and high scores)
- [ ] `AddFontResourceA` of a private TrueType file, `CreateFontA`/`ExtTextOutA`/`GetTextExtentPoint32A`
      through SDL3_ttf (`International.ttf`); `StretchDIBits`
- [ ] Windowed and full screen, cursor

### M6 – Sound and music

- [ ] `mmioOpenA`, `mmioRead`, `mmioAscend` (with the existing mmio calls) for the WAV effects and speech
- [ ] DirectSound buffers, volume and pan as the engine uses them
- [ ] Ogg music through the lifted `vorbisfile.dll`, fed from the game's own streaming code (thread?) **(open)**

### M7 – In game

- [ ] Every level of Sacked (`Level_00`–`Level_19`) and of Crazy School (`Level_00`–`Level_20`), both modes
      ("Zeit", "Punkte"), the mini games
- [ ] Every other language: the game starts, its texts show (Cyrillic and Central European code pages in GDI text
      and in the games' own fonts) and a level is played
- [ ] High scores, unlocked levels and settings saved and read back
- [ ] Scripted scenarios per game (`games/<game>/scenarios`) and a one-hour soak each without trap

### M8 – Converter and platforms

- [ ] `recomp convert --system linux|windows|web --download sacked|crazyschool --language de|en|hu|pl|nl|ru`:
      fetch the medium from its manifest URL, stream it to disk with progress, check size and SHA-256, then
      convert; without `--download`, a local medium is taken as before
- [ ] Browser page: per game a row of **flags** for its languages; choosing one downloads that language's medium
      from archive.org with `fetch()` (CORS) into the worker, checks it and converts it; the file picker stays as
      a fallback; the page states that the games are offered there as abandonware
- [ ] Packages: Windows (zip), Linux (AppImage), browser build; no Android
- [ ] All three start and reach a level

### M9 – Release v0.1

- [ ] Both games played through on Linux, Windows and in the browser
- [ ] Release notes with the known issues; README for players

## Definition of done (per release)

- Every level of both games can be played to the end on Linux, Windows and in the browser.
- No trap and no unimplemented import in a one-hour soak per game.
- The converter produces the builds from the archive.org images and from local copies.
- Docs current: PLAN ticked, RESEARCH answered, TARGETS and the manifests in sync.

## Risks

| Risk | Impact | Plan |
|------|--------|------|
| `MSVCRT.dll` and C++ exceptions in HLE (new to the toolchain) | Sacked does not start or crashes on the first `throw` | Crazy School leads M4–M5 meanwhile; EH unit tests before the game; the shared SEH dispatch and unwinding already exist |
| STLport reads `_iob` and `__pioinfo` directly | wrong `FILE` layout breaks its streams | keep MSVC 6's structures in guest memory; tests with STLport's stream functions |
| Old NSIS build without a reference extractor | wrong file names or layout | the header's own entries are the reference; binaries pinned by hash; compare with a Windows install if needed |
| archive.org CORS for the browser download | the page cannot fetch the images | a quick check with `curl` showed no `Access-Control-Allow-Origin` on the download (redirect and mirror): **confirm in a real browser in M1**; the file picker stays as fallback |
| archive.org items change or vanish | download fails | pinned SHA-256 refuses changed files; local images keep working |
| Eight game builds, three builds of each Vorbis DLL | more lift and test work | they are small; the engine is the same, so fixes and hints carry over; German first, the others must start and play a level |
| MSVC 7.1 builds (pl Crazy School, nl Sacked, VC 7.1 runtime) | discovery rules and CPU features (SSE2 in the CRT) not seen in the sister projects | measure in M2; the CRT's SSE2 paths are chosen by `cpuid`, which reports none |
| Non-Latin text (Russian, Polish, Hungarian) | wrong glyphs in GDI text | GDI's code page handling through SDL3_ttf; test each language in M7 |
| Engine runs on real time (timing, sound thread) | races in the cooperative thread layer | identify each thread in M4; virtual clock for scripted runs |

## Research questions

Tracked with answers in [RESEARCH.md](RESEARCH.md):

1. ~~Which NSIS version is Limbic's installer built from, and what is its opcode numbering?~~ (answered,
   RESEARCH)
2. Which display modes do the games set (8-bit with `.PAL` palettes, 16-bit)? What does "DirectX 8.1 3D card"
   in Crazy School's requirements refer to, when only DirectDraw 7 is used?
3. What do the threads do (`_beginthreadex` in Sacked, `CreateThread` in Crazy School)?
4. Which registry values must exist before the first start (installer keys vs. the game's own defaults)?
5. Do the games throw C++ exceptions in normal play, or only on errors?
6. Where is progress saved (`HIGHSCORE.DAT`, registry)?
7. Do the browser's requests to archive.org get CORS headers?
8. Do the localised builds differ in code (beyond their texts), and how much of the engine fixes carry over?

## Decision log

| Date       | Decision |
|------------|----------|
| 2026-11-22 | The project follows TropicoRecomp, CulturesRecomp and HoMM4Recomp: MIT license, generated code and runtime in C11, the toolchain in Rust, SDL3 as the only platform dependency. Platforms: Windows, Linux, browser; macOS built in CI, not offered; **no Android**. |
| 2026-11-22 | HoMM4Recomp is the template for the shared code (at the commit pinned in M0). Code is kept general, so fixes can go back to the three sister projects. |
| 2026-11-22 | One repository for both games: the same engine (Limbic), the same middleware. Repository and project renamed from GefeuertRecomp to **SackedRecomp**; game ids `sacked` and `crazyschool`, programs `sacked-sacked` and `sacked-crazyschool`. |
| 2026-11-22 | Targets: every language of both games archived on archive.org (Sacked: de, en, hu, pl, nl, ru; Crazy School: de, ru, pl), pinned by hash with their exact download URLs. The converter downloads the chosen language itself (abandonware); local copies of the same files are accepted. Where an item offers several forms, the smallest complete one is pinned (the Dutch CD's zip, not its 788 MB image; the Russian CloneCD images, not the RAR archives). |
| 2026-11-22 | The Dutch build's own `msvcr71.dll`/`msvcp71.dll` are lifted (shipped, only kernel32 below them), not replaced: less HLE, the original behaviour. |
| 2026-11-22 | No resolution or widescreen patches: the games run at their own resolution. |
| 2026-11-22 | `MSVCRT.dll` and `MSVCP60.dll` are HLE (system DLLs, not shipped); `stlport_vc645.dll` and the Vorbis DLLs are lifted. |
| 2026-11-22 | Crazy School leads from M4 (static CRT), Sacked follows once `hle/msvcrt` and the C++ EH work. |
| 2026-11-22 | M0: the shared code copied from HoMM4Recomp at `df5671d`; Android parts and Heroes IV's game directory not taken over; the shared crates and platform code keep their Android paths unchanged (general code). |
