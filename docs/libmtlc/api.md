# libmtlc API reference

Everything a frontend can call lives in seven headers under
[`include/mtlc/`](../../include/mtlc/). `#include <mtlc/mtlc.h>` pulls in all of
them except `build.h`, which is included explicitly by frontends that construct
IR through the builder.

Conventions that hold across the whole API:

- **Return codes.** Functions that can fail return `int`: 1 on success, 0 on
  failure, with a human-readable message printed to `stderr`. There is no
  error-code enum and no `errno`-style state.
- **NULL tolerance.** Setters and destroyers accept NULL and do nothing.
  Getters on NULL return 0/NULL. Constructors return NULL on allocation
  failure.
- **String ownership.** Unless a function's contract says otherwise, string
  arguments are copied; the caller keeps ownership of what it passed in.
- **Type ownership.** `const MtlcType *` arguments are borrowed and must stay
  valid for the life of the module they end up in. The canonical constructors
  (`mtlc_type_scalar`, `mtlc_type_pointer`) return immortal descriptors that
  satisfy this automatically. See [The type system](types.md).
- **Thread safety.** There is no shared mutable global state in the backend
  (per-compile diagnostic state is thread-local). One thread may own one
  compilation at a time: a builder, the module it produces, and the context
  driving it must all be used from a single thread, but two threads can each
  run their own compilation concurrently.

---

## mtlc.h

Umbrella header. Includes `context.h`, `module.h`, `pipeline.h`, `target.h`,
and `type.h`.

```c
const char *mtlc_version(void);
```

Returns a static version string, e.g. `"libmtlc 0.1.0"`. Never NULL, never
freed.

---

## context.h

A backend session handle holding the knobs the pipeline reads. It carries no
per-module state; one context can drive any number of modules sequentially.

```c
MtlcContext *mtlc_context_create(void);
void mtlc_context_destroy(MtlcContext *ctx);
```

`create` returns a zero-initialized context: optimization off (`opt_level` 0),
ML-opt off, whole-program off, explain off. Returns NULL on allocation failure.

```c
void mtlc_context_set_opt_level(MtlcContext *ctx, int level);
int  mtlc_context_opt_level(const MtlcContext *ctx);
```

0 means none: `mtlc_optimize` becomes a no-op that returns success. Any value
`>= 1` runs the full classical pipeline. There are no graduated levels today;
the distinction is off/on.

```c
void mtlc_context_set_ml_opt(MtlcContext *ctx, int enabled);
int  mtlc_context_ml_opt(const MtlcContext *ctx);
```

Records the caller's intent to run the ML optimizer. Note that
`mtlc_apply_ml_opt` is a separate explicit call; this knob is informational for
drivers that decide from it.

```c
void mtlc_context_set_whole_program(MtlcContext *ctx, int enabled);
int  mtlc_context_whole_program(const MtlcContext *ctx);
```

Set this when the module will become a single executable whose only entry point
is `main`, making every call site visible. It gates whole-program transforms
whose soundness requires that property (for example allocation-site layout
factorization, which rewrites callee bodies). Leave it off when emitting a
relocatable object that other code will link against.

```c
void mtlc_context_set_explain(MtlcContext *ctx, int enabled,
                              const char *focus_file);
int  mtlc_context_explain(const MtlcContext *ctx);
const char *mtlc_context_explain_focus_file(const MtlcContext *ctx);
```

Enables optimization-decision reporting (each vectorization/inlining decision,
with a reason when declined). `focus_file`, when non-NULL, limits remarks to
locations whose filename matches it. **The string is borrowed, not copied**; it
must outlive every `mtlc_optimize` call that uses this context.

---

## module.h

An opaque unit of IR, the currency of the pipeline.

```c
MtlcModule *mtlc_module_adopt_ir(void *ir_program);
```

Wraps an already-built backend IR program (an `IRProgram *` passed opaquely so
this header stays independent of the internal IR layout). **The module takes
ownership**: destroying the module destroys the IR. Frontends using the builder
never call this; `mtlc_builder_finish` does. It exists for consumers that
construct IR against the internal `src/ir/ir.h` directly (as the Mettle
reference frontend does).

```c
void *mtlc_module_ir(MtlcModule *module);
```

Borrows the underlying `IRProgram *`. The module retains ownership. NULL for a
NULL module.

```c
size_t mtlc_module_function_count(const MtlcModule *module);
```

