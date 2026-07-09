
<div align="center">

# libmtlc

**A from-scratch, frontend-agnostic compiler backend.**

Custom IR. Classical + GNN optimizers. Native codegen. Native linking.
Any frontend that can lower into the IR can drive the pipeline.

[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
&nbsp;![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux-2b6cb0.svg)
&nbsp;![Targets](https://img.shields.io/badge/targets-x86--64%20%7C%20ARM64%20%7C%20PTX-2f855a.svg)
&nbsp;![Dependencies](https://img.shields.io/badge/dependencies-no%20VM%20%C2%B7%20hand--encoded%20ISA-c53030.svg)

[**API**](include/mtlc/)
&nbsp;·&nbsp; [**Docs**](docs/)
&nbsp;·&nbsp; [**Mettle language**](docs/LANGUAGE.md)
&nbsp;·&nbsp; [**GitHub**](https://github.com/The-Mettle-Project/Mettle)

</div>

---

This repository is building **libmtlc**: a reusable native compiler backend. Frontends lower their own AST into libmtlc's IR and type model; the library owns optimize → codegen → link from there.

**Mettle** is the reference frontend in-tree — a systems language that exercises the full stack end to end. Both live here: the backend is not private to one language, and the language is not bolted onto someone else's stack.

| Layer | Role |
|-------|------|
| **libmtlc** | Backend library: IR, optimizers, codegen (x86-64 / ARM64 / PTX), PE/ELF linking. Public C headers in [`include/mtlc/`](include/mtlc/). Ships as `bin/mtlc.lib` (Windows) or `bin/libmtlc.a` (Linux). |
| **mettle** | Reference language + driver. Lowers `.mettle` into libmtlc IR, then runs the backend pipeline. |

## Why

Most new-language projects either emit C and outsource the rest, or grow a private backend only that language can use.

libmtlc is a **standalone backend** with its own type IR and module model, so multiple frontends (or experimental languages) can target the same optimizers and machine-code emitters. Hand-built instruction encodings. Own PE linker on Windows. No VM, no external assembler for the host path.

## Pipeline

```
                    your frontend                    libmtlc
              ┌─────────────────────┐    ┌──────────────────────────────┐
 source  ──►  │ parse / typecheck / │──► │ IR module (MtlcType, symbols, │
              │ lower to IR         │    │ per-inst value_type)         │
              └─────────────────────┘    │        │                     │
                                         │        ▼                     │
                                         │ classical optimizer          │
                                         │ optional GNN ML-opt          │
                                         │   (translation-validated)    │
                                         │        │                     │
                                         │        ▼                     │
                                         │ codegen: x86-64 · ARM64 · PTX│
                                         │ link:    PE (internal) · ELF │
                                         └──────────────────────────────┘
```

The backend does **not** include or call frontend headers. Codegen resolves types and symbols only from IR. Thread-local diagnostic state allows concurrent sessions from separate frontend threads.

## Public API

```c
#include <mtlc/mtlc.h>

MtlcContext *ctx = mtlc_context_create();
mtlc_context_set_opt_level(ctx, 1);
mtlc_context_set_ml_opt(ctx, 1);       /* optional GNN pass */

/* Frontend builds an IRProgram*, then hands ownership to the backend: */
MtlcModule *mod = mtlc_module_adopt_ir(ir_program);
mtlc_optimize(ctx, mod);
mtlc_apply_ml_opt(ctx, mod, NULL);

/* Codegen + link are still driven by the reference driver today;
 * the IR boundary is already frontend-free. */
void *ir = mtlc_module_ir(mod);

mtlc_module_destroy(mod);
mtlc_context_destroy(ctx);
```

Headers:

| Header | Responsibility |
|--------|----------------|
| [`mtlc.h`](include/mtlc/mtlc.h) | Umbrella |
| [`type.h`](include/mtlc/type.h) | Backend type IR (`MtlcType`) — what a frontend translates *into* |
| [`module.h`](include/mtlc/module.h) | Opaque IR module handle |
| [`context.h`](include/mtlc/context.h) | Session: opt level, ML-opt, explain, whole-program |
| [`pipeline.h`](include/mtlc/pipeline.h) | `mtlc_optimize`, `mtlc_apply_ml_opt` |
| [`target.h`](include/mtlc/target.h) | Arch / object / link enums |

## What the backend does

- **IR** — Instruction stream with backend-owned types, module symbol table, and function/global metadata. No AST after lowering.
- **Classical optimizer** — vectorization (AVX2), inlining, SROA, loop transforms, contracts (`@simd!`, `@inline!`, `@noalloc`), `--explain` decision reports.
- **ML optimizer** (`--ml-opt`) — GNN proposes rewrites; interpreter-based translation validation accepts or rejects each one. The model is never trusted alone. See [ml-opt](docs/ml-opt.md).
- **Codegen** — hand-encoded x86-64 (+ AVX2), ARM64, and NVIDIA PTX. No external assembler for the host path.
- **Link** — built-in PE linker on Windows; ELF objects on Linux (system link).
- **Debug / forensics** — source-level debug hooks, crash diagnosis, optional native-heap UAF traps.

## Reference frontend: Mettle

Mettle is a systems language that compiles to native code *through* libmtlc. It is how the backend is developed and regression-tested (hundreds of end-to-end tests).

Language highlights (full reference: [docs/LANGUAGE.md](docs/LANGUAGE.md)):

- Static types, pointers, structs, enums, closures, `defer` / `errdefer`
- C / OS interop; bundled stdlib
- Compile-time memory diagnostics (UAF, double free, leaks) without ownership annotations — [borrow checker](docs/borrow-checker.md)
- GPU kernels via native PTX — [GPU](docs/gpu.md)

```mettle
import "std/io";

fn main() -> int32 {
  print("hello from the reference frontend\n");
  return 0;
}
```

```bash
mettle --build hello.mettle -o hello
./hello   # Windows: .\hello.exe
```

## Install (Mettle driver)

**Linux (x86-64)**

```bash
curl -fsSL https://raw.githubusercontent.com/The-Mettle-Project/Mettle/main/install.sh | sh
```

**Windows (x86-64), PowerShell**

```powershell
irm https://raw.githubusercontent.com/The-Mettle-Project/Mettle/main/install.ps1 | iex
```

Installs the `mettle` driver + stdlib/runtime (not a separate libmtlc package yet). Pin with `--version v0.13.0` / `-Version v0.13.0`.

## Build from source

Produces the backend archive **and** the reference driver:

**Windows**

```powershell
.\build.bat          # bin\mtlc.lib + bin\mettle.exe
.\build.bat clang
```

**Linux**

```bash
make                 # bin/libmtlc.a + bin/mettle
make libmtlc         # backend only
```

```bash
./bin/mettle --build --release hello.mettle -o hello
```

Common flags: `--release` / `-O`, `--explain`, `--ml-opt`, `--debug-hooks`, `-d` / `-s` / `-g`. Full list: `mettle --help`.

## Repository layout

```
include/mtlc/     public C API (the stable surface of the backend)
src/
  mtlc_api.c      API implementation
  ir/             IR core, classical optimizer, GNN ML-opt, type IR
  codegen/        x86-64 · ARM64 · PTX  (backend-only; no frontend includes)
  linker/         COFF / PE
  frontend/       Mettle→libmtlc adapters (type map, symbol bake)
  lexer/ parser/ semantic/   reference frontend
  ir/ir_lower*.c  AST→IR lowering (frontend concern; not in the archive)
  main.c          driver that links against libmtlc
stdlib/           Mettle standard library
src/runtime/      optional objects linked into user programs
tests/            end-to-end suite through the reference frontend
docs/             language + tooling reference
examples/         demos and benchmarks
tools/            fuzz, benchmarks, ML training
```

## Development

```powershell
# Windows (primary CI)
.\build.bat
.\tests\run_tests.ps1
```

```bash
# Linux ELF path
make -j"$(nproc)"
bash tools/test-elf-native.sh
```

Ground rules (hand-encoded ISA only, differential fuzzer for codegen/opt changes): [CONTRIBUTING.md](CONTRIBUTING.md).

## Documentation

- [Language reference](docs/LANGUAGE.md) (Mettle)
- [Compilation & flags](docs/compilation.md)
- [ML optimizer](docs/ml-opt.md)
- [GPU / PTX](docs/gpu.md)
- [Borrow checker](docs/borrow-checker.md)
- [C interop](docs/c-interop.md)
- [Known limitations](docs/known-limitations.md)

## License

Apache-2.0. See [LICENSE](LICENSE).
