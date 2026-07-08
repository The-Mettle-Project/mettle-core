/* mtlc_type_from_frontend.c - translate the Mettle frontend's Type into MtlcType.
 *
 * FRONTEND-side adapter (compiles into the mettle driver, NOT into libmtlc). This
 * is the single translation unit permitted to include both the frontend `Type`
 * (semantic/symbol_table.h) and the backend `MtlcType` (mtlc/type.h). Keeping the
 * translation here is what lets libmtlc stay free of any frontend's type system.
 *
 * The mapping is structural and 1:1 on the kind enum (TypeKind and MtlcTypeKind
 * intentionally share order and membership). Results are memoized by frontend
 * Type pointer so shared/recursive types translate once and cycles terminate. */
#include "frontend/mtlc_frontend.h"

#include <stdlib.h>
#include <string.h>

/* Process-lifetime memo of (frontend Type* -> backend MtlcType*). A one-shot
 * compile never frees these; the arena leaks intentionally at exit. */
typedef struct {
  const Type *from;
  MtlcType *to;
} TypeMemoEntry;

static TypeMemoEntry *g_memo = NULL;
static size_t g_memo_count = 0;
static size_t g_memo_capacity = 0;

static MtlcType *memo_lookup(const Type *from) {
  for (size_t i = 0; i < g_memo_count; i++) {
    if (g_memo[i].from == from) {
      return g_memo[i].to;
    }
  }
  return NULL;
}

static void memo_insert(const Type *from, MtlcType *to) {
  if (g_memo_count == g_memo_capacity) {
    size_t next = g_memo_capacity ? g_memo_capacity * 2 : 64;
    TypeMemoEntry *grown =
        (TypeMemoEntry *)realloc(g_memo, next * sizeof(TypeMemoEntry));
    if (!grown) {
      return; /* out of memory: skip memoization; translation still proceeds */
    }
    g_memo = grown;
    g_memo_capacity = next;
  }
  g_memo[g_memo_count].from = from;
  g_memo[g_memo_count].to = to;
  g_memo_count++;
}

static MtlcTypeKind translate_kind(TypeKind kind) {
  switch (kind) {
  case TYPE_INT8:
    return MTLC_TYPE_INT8;
  case TYPE_INT16:
    return MTLC_TYPE_INT16;
  case TYPE_INT32:
    return MTLC_TYPE_INT32;
  case TYPE_INT64:
    return MTLC_TYPE_INT64;
  case TYPE_UINT8:
    return MTLC_TYPE_UINT8;
  case TYPE_UINT16:
    return MTLC_TYPE_UINT16;
  case TYPE_UINT32:
    return MTLC_TYPE_UINT32;
  case TYPE_UINT64:
    return MTLC_TYPE_UINT64;
  case TYPE_BOOL:
    return MTLC_TYPE_BOOL;
  case TYPE_FLOAT32:
    return MTLC_TYPE_FLOAT32;
  case TYPE_FLOAT64:
    return MTLC_TYPE_FLOAT64;
  case TYPE_STRING:
    return MTLC_TYPE_STRING;
  case TYPE_FUNCTION_POINTER:
    return MTLC_TYPE_FUNCTION_POINTER;
  case TYPE_POINTER:
    return MTLC_TYPE_POINTER;
  case TYPE_ARRAY:
    return MTLC_TYPE_ARRAY;
  case TYPE_STRUCT:
    return MTLC_TYPE_STRUCT;
  case TYPE_ENUM:
    return MTLC_TYPE_ENUM;
  case TYPE_TAGGED_ENUM:
    return MTLC_TYPE_TAGGED_ENUM;
  case TYPE_VOID:
    return MTLC_TYPE_VOID;
  }
  return MTLC_TYPE_VOID;
}

/* Duplicate an array of frontend Type* into an array of translated MtlcType*. */
static MtlcType **translate_type_array(struct Type **in, size_t count) {
  if (!in || count == 0) {
    return NULL;
  }
  MtlcType **out = (MtlcType **)calloc(count, sizeof(MtlcType *));
  if (!out) {
    return NULL;
  }
  for (size_t i = 0; i < count; i++) {
    out[i] = mtlc_type_from_frontend(in[i]);
  }
  return out;
}

MtlcType *mtlc_type_from_frontend(const Type *type) {
  if (!type) {
    return NULL;
  }
  MtlcType *existing = memo_lookup(type);
  if (existing) {
    return existing;
  }

  MtlcType *out = (MtlcType *)calloc(1, sizeof(MtlcType));
  if (!out) {
    return NULL;
  }
  /* Register BEFORE recursing so a type reachable from itself resolves to this
   * same node instead of recursing forever. */
  memo_insert(type, out);

  out->kind = translate_kind(type->kind);
  out->name = type->name; /* borrow the frontend's interned name */
  out->size = type->size;
  out->alignment = type->alignment;
  out->array_size = type->array_size;

  out->base_type = mtlc_type_from_frontend(type->base_type);
  out->fn_return_type = mtlc_type_from_frontend(type->fn_return_type);
  out->closure_env = mtlc_type_from_frontend(type->closure_env);
  out->fn_param_count = type->fn_param_count;
  out->fn_param_types =
      translate_type_array(type->fn_param_types, type->fn_param_count);

  /* Struct layout: names/offsets are borrowed; field types are translated. */
  out->field_count = type->field_count;
  out->field_names = (const char **)type->field_names;
  out->field_offsets = type->field_offsets;
  out->field_types =
      translate_type_array(type->field_types, type->field_count);

  /* Tagged-enum layout. */
  out->tagged_variant_count = type->tagged_variant_count;
  out->tagged_variant_names = (const char **)type->tagged_variant_names;
  out->tagged_variant_tags = type->tagged_variant_tags;
  out->tagged_variant_payloads = translate_type_array(
      type->tagged_variant_payloads, type->tagged_variant_count);
  out->tagged_data_offset = type->tagged_data_offset;
  out->tagged_data_size = type->tagged_data_size;

  return out;
}
