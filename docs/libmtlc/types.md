# The libmtlc type system

The backend owns its own type descriptor, `MtlcType` (in
[`include/mtlc/type.h`](../../include/mtlc/type.h)). Frontends translate their
types into it at the IR boundary; the code generators read nothing else. It is
a plain value struct with no methods and no hierarchy: kind, layout, and
composition pointers.

## Kinds

```c
typedef enum {
  MTLC_TYPE_INT8,  MTLC_TYPE_INT16,  MTLC_TYPE_INT32,  MTLC_TYPE_INT64,
  MTLC_TYPE_UINT8, MTLC_TYPE_UINT16, MTLC_TYPE_UINT32, MTLC_TYPE_UINT64,
  MTLC_TYPE_BOOL,
  MTLC_TYPE_FLOAT32, MTLC_TYPE_FLOAT64,
  MTLC_TYPE_STRING,
  MTLC_TYPE_FUNCTION_POINTER,
  MTLC_TYPE_POINTER, MTLC_TYPE_ARRAY,
  MTLC_TYPE_STRUCT, MTLC_TYPE_ENUM, MTLC_TYPE_TAGGED_ENUM,
  MTLC_TYPE_VOID
} MtlcTypeKind;
```

The scalar kinds, with the layout the canonical constructors give them and the
signedness the instruction selectors assume:

| Kind | Size | Align | Class |
|---|---|---|---|
| `INT8` / `INT16` / `INT32` / `INT64` | 1/2/4/8 | = size | signed integer |
| `UINT8` / `UINT16` / `UINT32` / `UINT64` | 1/2/4/8 | = size | unsigned integer |
| `BOOL` | 1 | 1 | unsigned integer (0/1) |
| `FLOAT32` / `FLOAT64` | 4/8 | = size | IEEE-754 binary32/64 |
| `POINTER` / `FUNCTION_POINTER` | 8 | 8 | unsigned address |
| `STRING` | 8 | 8 | pointer-sized handle (see caveat below) |
| `VOID` | 0 | 1 | only as a return type |

Signedness matters: it selects signed vs unsigned division, remainder,
comparison, and right-shift, and whether a narrow load sign- or zero-extends.
`mtlc_type_is_unsigned` reports the classification the backend uses.

**The `STRING` caveat.** `MTLC_TYPE_STRING` is the Mettle frontend's string
type: an 8-byte handle whose heap layout and helper kernels are defined by that
frontend's lowering. A foreign frontend should model its strings explicitly
(for example `mtlc_type_pointer(mtlc_type_scalar(MTLC_TYPE_UINT8))` for a
C-style byte pointer) rather than borrowing this kind.

## The descriptor

```c
typedef struct MtlcType {
  MtlcTypeKind kind;
  const char *name;            /* canonical, parseable name; may be NULL */
  size_t size, alignment;      /* bytes */
  MtlcAddressSpace address_space; /* POINTER only */

  struct MtlcType *base_type;  /* pointee (POINTER) / element (ARRAY) */
  size_t array_size;           /* element count (ARRAY) */

  /* FUNCTION_POINTER signature */
  struct MtlcType **fn_param_types;
  size_t fn_param_count;
  struct MtlcType *fn_return_type;
  struct MtlcType *closure_env; /* synthesized capture record, else NULL */

  /* STRUCT layout */
  const char **field_names;
  struct MtlcType **field_types;
  size_t *field_offsets;
  size_t field_count;

  /* TAGGED_ENUM layout */
  const char **tagged_variant_names;
  int *tagged_variant_tags;
  struct MtlcType **tagged_variant_payloads;
  size_t tagged_variant_count;
  size_t tagged_data_offset, tagged_data_size;
} MtlcType;
```

Descriptors are **immutable after construction** as far as the backend is
concerned; nothing in libmtlc writes to one.

## Canonical constructors and the immortality contract

