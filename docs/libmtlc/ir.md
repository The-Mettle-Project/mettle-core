# The libmtlc IR model

This is the contract between a frontend and the backend: what a module is made
of, what each instruction means, the rules control flow must follow, and the
shape each consumer of the IR expects. It is written against the builder API
(`mtlc/build.h`); the internal C structs behind it are not part of the public
surface.

## Shape of a module

A module is:

- an ordered list of **functions**, each a linear instruction stream (there is
  no nesting; basic blocks are implied by labels and terminators);
- a **type registry**: a name-to-descriptor table (`"int64"` maps to the int64
  descriptor, `"float32*"` to the pointer descriptor) that code generators use
  to resolve any type name the IR carries;
- a **module symbol table**: one entry per function and global with its kind,
  type signature, extern flag, link name, and (for globals) the folded
  constant initializer.

`mtlc_builder_finish` populates both tables from what you declared; a frontend
never fills them by hand.

## Values

A function body computes over two kinds of values:

- **Temporaries.** Every value-producing instruction (`mtlc_binary`,
  `mtlc_call`, `mtlc_load`, ...) defines a fresh single-assignment temporary.
  Temps are not storage: you cannot assign to them, and their handle is only
  the name of that one computed value.
- **Storage.** Parameters, locals (`mtlc_local`), and global references
  (`mtlc_global_ref`) name mutable slots. Reading the handle reads the current
  value; `mtlc_assign` writes it. Storage is how values cross basic blocks:
  the IR has no phi nodes, so a value needed after a branch merge must live in
  a local (the optimizer and register allocator recover registers from that
  shape).

Literals (`mtlc_const_int`, `mtlc_const_float`) are values too; they may be
used any number of times.

All handles are scoped to their `MtlcFn`. Nothing value-like crosses function
boundaries; cross-function data flows through calls, globals, and memory.

## The instruction set

What the builder emits, in terms of the semantics codegen implements:

| Builder call | Meaning |
|---|---|
| `mtlc_local(name, T)` | declare a mutable local of type `T` (recorded by type name; resolved via the registry) |
| `mtlc_assign(dest, v)` | store `v` into the storage `dest` names |
| `mtlc_binary(op, a, b, T)` | `t = a op b` with result type `T` baked on the instruction |
| `mtlc_unary(op, a, T)` | `t = op a` (`-`, `!`, `~`) |
| `mtlc_call(f, args, n, R)` | `t = f(args...)`; name resolved against the module symbol table |
| `mtlc_cast(v, T)` | `t = (T) v` (width/sign change, int/float, int/pointer) |
| `mtlc_address_of(s, PT)` | `t = &s` for a local or parameter, typed `PT` |
| `mtlc_load(p, E)` | `t = *(E *)p`; element size/floatness/signedness recorded |
| `mtlc_store(p, v, E)` | `*(E *)p = v` |
| `mtlc_label(L)` | define label `L` here |
| `mtlc_jump(L)` | unconditional transfer to `L` |
| `mtlc_branch_if_zero(c, L)` | transfer to `L` when `c == 0`, else fall through |
| `mtlc_return(v)` | return `v` (or nothing with `MTLC_NO_VALUE`) |

Semantics worth spelling out:

- **Result types drive selection.** `mtlc_binary`'s `result_type` decides
  everything the instruction encoder needs: operand width, signed vs unsigned
  `/ % < <= > >=` and `>>`, and float vs integer arithmetic. The backend never
  re-infers types from context, so a wrong `result_type` is a silent
  miscompile, not an error.
- **Comparisons produce 0/1 integers.** `==`,`!=`,`<`,`<=`,`>`,`>=` yield an
  integer 0 or 1 (give them an integer result type). `&&` and `||` here are
  **bitwise** over those 0/1 values, not short-circuit: both operands are
  always evaluated. A frontend that needs C-style short-circuit evaluation
  lowers it with branches (pattern below).
- **Integer overflow wraps.** Arithmetic is native two's complement; there are
  no traps and no undefined-overflow assumptions in the backend.
- **Division by zero** is whatever the target does (a fault on the CPU
  targets). The backend inserts no checks; frontends that promise checks emit
  them as IR.
