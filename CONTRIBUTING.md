# Contributing

This repository is **libmtlc**, a from-scratch native compiler backend: custom
IR, classical + GNN optimizers, hand-encoded x86-64 / ARM64 / PTX / SPIR-V
codegen, and its own PE/ELF linker. No LLVM, no VM, no managed runtime.
**Mettle** is the reference frontend: a systems language that lowers into the
backend IR and exercises the whole stack. This guide covers how to build, test,
and land changes to either half.

## Ground rules

The whole point of libmtlc is that it emits machine code itself. Never route
codegen through LLVM, Cranelift, or GNU `as`, not even as a reference oracle.
Instructions are hand-built from the ISA. `ptxas` (for the PTX/GPU path),
`spirv-val` (for SPIR-V), and QEMU (for AArch64 testing) are the only accepted
external tools.

The backend stays frontend-agnostic. `bin/mtlc.lib` must never reference the
Mettle frontend: no backend TU may include a parser/semantic/AST header, and
the `libmtlc_selfcontained` test gate fails the suite if the library's symbol
closure reaches into driver/frontend code. New backend capabilities are reached
through the public API in [`include/mtlc/`](include/mtlc/); if you add one,
extend that API and prove it from a non-Mettle caller (see the `public_api`
gate and [`examples/calc`](examples/calc)).

Correctness comes first. A miscompile is the worst possible bug, so any change to
the IR, optimizer, register allocator, or a code generator must keep debug and
release output in agreement (see the differential fuzzer below).

The code stays in C99. Everything under `src/` builds with `-std=c99 -Wall
-Wextra` and must compile clean under both GCC and Clang, on Windows and Linux.

## Repository layout

```
include/mtlc/   public libmtlc API (frontend-agnostic): type, build, pipeline, ...
src/
  ir/           libmtlc: IR core, classical + GNN optimizer, type IR
  codegen/      libmtlc: x86-64 · ARM64 · PTX · SPIR-V (no frontend includes)
  linker/       libmtlc: COFF/PE + ELF linking
  error/        libmtlc: frontend-neutral diagnostics reporter
  mtlc_*.c      libmtlc: public API, IR builder, self-containment fallbacks
  lexer/ parser/ semantic/   Mettle reference frontend
  frontend/     Mettle→libmtlc lowering adapters
  main.c        the `mettle` driver that drives the frontend + backend
  runtime/      optional helper objects linked into user programs (crash traces, ...)
examples/       runnable samples; examples/calc is a second, non-Mettle frontend
stdlib/         Mettle standard library (.mettle modules)
tests/          regression tests; run_tests.ps1 on Windows
tools/          ELF tests, benchmarks, fuzz scripts, ML optimizer
docs/           backend + reference-frontend documentation
```

Two build products come out of one tree: `bin/mtlc.lib` / `bin/libmtlc.a` (the
backend, linkable by any frontend) and `bin/mettle(.exe)` (the reference driver,
= frontend + backend).

## Building

On Windows, the primary target, build with GCC or Clang via MinGW-w64 or LLVM:

```powershell
.\build.bat                 # default: gcc
.\build.bat clang
.\build.bat --skip-tests    # build only
```

