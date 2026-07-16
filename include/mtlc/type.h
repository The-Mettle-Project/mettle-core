/* mtlc/type.h - backend-owned type descriptor (the "type IR").
 *
 * libmtlc is a standalone compiler backend, usable by any frontend. It must not
 * depend on any particular frontend's type system, so the backend owns its own
 * type descriptor here. A frontend lowers its own types into MtlcType at the IR
 * boundary (see mtlc_type_from_frontend in the reference Mettle frontend).
 *
 * This struct deliberately mirrors the shape the native code generators need:
 * kind + byte size/alignment, pointer/array element types, aggregate (struct)
 * layout, function-pointer signatures, and tagged-enum layout. It is a plain
 * value type with no methods; construct instances with the mtlc_type_* helpers
 * or fill the fields directly.
 */
#ifndef MTLC_TYPE_H
#define MTLC_TYPE_H

#include <stddef.h>
#include "memory.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Scalar/aggregate classification. Mirrors the categories every native backend
 * must distinguish for ABI classification, load/store widths, and layout. */
typedef enum {
  MTLC_TYPE_INT8,
  MTLC_TYPE_INT16,
  MTLC_TYPE_INT32,
  MTLC_TYPE_INT64,
  MTLC_TYPE_UINT8,
  MTLC_TYPE_UINT16,
  MTLC_TYPE_UINT32,
  MTLC_TYPE_UINT64,
  MTLC_TYPE_BOOL,
  MTLC_TYPE_FLOAT32,
  MTLC_TYPE_FLOAT64,
  MTLC_TYPE_STRING,
  MTLC_TYPE_FUNCTION_POINTER,
  MTLC_TYPE_POINTER,
  MTLC_TYPE_ARRAY,
  MTLC_TYPE_STRUCT,
  MTLC_TYPE_ENUM,
  MTLC_TYPE_TAGGED_ENUM,
  MTLC_TYPE_VOID
} MtlcTypeKind;

typedef struct MtlcType {
  MtlcTypeKind kind;
  const char *name;      /* optional; interned/owned by the frontend adapter */
  size_t size;           /* size in bytes */
  size_t alignment;      /* alignment in bytes */

  /* Meaningful for MTLC_TYPE_POINTER. DEFAULT/GENERIC retain ordinary host
   * pointer behavior; device frontends should use mtlc_type_pointer_in(). */
  MtlcAddressSpace address_space;

  struct MtlcType *base_type; /* pointer/array element type */
  size_t array_size;          /* element count for MTLC_TYPE_ARRAY */

  /* Function-pointer signature (MTLC_TYPE_FUNCTION_POINTER). */
  struct MtlcType **fn_param_types;
  size_t fn_param_count;
  struct MtlcType *fn_return_type;
  /* Synthesized closure-environment struct type for a capturing closure, else
   * NULL. The closure value is an 8-byte pointer to a heap record whose field 0
   * is the code pointer and whose remaining fields are the captures. */
  struct MtlcType *closure_env;

  /* Aggregate layout (MTLC_TYPE_STRUCT). */
  const char **field_names;
  struct MtlcType **field_types;
  size_t *field_offsets;
  size_t field_count;

  /* Tagged-enum layout (MTLC_TYPE_TAGGED_ENUM). */
  const char **tagged_variant_names;
  int *tagged_variant_tags;                 /* discriminant per variant */
  struct MtlcType **tagged_variant_payloads;/* payload type per variant (NULL=none) */
  size_t tagged_variant_count;
  size_t tagged_data_offset;                /* offset of the data union */
  size_t tagged_data_size;                  /* size of the data union */
} MtlcType;

/* Canonical descriptor for a scalar/primitive kind: a shared, immortal singleton
 * with the right size/alignment and canonical name. Intended for frontends that
 * build IR through mtlc/build.h -- the returned pointer never needs freeing and
 * outlives codegen. Returns NULL for kinds that need caller-supplied layout
 * (STRUCT/ARRAY/POINTER/FUNCTION_POINTER/ENUM/TAGGED_ENUM); build those by
 * filling an MtlcType you own, or with mtlc_type_pointer below. */
const MtlcType *mtlc_type_scalar(MtlcTypeKind kind);

/* Canonical pointer-to-`base` descriptor. Interned and immortal like
 * mtlc_type_scalar: calling it twice with the same base returns the same
 * pointer, and the result never needs freeing. `base` must itself be a
 * canonical descriptor (from mtlc_type_scalar or mtlc_type_pointer), so
 * pointer-to-pointer chains work. Returns NULL on NULL base or OOM. */
const MtlcType *mtlc_type_pointer(const MtlcType *base);

/* Canonical pointer descriptor with an explicit device address space. The
 * descriptor is interned and immortal like mtlc_type_pointer(). */
const MtlcType *mtlc_type_pointer_in(const MtlcType *base,
                                     MtlcAddressSpace address_space);

/* Queries used across the backend. Implemented in src/ir/mtlc_type.c. */
int mtlc_type_is_integer(const MtlcType *t);
int mtlc_type_is_unsigned(const MtlcType *t);
int mtlc_type_is_float(const MtlcType *t);
int mtlc_type_is_aggregate(const MtlcType *t);
size_t mtlc_type_size(const MtlcType *t);
size_t mtlc_type_alignment(const MtlcType *t);
const char *mtlc_type_kind_name(MtlcTypeKind kind);

#ifdef __cplusplus
}
#endif

#endif /* MTLC_TYPE_H */
