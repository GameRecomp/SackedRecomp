# Contributing

Thanks for helping. Please read this first. Rule 1 protects the project.

## Setup

```sh
git config core.hooksPath tools/hooks    # once per clone: audit before each commit, commit message check
```

## Rules

1. **No copyrighted material, anywhere.** That covers files, commit messages, issues, PRs and CI artifacts. It
   includes anything from `orig/` or `generated/`, disassembly or decompiled listings of the games, screenshots
   or text from the games, and code copied from projects whose license does not allow it (for example
   leaked SDKs or non-free Wine forks). Describing *behaviour* in your own words (for example "function at
   `0x4a1230` reads the map header and validates the version") is fine and encouraged.
2. **No personal data or absolute machine paths** in committed files. Use paths relative to the repository root.
3. **Every source file starts with an SPDX identifier** within its first five lines:
   `SPDX-License-Identifier: MIT`. `tools/audit.sh` enforces this.
4. **Hashes, not files.** New builds are added by hash and size (see [docs/WORKFLOWS.md](docs/WORKFLOWS.md)).
5. **Keep it deterministic.** The recompiler output must not depend on the host, the time or the order of hash maps.
6. **Share code between the games.** All games use the same recompiler, runtime, HLE and platform code.
   Differences between games and builds go into `games/<id>/game.toml` or a small game module, never into a
   copy of shared code. A change to a shared rule must hold for every target, not just the one you work on.
7. **Tests go with the change.** The lifter gets difftests, the HLE gets unit tests where it can be
   tested without the game, and the manifests are covered by the consistency test.

## Style

- C: C11, `.clang-format` (LLVM base, 4 spaces, 100 columns). No compiler extensions in runtime code,
  except behind `#ifdef`. Public symbols are prefixed `rc_`. HLE entry points are named `hle_<dll>_<Function>`.
- Rust: stable toolchain, `cargo fmt`, `cargo clippy -D warnings`.
- Docs: English, short sentences, facts with sources. Open questions are marked **(open)** in
  [docs/RESEARCH.md](docs/RESEARCH.md).

## Commits and PRs

- Commit messages describe the change only. They do not mention AI tools, models, vendors or sessions, and they
  carry no attribution trailers for them. `tools/hooks/commit-msg` enforces this.
- One topic per PR. Reference the milestone (for example `M2: resolve jump tables in Cultures.exe`).
- Update the status in [docs/PLAN.md](docs/PLAN.md) when a checklist item is done.
