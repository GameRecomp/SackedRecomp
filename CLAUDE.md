# SackedRecomp – orientation for a new working session

Static recompilation of Limbic Entertainment's two 2D prank games, *Sacked: Your Last Day* (German:
*Gefeuert! – Dein letzter Tag*, 2003) and *Crazy School* (*Schulverweis*, 2003/04), into portable C on an
SDL3 runtime. Fourth project on the toolchain of `../TropicoRecomp`, `../CulturesRecomp` and `../HoMM4Recomp`;
HoMM4Recomp is the template for all shared code. Start here, then read `docs/PLAN.md` (milestones, checklists,
decision log), `docs/RESEARCH.md` and `docs/TARGETS.md`.

## State (2026-11-22)

M0–M3 and M4 done; M5–M8 well along. Every pinned release of both games (Sacked: German CD, English,
Hungarian, Polish, Dutch, Russian; Crazy School: German CD, Russian, Polish) is unpacked by `recomp extract`,
lifted, built and reaches its main menu; the German versions of both games start their first level
(`tools/run-scenario sacked firstlevel`, `crazyschool firstlevel`). `recomp convert --system linux` makes a
native build that runs; in Firefox the converter page converts both German games and the player runs them. The
page offers the languages as flags and downloads the release from archive.org (`/cors/`). Next: play levels
through, sound and windowed/fullscreen checked by hand, CI and release packages (see `docs/PLAN.md`).

```sh
CMAKE_PREFIX_PATH=<your SDL3 prefix> cmake --preset dev && cmake --build --preset dev    # unpacks, lifts, builds
build/dev/games/sacked/sacked-sacked --game-dir orig/sacked/cd-de
build/dev/games/crazyschool/sacked-crazyschool --game-dir orig/crazyschool/cd-de
build/dev/games/sacked/sacked-sacked-pl --game-dir orig/sacked/pl     # a target per other release
tools/run-scenario sacked firstlevel                 # headless, virtual clock, compares calls and a screenshot
recomp/target/release/recomp download --dest orig sacked ru          # a release from archive.org
```

## Facts that matter

- Inputs (git-ignored, in `orig/`): the archive.org images and installers listed in `docs/TARGETS.md`. All are
  abandonware; the converter downloads them itself.
- Executables: MSVC 6 (Polish Crazy School: MSVC 7.1), base `0x400000`, no relocations, `.text` 0.3–0.5 MiB.
  DirectDraw 7, DirectInput 7, DirectSound, GDI text, Ogg Vorbis through shipped DLLs (lifted, rebased).
- Sacked links the system's `MSVCRT.dll` (and STLport, shipped, lifted): `hle/msvcrt` (with the C++ exception
  runtime and MSVCP60's `std::string`) replaces it. Crazy School links its CRT statically, but its Vorbis DLLs
  use MSVCRT too. The Dutch Sacked and the Polish Crazy School are MSVC 7.1 builds (the Dutch one with its own
  `msvcr71`/`msvcp71`, lifted).
- Scripted input: the games move their own pointer by DirectInput's relative motion, and buttons react only
  while the pointer moves over them (see `games/*/scenarios/firstlevel.input`).
- No Android build, no resolution patches (decision log).

## Conventions (same as the sister projects)

- **Share code**: one recompiler, runtime, HLE and platform layer. Game-specific behaviour goes into
  `games/<id>/` (manifest, patches), never into a copy of shared code. Keep changes general so they can go back
  to the sister projects.
- Never commit, upload or paste game files, generated C, disassembly or decompiled code. Addresses and behaviour
  described in our own words are fine.
- Every source file starts with `SPDX-License-Identifier: MIT`; no absolute home paths.
- Docs, code and comments in English (the maintainer writes in German).
- Commits in logical steps; author **and** committer `TerminShock <357108427+TerminShock@users.noreply.github.com>`
  (as in the initial commit). Never the local git identity. Messages describe the change only: no AI tools, models
  or attribution trailers (`tools/hooks/commit-msg` enforces it).
- No personal data anywhere in the repository: no real names, e-mail addresses, user names or home paths.
- Keep the docs current: tick `docs/PLAN.md`, add findings to `docs/RESEARCH.md` (unknowns **(open)**),
  decisions to the decision log, new builds to `docs/TARGETS.md` and the manifest first.
