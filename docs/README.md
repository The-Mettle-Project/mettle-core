# Documentation

This repository is **libmtlc**, a reusable native compiler backend, plus
**Mettle**, the reference frontend that drives it. The docs are organized the
same way.

## The backend: libmtlc

libmtlc owns the IR, the optimizers, code generation for four targets, and
linking. Any frontend can build IR and drive it through the public C API in
[`include/mtlc/`](../include/mtlc/).

- [**Writing a frontend for libmtlc**](embedding.md): the public API end to
  end (build IR, optimize, emit code, link). Start here to use the backend.
- [Compilation](compilation.md): the pipeline, the `mettle` driver, and its
  compiler options.
- [Linker and build pipelines](linker-build-pipelines.md): the native PE/ELF
  linker and how objects become executables.
- [GPU offload](gpu.md): the PTX and SPIR-V targets.
- [ML-driven IR optimization](ml-opt.md): the GNN optimizer.
- [Translation validation](translation-validation.md): the per-pass
  correctness gate (`--verify`).
- [Profile-guided optimization](pgo.md): zero-run PGO (`--pgo`).
- [Runtime model](runtime-model.md): what libmtlc-emitted programs assume of
  the OS, and the two opt-in helper objects.
- [Diagnostics](diagnostics.md): the frontend-neutral diagnostics reporter.

The four code-generation targets are x86-64 (with AVX2), ARM64, NVIDIA PTX, and
SPIR-V (OpenCL). A second, non-Mettle frontend that exercises the whole public
API lives in [`examples/calc`](../examples/calc).

## The reference frontend: the Mettle language

Mettle is a typed, assembly-inspired systems language. These document the
*language* the reference frontend implements (see the
[language reference index](LANGUAGE.md)):

- [Lexical structure](lexical-structure.md), [Types](types.md),
  [Declarations](declarations.md), [Expressions](expressions.md),
  [Control flow](control-flow.md)
- [Modules](modules.md), [Imports](imports.md),
  [Standard library](standard-library.md)
- [Heap allocation](heap-allocation.md), [Borrow checker](borrow-checker.md),
  [C interoperability](c-interop.md)
- [Compile-time execution](testing.md) (`mettle test` / `trace`),
  [Quick reference](quick-reference.md), [Known limitations](known-limitations.md)

## Contributing

See [CONTRIBUTING.md](../CONTRIBUTING.md) for the build/test workflow and the
rules that keep the backend frontend-agnostic.