`build.bat` starts from a clean `obj\` tree, compiles every module, archives the
backend into `bin\mtlc.lib`, links `bin\mettle.exe`, and bundles `stdlib/` and
the runtime objects into `bin\`. Run it from PowerShell.

On Linux or macOS the toolchain builds on the host, and codegen still targets
x86-64 Windows and Linux:

```bash
make -j"$(nproc)"           # bin/mettle + bundled stdlib/ and runtime/
make libmtlc                # backend only -> bin/libmtlc.a
make install                # optional: /usr/local
make clean
```

Both builds use `-O2 -g -fno-omit-frame-pointer`. When you add a new `.c` file,
register it in both `build.bat` and the `Makefile`, and put it on the correct
side of the split (a backend file goes into the `mtlc.lib` archive and must not
pull in frontend headers). Files under `src/ir/` and `src/ir/optimizer/` are
picked up by wildcard automatically.

## Testing

Every change must pass the regression suite before it goes up for review.

On Windows, which is what CI runs in full:

```powershell
.\build.bat                          # builds + runs tests
.\tests\run_tests.ps1                # tests against the current bin\mettle.exe
.\tests\run_tests.ps1 -BuildCompiler # rebuild first, then test
```

On Linux, exercising the ELF backend:

```bash
make -j"$(nproc)"
bash tools/test-elf-native.sh
make test                            # crash-handler unit test
```

Three gates specifically guard the backend/frontend boundary and run as part of
the suite: `libmtlc_selfcontained` (the library references no frontend symbol),
`calc_frontend` (a non-Mettle frontend compiles + runs against the library
alone), and `public_api` (the full `include/mtlc/` surface: the builder and all
four targets, end to end). Keep them green.

### Differential fuzzer (required for codegen/optimizer changes)

Any change touching the IR, optimizer, register allocator, or a code generator
must be run through the debug-vs-release miscompile fuzzer in `tools/fuzz/`. It
compiles generated programs at `-O0` and `--release` and diffs the results;
`METTLE_SKIP_PASS` bisects which pass introduced a divergence. A single silent
miscompile that escapes review is worse than a missed optimization. The nightly
workflow runs the heavy sweep; run a local pass on anything that changes
generated code.

## Making changes

### Optimizer / codegen (libmtlc)

New recognizers and rewrites are easy to write and easy to silently rot when an
unrelated pass reorders IR, so guard exact-shape recognizers with an IR-match
assertion on the benchmark source; that way drift is caught, not discovered
later. Vectorizers and aggressive rewrites run under `--release`, so prove their
soundness preconditions (for example, that the loop starts at zero) rather than
assuming them. Use `--explain` and `--simd-report` to confirm a loop actually
became what you intended, and `--verify` (per-pass translation validation) to
catch a pass that changed program meaning.

### Reference frontend (Mettle)

Prefer inference over annotations, since the borrow checker and capability
analysis are pure inference by design and should not grow `@owned` /
`@borrow`-style ownership annotations. `stdlib/` is written in Mettle; Windows
is the most complete target, so keep the Linux shims (`.linux` variants,
`std/net_posix`, and similar) working where they exist.

## Code style

Match the surrounding code: same naming, same idiom, same comment density.
Comment minimally, defaulting to zero comments and adding one only to explain a
non-obvious why, with no docstrings, no what-comments, and no task references.
In Mettle source, types are explicit: `var` and function-local `const` need
explicit types and nothing is inferred, though range-`for` induction variables
and global `const` are exempt.

## Submitting a pull request

Branch off `development`, which is where PRs land; `main` is the release branch.
Keep the change focused, and let unrelated cleanups go in their own PR. Build
clean under both GCC and Clang on your platform, run `tests\run_tests.ps1` on
Windows, and for codegen changes run the differential fuzzer. When you open the
PR, CI runs Windows GCC (full suite plus fuzz gate), Windows Clang
(compile-only), Linux GCC/Clang (ELF backend), and Linux ASan/UBSan, and every
job must be green. Describe the change, the platforms you tested, and, for
optimizer or codegen work, how you verified correctness (fuzzer, `--verify`,
benchmark numbers).

## Reporting bugs

Open an issue with a minimal reproducer (a `.mettle` program, or a small
frontend that builds IR through `include/mtlc/`), the exact command line
(including `--release` and any flags), the platform and host compiler, and
expected vs actual output. For a suspected miscompile, note whether it
reproduces at `-O0`, `--release`, or both. That alone narrows it down enormously.

## License

By contributing you agree that your contributions are licensed under Apache-2.0,
the same license as the project. See [LICENSE](LICENSE).
