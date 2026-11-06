# Contributing to wintomac

Thanks for helping. wintomac turns x86-64 Windows programs into native arm64 macOS apps, and most of the work
left is making more real programs run. You don't need to know compilers to help.

## Good ways to start

* **Try a program and report what happens.** Free and open-source Windows games are ideal (Quake/Doom source
  ports, open-source engines, demos). Open an issue with the program, where you got it, the command you ran, and
  the last lines of output. `WINTOMAC_TRACE=1` logs every Windows API call; `wintomac_misses.txt` lists code the
  analysis missed.
* **Missing Windows APIs.** A run that stops on an unimplemented import names it. Shims live in `runtime/`
  (`kernel32.c`, `user32.c`, `win_misc.c`, ...). Add the function, plus a small test in `tests/src/` if it has
  behaviour worth checking.
* **Instructions.** The lifter (`recomp/lifter.py`, `recomp/vector.py`) logs unsupported instructions in
  `report.json` (`unsupported_mnemonics`). Each needs a lifting rule and a test in `tests/src/` (`avx_*`, `jit_*`).
* **Docs.** If a step in the README didn't work for you, fix it.

## Building and testing

See the Quick start in the README. Before sending a pull request:

```sh
make check            # cross-compiles the tests with MinGW, recompiles them, runs them, diffs every result
```

Each test's output must match the native build byte for byte. On a 16 GB Mac, limit parallel jobs:
`WINTOMAC_JOBS=3 make -j3 check`.

## Rules

* **No game files, ever:** no executables, data, ROMs or dumps of commercial software in the repository, issues
  or pull requests. Point to where people can get a program legally instead. `tools/publish_audit.sh` checks the tree.
* **No keys, DRM or anti-cheat bypasses.**
* **Credit what you use.** If your change uses or was inspired by another project, add it to `CREDITS.md` with its licence.
* Keep pull requests focused: one fix or feature, with a test when behaviour changes.

By contributing you agree that your contribution is licensed under the MIT licence (see `LICENSE`).
