/* mtlc_type.c - queries over the backend-owned type descriptor (mtlc/type.h).
 *
 * Part of libmtlc. Deliberately frontend-free: it knows nothing about how a
 * frontend's types were translated into MtlcType, only how to answer the
 * classification questions the code generators ask. */
#include "mtlc/type.h"

/* Immortal canonical singletons for the scalar/primitive kinds. Fully static so
 * the pointers are valid for the process lifetime with no ownership concerns --
 * exactly what a frontend building IR through mtlc/build.h needs. */
#define MTLC_SCALAR(k, nm, sz, al)                                             \
  {.kind = (k), .name = (nm), .size = (sz), .alignment = (al)}
static const MtlcType k_scalar_int8 = MTLC_SCALAR(MTLC_TYPE_INT8, "int8", 1, 1);
static const MtlcType k_scalar_int16 = MTLC_SCALAR(MTLC_TYPE_INT16, "int16", 2, 2);
static const MtlcType k_scalar_int32 = MTLC_SCALAR(MTLC_TYPE_INT32, "int32", 4, 4);
static const MtlcType k_scalar_int64 = MTLC_SCALAR(MTLC_TYPE_INT64, "int64", 8, 8);
static const MtlcType k_scalar_uint8 = MTLC_SCALAR(MTLC_TYPE_UINT8, "uint8", 1, 1);
static const MtlcType k_scalar_uint16 = MTLC_SCALAR(MTLC_TYPE_UINT16, "uint16", 2, 2);
static const MtlcType k_scalar_uint32 = MTLC_SCALAR(MTLC_TYPE_UINT32, "uint32", 4, 4);
static const MtlcType k_scalar_uint64 = MTLC_SCALAR(MTLC_TYPE_UINT64, "uint64", 8, 8);
static const MtlcType k_scalar_bool = MTLC_SCALAR(MTLC_TYPE_BOOL, "bool", 1, 1);
static const MtlcType k_scalar_float32 = MTLC_SCALAR(MTLC_TYPE_FLOAT32, "float32", 4, 4);
static const MtlcType k_scalar_float64 = MTLC_SCALAR(MTLC_TYPE_FLOAT64, "float64", 8, 8);
static const MtlcType k_scalar_string = MTLC_SCALAR(MTLC_TYPE_STRING, "string", 8, 8);
static const MtlcType k_scalar_void = MTLC_SCALAR(MTLC_TYPE_VOID, "void", 0, 1);
#undef MTLC_SCALAR

const MtlcType *mtlc_type_scalar(MtlcTypeKind kind) {
  switch (kind) {
  case MTLC_TYPE_INT8:
    return &k_scalar_int8;
  case MTLC_TYPE_INT16:
    return &k_scalar_int16;
  case MTLC_TYPE_INT32:
    return &k_scalar_int32;
  case MTLC_TYPE_INT64:
    return &k_scalar_int64;
  case MTLC_TYPE_UINT8:
    return &k_scalar_uint8;
  case MTLC_TYPE_UINT16:
    return &k_scalar_uint16;
  case MTLC_TYPE_UINT32:
    return &k_scalar_uint32;
  case MTLC_TYPE_UINT64:
    return &k_scalar_uint64;
  case MTLC_TYPE_BOOL:
    return &k_scalar_bool;
  case MTLC_TYPE_FLOAT32:
    return &k_scalar_float32;
  case MTLC_TYPE_FLOAT64:
    return &k_scalar_float64;
  case MTLC_TYPE_STRING:
    return &k_scalar_string;
  case MTLC_TYPE_VOID:
    return &k_scalar_void;
  default:
    return NULL; /* aggregates/pointers need caller-supplied layout */
  }
}

/* Interned pointer types: pointer-to-X is created once and lives for the
 * process (same immortality contract as the scalar singletons -- the module
 * type registry stores MtlcType* by reference and never frees them). The
 * table is tiny in practice: one entry per distinct pointee. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Thread-local, upholding the backend's no-shared-mutable-global-state
 * invariant (see common.h): two frontends on separate threads each intern
 * their own (immutable, immortal) pointer descriptors. */
#include "../common.h"
static MTLC_THREAD_LOCAL const MtlcType **g_ptr_cache;
static MTLC_THREAD_LOCAL size_t g_ptr_cache_count, g_ptr_cache_cap;

const MtlcType *mtlc_type_pointer(const MtlcType *base) {
  return mtlc_type_pointer_in(base, MTLC_ADDRESS_SPACE_GENERIC);
}

static const char *mtlc_address_space_name(MtlcAddressSpace address_space) {
  switch (address_space) {
  case MTLC_ADDRESS_SPACE_GLOBAL: return "global";
  case MTLC_ADDRESS_SPACE_WORKGROUP: return "workgroup";
  case MTLC_ADDRESS_SPACE_CONSTANT: return "constant";
  case MTLC_ADDRESS_SPACE_PRIVATE: return "private";
  case MTLC_ADDRESS_SPACE_DEFAULT:
  case MTLC_ADDRESS_SPACE_GENERIC:
    return "generic";
  }
  return NULL;
}

