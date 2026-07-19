# tools/hooks

Versioned git hooks (`.git/hooks/` isn't tracked by git, so hooks meant to be
shared across clones have to live somewhere else and get pointed to).

## Install (once per clone)

```sh
git config core.hooksPath tools/hooks
```

## What's here

- **pre-push** — rebuilds `build/` and `build-vk/` if any C/C++ source
  changed since diverging from `main` (blocks the push on a build failure),
  then runs `clang-tidy` (using the repo's `.clang-tidy`) on changed
  `.cpp`/`.c` files as an advisory, non-blocking check. Needs `clang-tidy`
  on `PATH` or at `/opt/homebrew/opt/llvm/bin/clang-tidy` (Homebrew's `llvm`
  is keg-only, so it's not symlinked into `PATH` by default) — if it isn't
  found, the lint step is skipped with a message rather than failing.

Skip the hook for one push (e.g. a docs-only change, or you already know
the build's fine): `git push --no-verify`.

## Why local instead of CI

No GitHub Actions pipeline for this (yet) — with no binaries hosted on
GitHub and only local builds happening today, a required CI check would
mostly be catching a scenario that doesn't exist yet. Worth revisiting at
M3, when Linux/Windows builds make "does it build on a platform I don't
have in front of me" a real question that local hooks can't answer.
