/* mtlc_type.c - queries over the backend-owned type descriptor (mtlc/type.h).
 *
 * Part of libmtlc. Deliberately frontend-free: it knows nothing about how a
 * frontend's types were translated into MtlcType, only how to answer the
 * classification questions the code generators ask. */
#include "mtlc/type.h"

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