const MtlcType *mtlc_type_pointer_in(const MtlcType *base,
                                     MtlcAddressSpace address_space) {
  if (!base) {
    return NULL;
  }
  if (!mtlc_address_space_name(address_space)) {
    return NULL;
  }
  if (address_space == MTLC_ADDRESS_SPACE_DEFAULT) {
    address_space = MTLC_ADDRESS_SPACE_GENERIC;
  }
  for (size_t i = 0; i < g_ptr_cache_count; i++) {
    if (g_ptr_cache[i]->base_type == (MtlcType *)base &&
        g_ptr_cache[i]->address_space == address_space) {
      return g_ptr_cache[i];
    }
  }
  MtlcType *p = (MtlcType *)calloc(1, sizeof(MtlcType));
  if (!p) {
    return NULL;
  }
  p->kind = MTLC_TYPE_POINTER;
  p->size = 8;
  p->alignment = 8;
  p->base_type = (MtlcType *)base;
  p->address_space = address_space;
  /* Generic pointers retain the source-compatible "T*" spelling. Explicit
   * spaces are part of the canonical name so a module can register global and
   * workgroup pointers to the same element type without aliasing them. */
  {
    const char *bn = base->name ? base->name : mtlc_type_kind_name(base->kind);
    const char *asn = mtlc_address_space_name(address_space);
    int explicit_space = address_space != MTLC_ADDRESS_SPACE_GENERIC;
    size_t n = strlen(bn) + 2 + (explicit_space ? strlen(asn) + 2 : 0);
    char *nm = (char *)malloc(n);
    if (!nm) {
      free(p);
      return NULL;
    }
    if (explicit_space) {
      snprintf(nm, n, "%s:%s*", asn, bn);
    } else {
      snprintf(nm, n, "%s*", bn);
    }
    p->name = nm;
  }
  if (g_ptr_cache_count == g_ptr_cache_cap) {
    size_t next = g_ptr_cache_cap ? g_ptr_cache_cap * 2 : 8;
    const MtlcType **grown =
        (const MtlcType **)realloc(g_ptr_cache, next * sizeof(*g_ptr_cache));
    if (!grown) {
      free((char *)p->name);
      free(p);
      return NULL;
    }
    g_ptr_cache = grown;
    g_ptr_cache_cap = next;
  }
  g_ptr_cache[g_ptr_cache_count++] = p;
  return p;
}

int mtlc_type_is_integer(const MtlcType *t) {
  if (!t) {
    return 0;
  }
  switch (t->kind) {
  case MTLC_TYPE_INT8:
  case MTLC_TYPE_INT16:
  case MTLC_TYPE_INT32:
  case MTLC_TYPE_INT64:
  case MTLC_TYPE_UINT8:
  case MTLC_TYPE_UINT16:
  case MTLC_TYPE_UINT32:
  case MTLC_TYPE_UINT64:
  case MTLC_TYPE_BOOL:
    return 1;
  default:
    return 0;
  }
}

int mtlc_type_is_unsigned(const MtlcType *t) {
  if (!t) {
    return 0;
  }
  switch (t->kind) {
  case MTLC_TYPE_UINT8:
  case MTLC_TYPE_UINT16:
  case MTLC_TYPE_UINT32:
  case MTLC_TYPE_UINT64:
  case MTLC_TYPE_BOOL:
    return 1;
  default:
    return 0;
  }
}

int mtlc_type_is_float(const MtlcType *t) {
  return t && (t->kind == MTLC_TYPE_FLOAT32 || t->kind == MTLC_TYPE_FLOAT64);
}

int mtlc_type_is_aggregate(const MtlcType *t) {
  return t && (t->kind == MTLC_TYPE_STRUCT || t->kind == MTLC_TYPE_TAGGED_ENUM ||
               t->kind == MTLC_TYPE_ARRAY);
}

size_t mtlc_type_size(const MtlcType *t) { return t ? t->size : 0; }

size_t mtlc_type_alignment(const MtlcType *t) { return t ? t->alignment : 0; }

const char *mtlc_type_kind_name(MtlcTypeKind kind) {
  switch (kind) {
  case MTLC_TYPE_INT8:
    return "int8";
  case MTLC_TYPE_INT16:
    return "int16";
  case MTLC_TYPE_INT32:
    return "int32";
  case MTLC_TYPE_INT64:
    return "int64";
  case MTLC_TYPE_UINT8:
    return "uint8";
  case MTLC_TYPE_UINT16:
    return "uint16";
  case MTLC_TYPE_UINT32:
    return "uint32";
  case MTLC_TYPE_UINT64:
    return "uint64";
  case MTLC_TYPE_BOOL:
    return "bool";
  case MTLC_TYPE_FLOAT32:
    return "float32";
  case MTLC_TYPE_FLOAT64:
    return "float64";
  case MTLC_TYPE_STRING:
    return "string";
  case MTLC_TYPE_FUNCTION_POINTER:
    return "fnptr";
  case MTLC_TYPE_POINTER:
    return "pointer";
  case MTLC_TYPE_ARRAY:
    return "array";
  case MTLC_TYPE_STRUCT:
    return "struct";
  case MTLC_TYPE_ENUM:
    return "enum";
  case MTLC_TYPE_TAGGED_ENUM:
    return "tagged_enum";
  case MTLC_TYPE_VOID:
    return "void";
  }
  return "?";
}
