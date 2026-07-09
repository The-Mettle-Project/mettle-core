# Writing a frontend for libmtlc

libmtlc is a standalone compiler backend. Any frontend can lower its own
language into libmtlc's IR and drive the pipeline — custom IR, the classical +
GNN optimizers, native x86-64 / ARM64 / PTX / SPIR-V codegen, and native PE/ELF
linking — through the public C API in [`include/mtlc/`](../include/mtlc). Your
frontend includes only those headers and links only `bin/mtlc.lib` (Windows) or
`bin/libmtlc.a` (Linux). It never touches a backend-internal header.

The reference Mettle frontend is one consumer; [`examples/calc`](../examples/calc)
is a second, deliberately unrelated one — a tiny C-like language in a single
file. This document walks the same path.

## The public surface

| Header | What it gives you |
|--------|-------------------|
| [`mtlc/type.h`](../include/mtlc/type.h) | `MtlcType`, the backend type descriptor, and `mtlc_type_scalar()` |
| [`mtlc/build.h`](../include/mtlc/build.h) | the IR builder: functions, values, instructions, control flow |
| [`mtlc/module.h`](../include/mtlc/module.h) | `MtlcModule`, an opaque unit of IR |
| [`mtlc/context.h`](../include/mtlc/context.h) | `MtlcContext`, a backend session holding the optimization knobs |
| [`mtlc/pipeline.h`](../include/mtlc/pipeline.h) | `mtlc_optimize`, `mtlc_apply_ml_opt`, `mtlc_emit_object`, `mtlc_build_executable` |
| [`mtlc/target.h`](../include/mtlc/target.h) | architecture / object-format / link-target enums |

## 1. Build IR

Create a builder, declare functions, and emit an instruction stream. Values are
opaque `MtlcValue` handles; control flow is explicit labels and branches (your
frontend lowers its own `if`/`while`/`for`).

```c
#include <mtlc/build.h>

const MtlcType *i64 = mtlc_type_scalar(MTLC_TYPE_INT64);
MtlcBuilder *b = mtlc_builder_create();

/* fn add(a, b) { return a + b; } */
const char *pn[] = {"a", "b"};
const MtlcType *pt[] = {i64, i64};
MtlcFn *add = mtlc_builder_function(b, "add", i64, pn, pt, 2, /*extern=*/0);
MtlcValue sum = mtlc_binary(add, "+", mtlc_fn_param(add, 0),
                            mtlc_fn_param(add, 1), i64);
mtlc_return(add, sum);

/* fn main() { return add(40, 2); } */
MtlcFn *m = mtlc_builder_function(b, "main", i64, NULL, NULL, 0, 0);
MtlcValue args[] = {mtlc_const_int(m, i64, 40), mtlc_const_int(m, i64, 2)};
mtlc_return(m, mtlc_call(m, "add", args, 2, i64));

MtlcModule *module = mtlc_builder_finish(b);  /* consumes the builder */
```

`mtlc_builder_finish` populates the module's type registry and symbol table (the
tables codegen reads) from the types you declared, then hands back a module.
Calls resolve by name, so functions may be defined in any order.

## 2. Optimize

```c
#include <mtlc/pipeline.h>

MtlcContext *ctx = mtlc_context_create();
mtlc_context_set_opt_level(ctx, 1);
mtlc_context_set_whole_program(ctx, 1);   /* single-exe: every call site visible */
mtlc_optimize(ctx, module);
/* optional: mtlc_context_set_ml_opt(ctx, 1); mtlc_apply_ml_opt(ctx, module, NULL); */
```

## 3. Emit code

Emit a relocatable object, or go all the way to a native executable (on Windows
this uses libmtlc's own internal PE linker — no external toolchain):

```c
mtlc_build_executable(ctx, module, "a.exe");   /* or mtlc_emit_object(ctx, module, "a.o") */

mtlc_module_destroy(module);
mtlc_context_destroy(ctx);
```

## Build your frontend

```bash
# Windows, after .\build.bat
gcc -Iinclude my_frontend.c bin/mtlc.lib -o my_frontend.exe -ldbghelp

# Linux, after `make libmtlc`
cc -Iinclude my_frontend.c bin/libmtlc.a -o my_frontend
```

## Scope of the builder today

`mtlc/build.h` covers the imperative scalar core: functions, parameters, `var`
locals, assignment, integer/float arithmetic and comparisons, calls, and label
/ branch control flow — enough for a real language (see `examples/calc`).
Pointers, arrays, and aggregates exist in `MtlcType` and the IR, but the builder
does not yet expose load/store/GEP-style helpers for them; a frontend that needs
those constructs today drops to the IR directly. Extending the builder to cover
them is additive and does not change the surface above.

See also: [compilation pipeline](compilation.md), [GPU offload](gpu.md).
