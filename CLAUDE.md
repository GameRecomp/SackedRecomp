# SackedRecomp – orientation for a new working session

Static recompilation of Limbic Entertainment's two 2D prank games, *Sacked: Your Last Day* (German:
*Gefeuert! – Dein letzter Tag*, 2003) and *Crazy School* (*Schulverweis*, 2003/04), into portable C on an
SDL3 runtime. Fourth project on the toolchain of `../TropicoRecomp`, `../CulturesRecomp` and `../HoMM4Recomp`;
HoMM4Recomp is the template for all shared code. Start here, then read `docs/PLAN.md` (milestones, checklists,
decision log), `docs/RESEARCH.md` and `docs/TARGETS.md`.

## State (2026-11-22)

M0 and M1 done: the shared code is in, both games' manifests pin every release that is known (Sacked: German
CD, English, Hungarian, Polish, Dutch, Russian; Crazy School: German CD, Russian, Polish), and `recomp extract`
unpacks every medium (raw CD images, NSIS, Inno Setup 3.0/4.2, Windows Installer + cabinet). Next: M2 (discover).

```sh
CMAKE_PREFIX_PATH=<your SDL3 prefix> cmake --preset dev && cmake --build --preset dev    # unpacks, lifts, builds
build/dev/games/sacked/sacked-sacked --game-dir orig/sacked/cd-de
build/dev/games/crazyschool/sacked-crazyschool --game-dir orig/crazyschool/cd-de
```

## Facts that matter

- Inputs (git-ignored, in `orig/`): the archive.org images and installers listed in `docs/TARGETS.md`. All are
  abandonware; the converter downloads them itself.
- Executables: MSVC 6 (Polish Crazy School: MSVC 7.1), base `0x400000`, no relocations, `.text` 0.3–0.5 MiB.
  DirectDraw 7, DirectInput 7, DirectSound, GDI text, Ogg Vorbis through shipped DLLs (lifted, rebased).
- Sacked links the system's `MSVCRT.dll` (and STLport, shipped, lifted): `hle/msvcrt` with the C++ exception
  runtime is the new piece. Crazy School links its CRT statically.
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