Everything in the API that accepts a `const MtlcType *` **borrows** it, and the
module's type registry stores the pointer **by reference and never frees it**.
So every descriptor you hand the backend must outlive every module that saw it.
The canonical constructors make that trivial:

```c
const MtlcType *mtlc_type_scalar(MtlcTypeKind kind);
const MtlcType *mtlc_type_pointer(const MtlcType *base);
const MtlcType *mtlc_type_pointer_in(const MtlcType *base,
                                     MtlcAddressSpace address_space);
```

- `mtlc_type_scalar` returns a static singleton per scalar kind: immortal,
  shared, correctly sized and named (`"int64"`, `"float32"`, ...). It returns
  NULL for kinds needing caller-supplied layout (`STRUCT`, `ARRAY`,
  `TAGGED_ENUM`, `ENUM`, `FUNCTION_POINTER`, and raw `POINTER`; build pointers
  with `mtlc_type_pointer`).
- `mtlc_type_pointer` creates a generic pointer. `mtlc_type_pointer_in` creates
  a pointer in generic, global, workgroup, constant, or private device memory.
  Pointer descriptors intern by `(base, address_space)`: the first request
  allocates a descriptor that lives for the rest of the process, and every
  later identical request returns the same pointer. Chains compose
  (`mtlc_type_pointer(mtlc_type_pointer(i64))` is `int64**`). The intern cache
  is thread-local, upholding the backend's no-shared-mutable-globals rule;
  descriptors created on different threads for the same pointee are distinct
  pointers but interchangeable in meaning.

Call them at any time, in any order, from the thread doing that compilation,
and never free the results.

## Names must parse

The IR carries types **by name** in two places: a local declaration and a cast
record the type's name, and function signatures record parameter/return type
names. Code generators resolve those names through the module type registry.
The builder registers every descriptor it saw under `type->name` (falling back
to the kind name), so:

- canonical scalars register as `"int8"` ... `"void"`;
- generic pointers register as `"<base>*"` (`"int64*"`, `"float32**"`);
  explicitly spaced pointers include the space in their canonical name (for
  example `"global:float32*"`) so distinct spaces cannot collide;
- a hand-built descriptor **must have a unique, stable `name`**, or two
  different structs would collide under the fallback kind name.

## Hand-built aggregates

`STRUCT`, `ARRAY`, and `TAGGED_ENUM` descriptors can be built by filling the
struct yourself: allocate it (statically, or from an arena that outlives the
module), set `kind`, `name`, `size`, `alignment`, and the layout arrays, and
use it wherever a type is accepted. The x86-64 code generator honors the
layout (field offsets for member access lowered as base-plus-offset,
aggregate copies by size). Two honest limitations:

1. The **builder has no aggregate helpers yet**: no field-address instruction
   and no aggregate locals through `mtlc_local`. Field access today is pointer
   arithmetic (`base + offset`, then `mtlc_load`/`mtlc_store` on the field's
   scalar type), which is exactly what it compiles to anyway.
2. The GPU and ARM64 emitters do not accept aggregates at all (see the
   [per-target table](ir.md#what-each-consumer-requires)).

## Classification queries

```c
int mtlc_type_is_integer(const MtlcType *t);   /* INT8..UINT64, BOOL        */
int mtlc_type_is_unsigned(const MtlcType *t);  /* UINT*, BOOL, pointers     */
int mtlc_type_is_float(const MtlcType *t);     /* FLOAT32, FLOAT64          */
int mtlc_type_is_aggregate(const MtlcType *t); /* STRUCT, TAGGED_ENUM, ARRAY */
size_t mtlc_type_size(const MtlcType *t);      /* 0 for NULL                */
size_t mtlc_type_alignment(const MtlcType *t); /* 0 for NULL                */
const char *mtlc_type_kind_name(MtlcTypeKind kind);
```

These are the same predicates the code generators call; if you need to make a
layout or signedness decision in your frontend, deciding it the way the backend
will is one call away.