Number of functions with bodies in the module (0 for NULL).

```c
void mtlc_module_destroy(MtlcModule *module);
```

Destroys the module and the IR program it owns. Do not destroy a module while
any pipeline call on it is in flight.

---

## build.h

The IR builder: how a frontend constructs a module without touching internal
headers. The programming model is documented in [The IR model](ir.md); this
section is the per-function contract.

### Handles

```c
typedef struct MtlcBuilder MtlcBuilder;   /* one module under construction */
typedef struct MtlcFn      MtlcFn;        /* one function body under construction */
typedef int MtlcValue;                    /* a value handle inside one MtlcFn */
#define MTLC_NO_VALUE (-1)
```

`MtlcValue` handles are indices into a per-function table. They are only
meaningful with the `MtlcFn` that produced them and never cross functions.
`MTLC_NO_VALUE` is the "no value" sentinel: a void call's result, a void
return's operand, and every error return.

**Error latching.** The builder is designed so a lowering pass can emit
straight-line calls without checking each one: any internal failure (allocation,
bad handle, NULL argument) latches an error flag on the builder. Subsequent
calls become no-ops returning `MTLC_NO_VALUE`, and `mtlc_builder_finish`
returns NULL. Check once, at finish.

### Lifecycle

```c
MtlcBuilder *mtlc_builder_create(void);
void mtlc_builder_destroy(MtlcBuilder *builder);
MtlcModule *mtlc_builder_finish(MtlcBuilder *builder);
```

`finish` **consumes the builder** in all cases: on success it returns the
module (never call `mtlc_builder_destroy` afterwards); on failure it frees
everything and returns NULL. `destroy` is for abandoning a build you have not
finished.

