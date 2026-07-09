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