- **Loads extend by element signedness.** A `uint8/16/32` load zero-extends
  into the 64-bit temp; a signed load sign-extends. That is why `mtlc_load`
  takes the element type rather than a byte count.
- **Pointers are integers with a type.** Pointer arithmetic is ordinary
  integer arithmetic on the address value; scale indexes by the element size
  yourself. `mtlc_cast` converts between pointer and integer freely.

## Control flow

The rules the instruction stream must satisfy:

1. **Labels are function-scoped** and must be unique within the function.
   Forward references are fine (jump first, define later).
2. **A label starts a basic block.** Falling into a label from the preceding
   instruction is allowed and means what it looks like.
3. **`mtlc_branch_if_zero` is the only conditional.** Taken on zero, falls
   through on nonzero.
4. **Every path must end at `mtlc_return`.** The builder does not synthesize
   returns, and a body that can run off the end is invalid. The cheap
   guarantee is an unconditional `mtlc_return` at the end of lowering (dead if
   unreachable; the optimizer removes it).

### Lowering patterns

The three structures every frontend needs, in the exact shape the reference
frontend and `examples/calc` emit. `if`/`else`:

```
    branch_if_zero cond -> L_else
    ...then body...
    jump L_end
L_else:
    ...else body...
L_end:
```

`while`:

```
L_top:
    cond = ...recompute condition...
    branch_if_zero cond -> L_end
    ...body...
    jump L_top
L_end:
```

Short-circuit `a && b` as a value (evaluate `b` only when `a` is nonzero):

```
    r = local int
    assign r, 0
    branch_if_zero a -> L_done
    t = (b != 0)
    assign r, t
L_done:
    ...use r...
```

`break`/`continue` are jumps to the loop's `L_end`/`L_top`. Arbitrary CFGs are
legal (the IR does not require structured control flow); reducibility is only
required in practice because every consumer handles what structured lowering
produces, and irreducible graphs are untested territory.

## Module tables

At `mtlc_builder_finish`:

- Every distinct `MtlcType` that passed through the builder is registered in
  the **type registry** under its canonical name (plus all scalar names, always).
  This is how a `DECLARE_LOCAL` carrying `"int64*"` or a parameter list carrying
  `"float32"` resolves to layout during code generation.
- Each declared function becomes a **module symbol** with kind, extern flag,
  return type, and parameter types. Each global becomes a variable symbol with
  its type and folded initializer. Code generators read this table to emit
  global storage, classify call arguments, and decide what is defined here
  versus imported.

Calls resolve by name against this table at code-generation time. A call to a
name with no symbol and no body is an error at emit time, not at build time.

## What each consumer requires

**The optimizer** (`mtlc_optimize`) accepts anything the builder produces and
preserves these rules. Run it only for the x86-64 path.

**x86-64 codegen** consumes optimized or unoptimized IR. This is the only
target with full coverage of the instruction set above, including
`mtlc_address_of` and mixed float/integer bodies.

**ARM64, PTX, and SPIR-V consume the unoptimized shape**: the vectorizer and
loop rewrites introduce x86-specific SIMD instructions these emitters do not
implement, so emit for them before optimizing (or build a separate module).
Their subsets:

| | ARM64 | PTX / SPIR-V |
|---|---|---|
| integers, arithmetic, compares, casts | yes | yes |
| labels/jumps/branches, calls, recursion | yes | intra-kernel CF yes; calls only to the intrinsic set |
| floats | no | yes (f32/f64) |
| pointers, load/store | no | yes (the kernel model) |
| `mtlc_address_of` | no | no |
| every function becomes | a linked function (entry `main`) | a GPU kernel entry point |

The GPU emitters additionally recognize the intrinsic call names documented in
[the pipeline reference](pipeline.md#the-code-generators): the `gpu_*`
thread-index set, `gpu_barrier`, the f32 math set, `h2f`/`f2h`, and the
unsigned atomics.

## Diagnostics attached to IR

Instructions carry an optional source location (file/line/column) used by
debug info, optimization remarks, and the compile-time interpreter's error
reports. IR built through the public builder currently carries none, so those
features degrade gracefully (remarks without positions); a future builder
revision will expose location tagging.