At finish the builder does the bookkeeping codegen depends on: it registers
every type it saw by name in the module's type registry, and adds one module
symbol per declared function and global (with parameter/return types, extern
flags, and folded initializers). See [The IR model](ir.md#module-tables).

### Declarations

```c
MtlcFn *mtlc_builder_function(MtlcBuilder *builder, const char *name,
                              const MtlcType *return_type,
                              const char *const *param_names,
                              const MtlcType *const *param_types,
                              size_t param_count, int is_extern);
```

Declares a function. `name` and every `param_names[i]` are copied. Types are
borrowed (use canonical descriptors). With `is_extern` nonzero this declares a
body-less external symbol resolved at link time (libc functions work on the
executable path) and **returns NULL by design**; do not treat that NULL as an
error. Otherwise it returns a function builder to emit the body into.

The first non-extern function named `main` is the program entry point for
`mtlc_build_executable` and the ARM64 emitter. A `main` with exactly two
parameters is flagged as wanting `(argc, argv)` and the startup code passes
them.

```c
void mtlc_builder_global(MtlcBuilder *builder, const char *name,
                         const MtlcType *type, long long init_value,
                         int is_extern);
```

Declares a module-level global variable of a scalar type with a constant
integer initializer (`0` gives a zero-initialized global). Extern globals take
no initializer. Reference it in code with `mtlc_global_ref`.

### Values

```c
MtlcValue mtlc_fn_param(MtlcFn *fn, size_t index);
```

The value of parameter `index` (0-based). Out-of-range latches an error.
Parameters are mutable storage: `mtlc_assign` may write to them.

```c
MtlcValue mtlc_const_int(MtlcFn *fn, const MtlcType *type, long long value);
MtlcValue mtlc_const_float(MtlcFn *fn, const MtlcType *type, double value);
```

Literals. An integer literal's width and signedness are taken from its use
site (the `type` argument is currently advisory). A float literal is tagged 32
or 64 bit from `type`.

```c
MtlcValue mtlc_local(MtlcFn *fn, const char *name, const MtlcType *type);
MtlcValue mtlc_global_ref(MtlcFn *fn, const char *name);
```

`mtlc_local` declares a mutable local variable (its `DECLARE_LOCAL` records the
type by name) and returns its storage handle. Local names must be unique within
a function; the builder binds by name. `mtlc_global_ref` returns a handle to a
global declared with `mtlc_builder_global`; reads use the handle directly and
`mtlc_assign` writes through it.

### Instructions

```c
void mtlc_assign(MtlcFn *fn, MtlcValue dest, MtlcValue value);
```

Stores `value` into the storage `dest` refers to (a local, parameter, or global
reference). Assigning to a literal or temp handle is meaningless and will
produce IR that codegen rejects; assign only to storage handles.

```c
MtlcValue mtlc_binary(MtlcFn *fn, const char *op, MtlcValue lhs, MtlcValue rhs,
                      const MtlcType *result_type);
```

A binary operation producing a fresh temp. `op` is one of
`+ - * / % == != < <= > >= && || & | ^ << >>` (spelled exactly). `result_type`
is baked onto the instruction so codegen never re-derives it: it decides
integer width, signed vs unsigned division/comparison/shift, and float
arithmetic. Comparisons produce a 0/1 integer, so pass an integer
`result_type` for them.

```c
MtlcValue mtlc_unary(MtlcFn *fn, const char *op, MtlcValue operand,
                     const MtlcType *result_type);
```

`-` (negate), `!` (logical not, 0/1 result), `~` (bitwise not).

```c
MtlcValue mtlc_call(MtlcFn *fn, const char *callee, const MtlcValue *args,
                    size_t arg_count, const MtlcType *return_type);
```

Calls `callee` by name. The name resolves at code-generation time against the
module symbol table, so calls may precede the callee's declaration in build
order, and externs resolve at link time. Returns the result temp, or
`MTLC_NO_VALUE` when `return_type` is void.

```c
MtlcValue mtlc_cast(MtlcFn *fn, MtlcValue value, const MtlcType *type);
```

Converts `value` to `type`: integer width/sign changes, int/float conversions,
and int/pointer reinterpretation.

```c
MtlcValue mtlc_address_of(MtlcFn *fn, MtlcValue storage,
                          const MtlcType *pointer_type);
```

The address of a local or parameter, as a pointer value of `pointer_type`.

```c
MtlcValue mtlc_load(MtlcFn *fn, MtlcValue address, const MtlcType *elem_type);
void mtlc_store(MtlcFn *fn, MtlcValue address, MtlcValue value,
                const MtlcType *elem_type);
```

Scalar memory access through a pointer value (a pointer parameter, an
`mtlc_address_of` result, a `malloc` return, or pointer arithmetic done with
`mtlc_binary`). `elem_type` must be a scalar; its size, floatness, and
signedness are recorded on the instruction (an unsigned 32-bit load
zero-extends, a signed one sign-extends). Array indexing is pointer
arithmetic: scale the index by the element size and add it to the base.

### Control flow

```c
void mtlc_label(MtlcFn *fn, const char *label);
void mtlc_jump(MtlcFn *fn, const char *label);
void mtlc_branch_if_zero(MtlcFn *fn, MtlcValue cond, const char *label);
void mtlc_return(MtlcFn *fn, MtlcValue value);
```

Labels are function-scoped strings; a jump or branch may target a label defined
later. `mtlc_branch_if_zero` transfers control to `label` when `cond` is zero
and falls through otherwise; it is the only conditional primitive (frontends
compose `if`/`else`/`while` from it, see [The IR model](ir.md#control-flow)).
`mtlc_return` with `MTLC_NO_VALUE` is a void return. **Every path through a
function body must reach a return**; the builder does not synthesize one.

---

## pipeline.h

The stages. All take the context first; where a stage does not yet read the
context that is documented rather than pretended otherwise.

```c
int mtlc_optimize(MtlcContext *ctx, MtlcModule *module);
```

Runs the classical optimizer using the context knobs (`opt_level`,
`whole_program`, `explain`, `explain_focus_file`). With `ctx` non-NULL and
`opt_level <= 0` it is a successful no-op. A NULL `ctx` optimizes with
conservative defaults (on, not whole-program, no explain). Returns 0 on error;
the practical error today is a violated optimization contract, already reported
to stderr. See [The pipeline](pipeline.md#the-classical-optimizer) for what
actually runs.

```c
typedef struct {
  int proposals;   /* rewrites the model proposed */
  int validated;   /* proposals that passed differential validation */
  int proven;      /* proposals proven equivalent */
  int rejected;    /* proposals rejected with a counterexample */
  int skipped;     /* proposals not attempted */
} MtlcMlOptStats;

int mtlc_apply_ml_opt(MtlcContext *ctx, MtlcModule *module,
                      MtlcMlOptStats *stats);
```

Runs the GNN-driven optimizer behind its translation-validation gate, then
hoists constants. The caller decides when to run it (typically after
`mtlc_optimize`); `ctx` is currently unused. `stats` may be NULL. Requires a
model to be present; default builds ship none, in which case the pass proposes
nothing and succeeds.

```c
int mtlc_emit_object(MtlcContext *ctx, MtlcModule *module, const char *path);
```

Generates native x86-64 code for the whole module and writes a relocatable
object in the host format: COFF on Windows, ELF elsewhere. `ctx` is currently
unused. Optimize first if you want optimized code.

```c
int mtlc_emit(MtlcContext *ctx, MtlcModule *module, MtlcArch arch,
              const char *path);
```

Lowers the module for `arch` and writes that target's natural product:

| `arch` | Product written to `path` |
|---|---|
| `MTLC_ARCH_X86_64` | host-format relocatable object (same as `mtlc_emit_object`) |
| `MTLC_ARCH_ARM64` | self-contained static AArch64 ELF **executable** (`_start` calls `main`, exits with its return value) |
| `MTLC_ARCH_PTX` | NVIDIA PTX module (text), one `.visible .entry` per function |
| `MTLC_ARCH_SPIRV` | SPIR-V binary module (OpenCL 1.2), one kernel entry point per function |

The ARM64, PTX, and SPIR-V paths consume the **unoptimized** IR shape: emit
before calling `mtlc_optimize` on that module. Per-target subsets and
constraints are in [The pipeline](pipeline.md#the-code-generators).

```c
int mtlc_build_executable(MtlcContext *ctx, MtlcModule *module,
                          const char *output_path);
```

The whole back half in one call: emits a temporary object, synthesizes the
C-runtime startup that calls the module's `main`, links, and deletes the
temporaries. On Windows this uses libmtlc's own PE linker with imports resolved
by DLL name from `kernel32.dll`, `ucrtbase.dll`, and `msvcrt.dll` (no Windows
SDK import libraries needed), so extern libc calls like `malloc` and `putchar`
just work. On ELF hosts it invokes the system C compiler (`cc -no-pie`) to
link. Returns 1/0; failures print to stderr.

---

## target.h

```c
typedef enum { MTLC_ARCH_X86_64, MTLC_ARCH_ARM64,
               MTLC_ARCH_PTX, MTLC_ARCH_SPIRV } MtlcArch;
typedef enum { MTLC_OBJ_COFF, MTLC_OBJ_ELF } MtlcObjectFormat;
typedef enum { MTLC_LINK_PE, MTLC_LINK_ELF } MtlcLinkTarget;

MtlcObjectFormat mtlc_host_object_format(void);  /* COFF on Windows, else ELF */
MtlcLinkTarget   mtlc_host_link_target(void);    /* PE on Windows, else ELF  */
const char *mtlc_arch_name(MtlcArch arch);       /* "x86-64", "arm64", ...   */
```

---

## type.h

The backend type descriptor and its canonical constructors. Documented in full
in [The type system](types.md); the short version:

```c
const MtlcType *mtlc_type_scalar(MtlcTypeKind kind);
const MtlcType *mtlc_type_pointer(const MtlcType *base);
```

Both return immortal, immutable, canonical descriptors that never need freeing
and satisfy every "must outlive the module" requirement in this API. Scalars
cover `MTLC_TYPE_INT8` through `MTLC_TYPE_VOID`; `mtlc_type_scalar` returns
NULL for kinds that need caller-supplied layout (structs, arrays, tagged
enums). Pointers intern: the same `base` returns the same descriptor, and
pointer-to-pointer chains compose.

```c
int mtlc_type_is_integer(const MtlcType *t);
int mtlc_type_is_unsigned(const MtlcType *t);
int mtlc_type_is_float(const MtlcType *t);
int mtlc_type_is_aggregate(const MtlcType *t);
size_t mtlc_type_size(const MtlcType *t);
size_t mtlc_type_alignment(const MtlcType *t);
const char *mtlc_type_kind_name(MtlcTypeKind kind);
```

Classification and layout queries; all NULL-safe (returning 0).

---

## Linking a frontend

```bash
# Windows (after .\build.bat)
gcc -Iinclude my_frontend.c bin/mtlc.lib -o my_frontend.exe -ldbghelp

# Linux (after `make libmtlc`)
cc -Iinclude my_frontend.c bin/libmtlc.a -o my_frontend
```

`-ldbghelp` on Windows satisfies the crash reporter's stack-walk imports. The
library is self-contained beyond system libraries; the `libmtlc_selfcontained`
suite gate enforces that its symbol closure never reaches into frontend code.
