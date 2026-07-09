/* IR -> SPIR-V binary emitter (OpenCL 1.2 environment). See spirv_emitter.h.
 *
 * SPIR-V sibling of ptx_emitter.c. Two things make SPIR-V different from PTX:
 *
 *  1. It is a *binary* word stream (little-endian 32-bit words), laid out in a
 *     fixed section order (capabilities, ext-imports, memory model, entry
 *     points, decorations, types+constants+globals, then function bodies). We
 *     buffer each section separately and concatenate at the end, so emission
 *     order need not match layout order.
 *
 *  2. It mandates *structured* control flow -- there is no free `bra`. Every
 *     function is lowered to a single OpSwitch state-machine dispatch loop:
 *
 *         entry:   <locals>; state = 0; branch header
 *         header:  OpLoopMerge exit, continue; branch switch
 *         switch:  s = load state; OpSelectionMerge merge; OpSwitch s -> case_i
 *         case_i:  <block i body>; state = <successor>; branch merge
 *         merge:   branch continue
 *         continue:branch header
 *         exit:    OpReturn
 *
 *     This is a correct-by-construction structuring of ANY reducible CFG (the
 *     source is always structured, so the CFG always is): the whole function
 *     needs exactly one OpLoopMerge and one OpSelectionMerge, and short-circuit
 *     `&&`/`||` branch chains, nested loops and breaks all fall out for free. A
 *     conditional branch picks its successor state with OpSelect (no nested
 *     merges). Every IR value lives in a Function-storage variable (reg2mem) so
 *     there are never cross-block SSA references; a driver's SPIR-V consumer
 *     promotes them back to registers.
 *
 * Pointers follow the PTX model: a kernel pointer parameter is an OpenCL
 * __global (CrossWorkgroup) pointer, immediately OpConvertPtrToU'd to a 64-bit
 * integer that all arithmetic runs on, then OpConvertUToPtr'd back to a typed
 * pointer at each load/store. That needs the Addresses capability and matches
 * the IR, whose address arithmetic is already baked into integer ops. */
#include "spirv_emitter.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- SPIR-V constants (from the core spec / OpenCL env) ---- */
#define SPV_MAGIC 0x07230203u
#define SPV_VERSION_1_0 0x00010000u

/* opcodes */
enum {
  Op_ExtInstImport = 11,
  Op_ExtInst = 12,
  Op_MemoryModel = 14,
  Op_EntryPoint = 15,
  Op_Capability = 17,
  Op_TypeVoid = 19,
  Op_TypeBool = 20,
  Op_TypeInt = 21,
  Op_TypeFloat = 22,
  Op_TypeVector = 23,
  Op_TypePointer = 32,
  Op_TypeFunction = 33,
  Op_Constant = 43,
  Op_Function = 54,
  Op_FunctionParameter = 55,
  Op_FunctionEnd = 56,
  Op_Variable = 59,
  Op_Load = 61,
  Op_Store = 62,
  Op_CompositeExtract = 81,
  Op_ConvertFToU = 109,
  Op_ConvertFToS = 110,
  Op_ConvertSToF = 111,
  Op_ConvertUToF = 112,
  Op_UConvert = 113,
  Op_SConvert = 114,
  Op_FConvert = 115,
  Op_ConvertPtrToU = 117,
  Op_ConvertUToPtr = 120,
  Op_Bitcast = 124,
  Op_SNegate = 126,
  Op_FNegate = 127,
  Op_IAdd = 128,
  Op_FAdd = 129,
  Op_ISub = 130,
  Op_FSub = 131,
  Op_IMul = 132,
  Op_FMul = 133,
  Op_UDiv = 134,
  Op_SDiv = 135,
  Op_FDiv = 136,
  Op_UMod = 137,
  Op_SRem = 138,
  Op_FRem = 140,
  Op_ShiftRightLogical = 194,
  Op_ShiftRightArithmetic = 195,
  Op_ShiftLeftLogical = 196,
  Op_BitwiseOr = 197,
  Op_BitwiseXor = 198,
  Op_BitwiseAnd = 199,
  Op_Not = 200,
  Op_Select = 169,
  Op_IEqual = 170,
  Op_INotEqual = 171,
  Op_UGreaterThan = 172,
  Op_SGreaterThan = 173,
  Op_UGreaterThanEqual = 174,
  Op_SGreaterThanEqual = 175,
  Op_ULessThan = 176,
  Op_SLessThan = 177,
  Op_ULessThanEqual = 178,
  Op_SLessThanEqual = 179,
  Op_FOrdEqual = 180,
  Op_FUnordNotEqual = 183,
  Op_FOrdLessThan = 184,
  Op_FOrdGreaterThan = 186,
  Op_FOrdLessThanEqual = 188,
  Op_FOrdGreaterThanEqual = 190,
  Op_ControlBarrier = 224,
  Op_AtomicIAdd = 234,
  Op_AtomicUMin = 237,
  Op_Decorate = 71,
  Op_LoopMerge = 246,
  Op_SelectionMerge = 247,
  Op_Label = 248,
  Op_Branch = 249,
  Op_Switch = 251,
  Op_Return = 253
};

/* capabilities */
enum {
  Cap_Addresses = 4,
  Cap_Kernel = 6,
  Cap_Float16 = 9,
  Cap_Float64 = 10,
  Cap_Int64 = 11,
  Cap_Int64Atomics = 12,
  Cap_Int16 = 22,
  Cap_Int8 = 39
};

/* storage classes */
enum {
  SC_Input = 1,
  SC_CrossWorkgroup = 5,
  SC_Function = 7
};

/* misc enums */
enum { AddrModel_Physical64 = 2, MemModel_OpenCL = 2, ExecModel_Kernel = 6 };
enum { Decoration_BuiltIn = 11 };
enum { MemAccess_Aligned = 2 };
enum { Scope_Device = 1, Scope_Workgroup = 2 };
/* SequentiallyConsistent(0x10) | WorkgroupMemory(0x100) */
enum { Sem_None = 0, Sem_WorkgroupBarrier = 0x110 };

/* BuiltIn ids */
enum {
  BI_NumWorkgroups = 24,
  BI_WorkgroupSize = 25,
  BI_WorkgroupId = 26,
  BI_LocalInvocationId = 27
};

/* OpenCL.std extended instruction numbers */
enum {
  CL_cos = 14,
  CL_exp = 19,
  CL_fabs = 23,
  CL_log = 37,
  CL_rsqrt = 56,
  CL_sin = 57,
  CL_sqrt = 61
};

/* ---- growable word buffer ---- */
typedef struct {
  uint32_t *data;
  size_t len, cap;
} Wb;

static void wb_push(Wb *w, uint32_t v) {
  if (w->len == w->cap) {
    w->cap = w->cap ? w->cap * 2 : 64;
    w->data = realloc(w->data, w->cap * sizeof(uint32_t));
  }
  w->data[w->len++] = v;
}
static void wb_free(Wb *w) {
  free(w->data);
  w->data = NULL;
  w->len = w->cap = 0;
}
static void wb_str(Wb *w, const char *s) {
  size_t len = strlen(s);
  size_t nwords = len / 4 + 1; /* room for the terminating NUL + zero pad */
  for (size_t i = 0; i < nwords; i++) {
    uint32_t word = 0;
    for (int b = 0; b < 4; b++) {
      size_t idx = i * 4 + (size_t)b;
      unsigned char c = idx < len ? (unsigned char)s[idx] : 0;
      word |= (uint32_t)c << (8 * b);
    }
    wb_push(w, word);
  }
}
/* emit an instruction whose operands are already in `ops` */
static void emit_ops(Wb *sec, uint16_t opcode, const Wb *ops) {
  wb_push(sec, ((uint32_t)(ops->len + 1) << 16) | opcode);
  for (size_t i = 0; i < ops->len; i++) {
    wb_push(sec, ops->data[i]);
  }
}
/* emit an instruction with up to 16 fixed operands passed as unsigned ints */
static void emitv(Wb *sec, uint16_t opcode, size_t nops, ...) {
  uint32_t tmp[16];
  va_list ap;
  va_start(ap, nops);
  for (size_t i = 0; i < nops && i < 16; i++) {
    tmp[i] = va_arg(ap, unsigned);
  }
  va_end(ap);
  wb_push(sec, ((uint32_t)(nops + 1) << 16) | opcode);
  for (size_t i = 0; i < nops; i++) {
    wb_push(sec, tmp[i]);
  }
}

/* ---- interning cache ---- */
typedef struct {
  char *key;
  uint32_t id;
} CacheEnt;

typedef struct {
  Wb caps, extimports, memmodel, entrypoints, decorations, typesconsts, functions;
  uint32_t next_id;
  CacheEnt *cache; /* types + constants + pointer types, keyed by string */
  size_t ncache, capcache;
  uint32_t opencl_ext;
  uint32_t builtin_var[64]; /* BuiltIn id -> Input variable id (0 = none yet) */
  int use_int8, use_int16, use_float16, use_float64, use_atomics64;
  char *error;
} SpvMod;

static uint32_t new_id(SpvMod *m) { return m->next_id++; }

static void mod_error(SpvMod *m, const char *fmt, ...) {
  if (m->error) {
    return;
  }
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  m->error = strdup(buf);
}

static uint32_t cache_get(SpvMod *m, const char *key) {
  for (size_t i = 0; i < m->ncache; i++) {
    if (strcmp(m->cache[i].key, key) == 0) {
      return m->cache[i].id;
    }
  }
  return 0;
}
static void cache_put(SpvMod *m, const char *key, uint32_t id) {
  if (m->ncache == m->capcache) {
    m->capcache = m->capcache ? m->capcache * 2 : 32;
    m->cache = realloc(m->cache, m->capcache * sizeof(CacheEnt));
  }
  m->cache[m->ncache].key = strdup(key);
  m->cache[m->ncache].id = id;
  m->ncache++;
}

/* ---- types ---- */
static uint32_t type_void(SpvMod *m) {
  uint32_t id = cache_get(m, "void");
  if (id) return id;
  id = new_id(m);
  emitv(&m->typesconsts, Op_TypeVoid, 1, id);
  cache_put(m, "void", id);
  return id;
}
static uint32_t type_int(SpvMod *m, int width) {
  char key[16];
  snprintf(key, sizeof(key), "i%d", width);
  uint32_t id = cache_get(m, key);
  if (id) return id;
  if (width == 8) m->use_int8 = 1;
  if (width == 16) m->use_int16 = 1;
  id = new_id(m);
  /* OpenCL environment: OpTypeInt signedness must be 0 (signed vs unsigned is
   * expressed by the operations, e.g. OpSDiv vs OpUDiv). */
  emitv(&m->typesconsts, Op_TypeInt, 3, id, (unsigned)width, 0u);
  cache_put(m, key, id);
  return id;
}
static uint32_t type_float(SpvMod *m, int width) {
  char key[16];
  snprintf(key, sizeof(key), "f%d", width);
  uint32_t id = cache_get(m, key);
  if (id) return id;
  if (width == 16) m->use_float16 = 1;
  if (width == 64) m->use_float64 = 1;
  id = new_id(m);
  emitv(&m->typesconsts, Op_TypeFloat, 2, id, (unsigned)width);
  cache_put(m, key, id);
  return id;
}
static uint32_t type_bool(SpvMod *m) {
  uint32_t id = cache_get(m, "bool");
  if (id) return id;
  id = new_id(m);
  emitv(&m->typesconsts, Op_TypeBool, 1, id);
  cache_put(m, "bool", id);
  return id;
}
static uint32_t type_pointer(SpvMod *m, int sc, uint32_t pointee) {
  char key[48];
  snprintf(key, sizeof(key), "p%d:%u", sc, pointee);
  uint32_t id = cache_get(m, key);
  if (id) return id;
  id = new_id(m);
  emitv(&m->typesconsts, Op_TypePointer, 3, id, (unsigned)sc, pointee);
  cache_put(m, key, id);
  return id;
}
static uint32_t type_vec3_ulong(SpvMod *m) {
  uint32_t id = cache_get(m, "v3ulong");
  if (id) return id;
  uint32_t u64 = type_int(m, 64);
  id = new_id(m);
  emitv(&m->typesconsts, Op_TypeVector, 3, id, u64, 3u);
  cache_put(m, "v3ulong", id);
  return id;
}

/* map an MtlcTypeKind to (width, is-float) */
static int kind_is_float(MtlcTypeKind k) {
  return k == MTLC_TYPE_FLOAT32 || k == MTLC_TYPE_FLOAT64;
}
static int kind_is_unsigned(MtlcTypeKind k) {
  switch (k) {
  case MTLC_TYPE_UINT8:
  case MTLC_TYPE_UINT16:
  case MTLC_TYPE_UINT32:
  case MTLC_TYPE_UINT64:
  case MTLC_TYPE_BOOL:
  case MTLC_TYPE_POINTER:
  case MTLC_TYPE_ARRAY:
  case MTLC_TYPE_STRING:
  case MTLC_TYPE_FUNCTION_POINTER:
    return 1;
  default:
    return 0;
  }
}
static int kind_bits(MtlcTypeKind k) {
  switch (k) {
  case MTLC_TYPE_INT8:
  case MTLC_TYPE_UINT8:
  case MTLC_TYPE_BOOL:
    return 8;
  case MTLC_TYPE_INT16:
  case MTLC_TYPE_UINT16:
    return 16;
  case MTLC_TYPE_INT32:
  case MTLC_TYPE_UINT32:
  case MTLC_TYPE_FLOAT32:
    return 32;
  default:
    return 64; /* int64/uint64/float64/pointer/array/string/funcptr */
  }
}
static uint32_t kind_type(SpvMod *m, MtlcTypeKind k) {
  switch (k) {
  case MTLC_TYPE_FLOAT32:
    return type_float(m, 32);
  case MTLC_TYPE_FLOAT64:
    return type_float(m, 64);
  case MTLC_TYPE_INT8:
  case MTLC_TYPE_UINT8:
  case MTLC_TYPE_BOOL:
    return type_int(m, 8);
  case MTLC_TYPE_INT16:
  case MTLC_TYPE_UINT16:
    return type_int(m, 16);
  case MTLC_TYPE_INT32:
  case MTLC_TYPE_UINT32:
    return type_int(m, 32);
  default:
    return type_int(m, 64);
  }
}

/* ---- constants ---- */
static uint32_t const_u32(SpvMod *m, uint32_t v) {
  char key[32];
  snprintf(key, sizeof(key), "cu32:%u", v);
  uint32_t id = cache_get(m, key);
  if (id) return id;
  uint32_t t = type_int(m, 32);
  id = new_id(m);
  emitv(&m->typesconsts, Op_Constant, 3, t, id, v);
  cache_put(m, key, id);
  return id;
}
static uint32_t const_scalar_int(SpvMod *m, MtlcTypeKind k, long long v) {
  int bits = kind_bits(k);
  char key[48];
  snprintf(key, sizeof(key), "ci:%d:%lld", bits, v);
  uint32_t id = cache_get(m, key);
  if (id) return id;
  uint32_t t = kind_type(m, k);
  id = new_id(m);
  if (bits <= 32) {
    emitv(&m->typesconsts, Op_Constant, 3, t, id, (unsigned)(uint32_t)v);
  } else {
    uint64_t u = (uint64_t)v;
    emitv(&m->typesconsts, Op_Constant, 4, t, id, (unsigned)(uint32_t)u,
          (unsigned)(uint32_t)(u >> 32));
  }
  cache_put(m, key, id);
  return id;
}
static uint32_t f32_bits(double v) {
  float f = (float)v;
  uint32_t b;
  memcpy(&b, &f, 4);
  return b;
}
static uint64_t f64_bits(double v) {
  uint64_t b;
  memcpy(&b, &v, 8);
  return b;
}
static uint32_t const_float(SpvMod *m, MtlcTypeKind k, double v) {
  char key[48];
  if (k == MTLC_TYPE_FLOAT32) {
    uint32_t bits = f32_bits(v);
    snprintf(key, sizeof(key), "cf32:%u", bits);
    uint32_t id = cache_get(m, key);
    if (id) return id;
    uint32_t t = type_float(m, 32);
    id = new_id(m);
    emitv(&m->typesconsts, Op_Constant, 3, t, id, bits);
    cache_put(m, key, id);
    return id;
  } else {
    uint64_t bits = f64_bits(v);
    snprintf(key, sizeof(key), "cf64:%llu", (unsigned long long)bits);
    uint32_t id = cache_get(m, key);
    if (id) return id;
    uint32_t t = type_float(m, 64);
    id = new_id(m);
    emitv(&m->typesconsts, Op_Constant, 4, t, id, (unsigned)(uint32_t)bits,
          (unsigned)(uint32_t)(bits >> 32));
    cache_put(m, key, id);
    return id;
  }
}

/* ---- BuiltIn Input variable (v3ulong) ---- */
static uint32_t builtin_var(SpvMod *m, int builtin) {
  if (m->builtin_var[builtin]) {
    return m->builtin_var[builtin];
  }
  uint32_t v3 = type_vec3_ulong(m);
  uint32_t pt = type_pointer(m, SC_Input, v3);
  uint32_t id = new_id(m);
  emitv(&m->typesconsts, Op_Variable, 3, pt, id, (unsigned)SC_Input);
  emitv(&m->decorations, Op_Decorate, 3, id, (unsigned)Decoration_BuiltIn,
        (unsigned)builtin);
  m->builtin_var[builtin] = id;
  return id;
}

/* ============================ value model ============================ */
/* A value descriptor, mirroring the PTX PtxVal (class/ptr/elem). Pointers are
 * carried as 64-bit integers; is_ptr/elem remember the pointee for load/store. */
typedef struct {
  MtlcTypeKind kind;
  int is_unsigned;
  int is_ptr;
  MtlcTypeKind elem;
} SpvDesc;

typedef struct {
  char *name;
  SpvDesc d;
  uint32_t var_id; /* OpVariable (Function ptr to kind_type(d.kind)) */
} SpvBind;

typedef struct {
  SpvMod *m;
  SpvBind *binds;
  size_t nbinds, capbinds;
  uint32_t used_builtins[16];
  size_t nused_builtins;
} SpvFn;

static SpvBind *find_bind(SpvFn *fn, const char *name) {
  if (!name) return NULL;
  for (size_t i = 0; i < fn->nbinds; i++) {
    if (strcmp(fn->binds[i].name, name) == 0) {
      return &fn->binds[i];
    }
  }
  return NULL;
}
static SpvBind *add_bind(SpvFn *fn, const char *name, SpvDesc d) {
  SpvBind *b = find_bind(fn, name);
  if (b) return b;
  if (fn->nbinds == fn->capbinds) {
    fn->capbinds = fn->capbinds ? fn->capbinds * 2 : 16;
    fn->binds = realloc(fn->binds, fn->capbinds * sizeof(SpvBind));
  }
  b = &fn->binds[fn->nbinds++];
  b->name = strdup(name);
  b->d = d;
  b->var_id = 0;
  return b;
}
static void track_builtin(SpvFn *fn, uint32_t var) {
  for (size_t i = 0; i < fn->nused_builtins; i++) {
    if (fn->used_builtins[i] == var) return;
  }
  if (fn->nused_builtins < 16) {
    fn->used_builtins[fn->nused_builtins++] = var;
  }
}

/* ---- type-name parsing (no symbol table), same rules as the PTX backend ---- */
static MtlcTypeKind base_kind_from_name(const char *s, int *is_unsigned) {
  *is_unsigned = (strstr(s, "uint") != NULL || strstr(s, "bool") != NULL);
  if (strstr(s, "float32") || strstr(s, "f32")) return MTLC_TYPE_FLOAT32;
  if (strstr(s, "float64") || strstr(s, "double") || strstr(s, "float"))
    return MTLC_TYPE_FLOAT64;
  if (strstr(s, "int64") || strstr(s, "uint64"))
    return *is_unsigned ? MTLC_TYPE_UINT64 : MTLC_TYPE_INT64;
  if (strstr(s, "int16") || strstr(s, "uint16"))
    return *is_unsigned ? MTLC_TYPE_UINT16 : MTLC_TYPE_INT16;
  if (strstr(s, "int8") || strstr(s, "uint8"))
    return *is_unsigned ? MTLC_TYPE_UINT8 : MTLC_TYPE_INT8;
  if (strstr(s, "int32") || strstr(s, "uint32"))
    return *is_unsigned ? MTLC_TYPE_UINT32 : MTLC_TYPE_INT32;
  if (strstr(s, "bool")) return MTLC_TYPE_BOOL;
  return MTLC_TYPE_INT32;
}
static SpvDesc desc_from_typename(const char *name) {
  SpvDesc v = {0};
  if (!name) name = "int64";
  int ptr = (strchr(name, '*') != NULL) || strstr(name, "cstring") ||
            strstr(name, "string");
  int isu = 0;
  MtlcTypeKind base = base_kind_from_name(name, &isu);
  if (strstr(name, "cstring")) base = MTLC_TYPE_UINT8;
  if (ptr) {
    v.kind = MTLC_TYPE_POINTER;
    v.is_ptr = 1;
    v.is_unsigned = 1;
    const char *fs = strchr(name, '*');
    v.elem = (fs && strchr(fs + 1, '*')) ? MTLC_TYPE_POINTER : base;
  } else {
    v.kind = base;
    v.is_unsigned = kind_is_unsigned(base);
    v.elem = MTLC_TYPE_VOID;
  }
  return v;
}

static SpvDesc operand_desc(SpvFn *fn, const IROperand *op) {
  SpvDesc v = {0};
  if (op->kind == IR_OPERAND_INT) {
    v.kind = (op->int_value > 2147483647LL || op->int_value < -2147483648LL)
                 ? MTLC_TYPE_INT64
                 : MTLC_TYPE_INT32;
    return v;
  }
  if (op->kind == IR_OPERAND_FLOAT) {
    v.kind = (op->float_bits == 32) ? MTLC_TYPE_FLOAT32 : MTLC_TYPE_FLOAT64;
    return v;
  }
  SpvBind *b = find_bind(fn, op->name);
  if (b) return b->d;
  v.kind = MTLC_TYPE_INT32;
  return v;
}

/* ---- intrinsics classification (shared by pre-pass and body) ---- */
static int sreg_component(const char *name, int *builtin) {
  if (!name || strncmp(name, "gpu_", 4) != 0) return -1;
  int comp = -1;
  size_t len = strlen(name);
  char axis = name[len - 1];
  if (axis == 'x') comp = 0;
  else if (axis == 'y') comp = 1;
  else if (axis == 'z') comp = 2;
  else return -1;
  if (strncmp(name, "gpu_tid_", 8) == 0) *builtin = BI_LocalInvocationId;
  else if (strncmp(name, "gpu_ntid_", 9) == 0) *builtin = BI_WorkgroupSize;
  else if (strncmp(name, "gpu_ctaid_", 10) == 0) *builtin = BI_WorkgroupId;
  else if (strncmp(name, "gpu_nctaid_", 11) == 0) *builtin = BI_NumWorkgroups;
  else return -1;
  return comp;
}
static int is_math_intrinsic(const char *c) {
  return c && (!strcmp(c, "sqrtf") || !strcmp(c, "rsqrtf") ||
               !strcmp(c, "fabsf") || !strcmp(c, "sinf") || !strcmp(c, "cosf") ||
               !strcmp(c, "logf") || !strcmp(c, "expf"));
}
static int is_atomic_intrinsic(const char *c) {
  return c && (!strcmp(c, "atomic_min_u32") || !strcmp(c, "atomic_min_u64") ||
               !strcmp(c, "atomic_add_u32"));
}

static SpvDesc call_result_desc(const IRInstruction *in) {
  SpvDesc r = {0};
  const char *c = in->text;
  int bi = 0;
  if (sreg_component(c, &bi) >= 0) {
    r.kind = MTLC_TYPE_UINT32;
    r.is_unsigned = 1;
  } else if (c && !strcmp(c, "h2f")) {
    r.kind = MTLC_TYPE_FLOAT32;
  } else if (c && !strcmp(c, "f2h")) {
    r.kind = MTLC_TYPE_UINT32;
    r.is_unsigned = 1;
  } else if (is_math_intrinsic(c)) {
    r.kind = MTLC_TYPE_FLOAT32;
  } else if (c && !strcmp(c, "atomic_min_u64")) {
    r.kind = MTLC_TYPE_UINT64;
    r.is_unsigned = 1;
  } else if (is_atomic_intrinsic(c)) {
    r.kind = MTLC_TYPE_UINT32;
    r.is_unsigned = 1;
  } else {
    r.kind = MTLC_TYPE_INT32;
  }
  return r;
}

static int is_compare_op(const char *t) {
  return !strcmp(t, "<") || !strcmp(t, ">") || !strcmp(t, "<=") ||
         !strcmp(t, ">=") || !strcmp(t, "==") || !strcmp(t, "!=");
}

/* dominant result kind of a binary op (mirrors PTX emit_binary result class) */
static SpvDesc binary_result_desc(SpvFn *fn, const IRInstruction *in) {
  const char *t = in->text ? in->text : "+";
  SpvDesc la = operand_desc(fn, &in->lhs), ra = operand_desc(fn, &in->rhs);
  SpvDesc dv = {0};
  if (is_compare_op(t)) {
    dv.kind = MTLC_TYPE_INT32;
    dv.is_unsigned = 1;
    return dv;
  }
  if (in->is_float) {
    dv.kind = (in->float_bits == 32) ? MTLC_TYPE_FLOAT32 : MTLC_TYPE_FLOAT64;
  } else if (la.is_ptr || ra.is_ptr) {
    dv.kind = MTLC_TYPE_POINTER;
    dv.is_ptr = 1;
    dv.is_unsigned = 1;
    dv.elem = la.is_ptr ? la.elem : ra.elem;
  } else if (kind_is_float(la.kind) || kind_is_float(ra.kind)) {
    dv.kind = (la.kind == MTLC_TYPE_FLOAT64 || ra.kind == MTLC_TYPE_FLOAT64)
                  ? MTLC_TYPE_FLOAT64
                  : MTLC_TYPE_FLOAT32;
  } else {
    int b64 = kind_bits(la.kind) == 64 || kind_bits(ra.kind) == 64;
    int u = la.is_unsigned || ra.is_unsigned;
    dv.kind = b64 ? (u ? MTLC_TYPE_UINT64 : MTLC_TYPE_INT64)
                  : (u ? MTLC_TYPE_UINT32 : MTLC_TYPE_INT32);
    dv.is_unsigned = u;
  }
  return dv;
}

/* result descriptor of any instruction that defines dest.name */
static SpvDesc result_desc(SpvFn *fn, const IRInstruction *in) {
  switch (in->op) {
  case IR_OP_BINARY:
    return binary_result_desc(fn, in);
  case IR_OP_UNARY: {
    SpvDesc s = operand_desc(fn, &in->lhs);
    SpvDesc r = {0};
    if (in->is_float) {
      r.kind = (in->float_bits == 32) ? MTLC_TYPE_FLOAT32 : MTLC_TYPE_FLOAT64;
    } else {
      r.kind = kind_bits(s.kind) == 64 ? MTLC_TYPE_INT64 : MTLC_TYPE_INT32;
      r.is_unsigned = s.is_unsigned;
    }
    return r;
  }
  case IR_OP_LOAD: {
    SpvDesc a = operand_desc(fn, &in->lhs);
    MtlcTypeKind elem = a.is_ptr ? a.elem : MTLC_TYPE_VOID;
    if (elem == MTLC_TYPE_VOID) {
      long long sz = (in->rhs.kind == IR_OPERAND_INT) ? in->rhs.int_value : 4;
      if (in->is_float) {
        elem = (sz == 4) ? MTLC_TYPE_FLOAT32 : MTLC_TYPE_FLOAT64;
      } else {
        elem = (sz == 8)   ? MTLC_TYPE_INT64
               : (sz == 2) ? MTLC_TYPE_INT16
               : (sz == 1) ? MTLC_TYPE_UINT8
                           : MTLC_TYPE_INT32;
      }
    }
    SpvDesc r = {0};
    r.kind = elem;
    r.is_unsigned = kind_is_unsigned(elem);
    return r;
  }
  case IR_OP_ASSIGN:
    return operand_desc(fn, &in->lhs);
  case IR_OP_CAST:
    return desc_from_typename(in->text);
  case IR_OP_CALL:
    return call_result_desc(in);
  default: {
    SpvDesc r = {0};
    r.kind = MTLC_TYPE_INT32;
    return r;
  }
  }
}

/* ---- conversions ---- */
/* convert value `id` of kind `from` to kind `to`; returns a new id (or the same
 * id when the representation is identical, e.g. int32 vs uint32). */
static uint32_t convert(SpvFn *fn, uint32_t id, MtlcTypeKind from, int from_uns,
                        MtlcTypeKind to) {
  SpvMod *m = fn->m;
  if (kind_type(m, from) == kind_type(m, to) &&
      kind_is_float(from) == kind_is_float(to)) {
    return id;
  }
  uint32_t tt = kind_type(m, to);
  uint32_t r = new_id(m);
  int ff = kind_is_float(from), tf = kind_is_float(to);
  uint16_t op;
  if (!ff && !tf) {
    op = from_uns ? Op_UConvert : Op_SConvert;
  } else if (!ff && tf) {
    op = from_uns ? Op_ConvertUToF : Op_ConvertSToF;
  } else if (ff && !tf) {
    op = kind_is_unsigned(to) ? Op_ConvertFToU : Op_ConvertFToS;
  } else {
    op = Op_FConvert;
  }
  emitv(&m->functions, op, 3, tt, r, id);
  return r;
}

/* Resolve an operand to a value id of kind `want`, materializing immediates and
 * inserting conversions (loads named values from their variables). */
static uint32_t materialize(SpvFn *fn, const IROperand *op, MtlcTypeKind want) {
  SpvMod *m = fn->m;
  if (op->kind == IR_OPERAND_INT) {
    if (kind_is_float(want)) {
      return const_float(m, want, (double)op->int_value);
    }
    return const_scalar_int(m, want, op->int_value);
  }
  if (op->kind == IR_OPERAND_FLOAT) {
    MtlcTypeKind src = (op->float_bits == 32) ? MTLC_TYPE_FLOAT32 : MTLC_TYPE_FLOAT64;
    uint32_t id = const_float(m, src, op->float_value);
    return convert(fn, id, src, 0, want);
  }
  SpvBind *b = find_bind(fn, op->name);
  if (!b) {
    mod_error(m, "SPIR-V: use of undefined value '%s'", op->name ? op->name : "?");
    return const_scalar_int(m, MTLC_TYPE_INT32, 0);
  }
  uint32_t ldt = kind_type(m, b->d.kind);
  uint32_t v = new_id(m);
  emitv(&m->functions, Op_Load, 3, ldt, v, b->var_id);
  return convert(fn, v, b->d.kind, b->d.is_unsigned, want);
}

static void store_name(SpvFn *fn, const char *name, uint32_t value) {
  SpvBind *b = find_bind(fn, name);
  if (!b) return;
  emitv(&fn->m->functions, Op_Store, 2, b->var_id, value);
}

/* ---- load/store through a computed pointer (carried as int64) ---- */
static MtlcTypeKind load_store_elem(SpvFn *fn, const IROperand *addr,
                                    const IRInstruction *in) {
  SpvDesc a = operand_desc(fn, addr);
  MtlcTypeKind elem = a.is_ptr ? a.elem : MTLC_TYPE_VOID;
  if (elem == MTLC_TYPE_VOID) {
    long long sz = (in->rhs.kind == IR_OPERAND_INT) ? in->rhs.int_value : 4;
    if (in->is_float) {
      elem = (sz == 4) ? MTLC_TYPE_FLOAT32 : MTLC_TYPE_FLOAT64;
    } else {
      elem = (sz == 8)   ? MTLC_TYPE_INT64
             : (sz == 2) ? MTLC_TYPE_INT16
             : (sz == 1) ? MTLC_TYPE_UINT8
                         : MTLC_TYPE_INT32;
    }
  }
  return elem;
}
static uint32_t typed_global_ptr(SpvFn *fn, uint32_t addr64, MtlcTypeKind elem) {
  SpvMod *m = fn->m;
  uint32_t pt = type_pointer(m, SC_CrossWorkgroup, kind_type(m, elem));
  uint32_t p = new_id(m);
  emitv(&m->functions, Op_ConvertUToPtr, 3, pt, p, addr64);
  return p;
}

/* ============================ op lowering ============================ */
static void emit_binary(SpvFn *fn, const IRInstruction *in) {
  SpvMod *m = fn->m;
  const char *t = in->text ? in->text : "+";
  SpvDesc la = operand_desc(fn, &in->lhs), ra = operand_desc(fn, &in->rhs);

  if (is_compare_op(t)) {
    int is_float = kind_is_float(la.kind) || kind_is_float(ra.kind) || in->is_float;
    MtlcTypeKind c;
    int uns = 0;
    if (is_float) {
      c = (la.kind == MTLC_TYPE_FLOAT64 || ra.kind == MTLC_TYPE_FLOAT64)
              ? MTLC_TYPE_FLOAT64
              : MTLC_TYPE_FLOAT32;
    } else {
      int b64 = kind_bits(la.kind) == 64 || kind_bits(ra.kind) == 64;
      uns = la.is_unsigned || ra.is_unsigned;
      c = b64 ? MTLC_TYPE_INT64 : MTLC_TYPE_INT32;
    }
    uint32_t a = materialize(fn, &in->lhs, c);
    uint32_t b = materialize(fn, &in->rhs, c);
    uint16_t op;
    if (is_float) {
      if (!strcmp(t, "==")) op = Op_FOrdEqual;
      else if (!strcmp(t, "!=")) op = Op_FUnordNotEqual;
      else if (!strcmp(t, "<")) op = Op_FOrdLessThan;
      else if (!strcmp(t, ">")) op = Op_FOrdGreaterThan;
      else if (!strcmp(t, "<=")) op = Op_FOrdLessThanEqual;
      else op = Op_FOrdGreaterThanEqual;
    } else {
      if (!strcmp(t, "==")) op = Op_IEqual;
      else if (!strcmp(t, "!=")) op = Op_INotEqual;
      else if (!strcmp(t, "<")) op = uns ? Op_ULessThan : Op_SLessThan;
      else if (!strcmp(t, ">")) op = uns ? Op_UGreaterThan : Op_SGreaterThan;
      else if (!strcmp(t, "<=")) op = uns ? Op_ULessThanEqual : Op_SLessThanEqual;
      else op = uns ? Op_UGreaterThanEqual : Op_SGreaterThanEqual;
    }
    uint32_t bt = type_bool(m);
    uint32_t cond = new_id(m);
    emitv(&m->functions, op, 4, bt, cond, a, b);
    /* materialize the boolean as a 0/1 int32 result (matches PTX selp) */
    uint32_t i32 = type_int(m, 32);
    uint32_t one = const_scalar_int(m, MTLC_TYPE_INT32, 1);
    uint32_t zero = const_scalar_int(m, MTLC_TYPE_INT32, 0);
    uint32_t res = new_id(m);
    emitv(&m->functions, Op_Select, 5, i32, res, cond, one, zero);
    if (in->dest.name) store_name(fn, in->dest.name, res);
    return;
  }

  SpvDesc rd = binary_result_desc(fn, in);
  MtlcTypeKind rk = rd.kind;
  uint32_t rt = kind_type(m, rk);
  int is_float = kind_is_float(rk);
  uint32_t a = materialize(fn, &in->lhs, rk);
  uint32_t b = materialize(fn, &in->rhs, rk);
  uint32_t res = new_id(m);
  uint16_t op = 0;

  if (!strcmp(t, "+")) op = is_float ? Op_FAdd : Op_IAdd;
  else if (!strcmp(t, "-")) op = is_float ? Op_FSub : Op_ISub;
  else if (!strcmp(t, "*")) op = is_float ? Op_FMul : Op_IMul;
  else if (!strcmp(t, "/")) op = is_float ? Op_FDiv : (rd.is_unsigned ? Op_UDiv : Op_SDiv);
  else if (!strcmp(t, "%")) op = is_float ? Op_FRem : (rd.is_unsigned ? Op_UMod : Op_SRem);
  else if (!strcmp(t, "&") || !strcmp(t, "&&")) op = Op_BitwiseAnd;
  else if (!strcmp(t, "|") || !strcmp(t, "||")) op = Op_BitwiseOr;
  else if (!strcmp(t, "^")) op = Op_BitwiseXor;
  else if (!strcmp(t, "<<")) op = Op_ShiftLeftLogical;
  else if (!strcmp(t, ">>")) op = la.is_unsigned ? Op_ShiftRightLogical : Op_ShiftRightArithmetic;
  else {
    mod_error(m, "SPIR-V: unsupported binary op '%s'", t);
    return;
  }
  emitv(&m->functions, op, 4, rt, res, a, b);
  if (in->dest.name) store_name(fn, in->dest.name, res);
}

static void emit_unary(SpvFn *fn, const IRInstruction *in) {
  SpvMod *m = fn->m;
  const char *t = in->text ? in->text : "";
  SpvDesc rd = result_desc(fn, in);
  MtlcTypeKind rk = rd.kind;
  uint32_t rt = kind_type(m, rk);
  int is_float = kind_is_float(rk);
  uint32_t s = materialize(fn, &in->lhs, rk);
  uint32_t res = new_id(m);
  if (!strcmp(t, "-")) {
    emitv(&m->functions, is_float ? Op_FNegate : Op_SNegate, 3, rt, res, s);
  } else if (!strcmp(t, "~")) {
    emitv(&m->functions, Op_Not, 3, rt, res, s);
  } else if (!strcmp(t, "!")) {
    uint32_t bt = type_bool(m);
    uint32_t zero = is_float ? const_float(m, rk, 0.0) : const_scalar_int(m, rk, 0);
    uint32_t cond = new_id(m);
    emitv(&m->functions, is_float ? Op_FOrdEqual : Op_IEqual, 4, bt, cond, s, zero);
    uint32_t one = const_scalar_int(m, MTLC_TYPE_INT32, 1);
    uint32_t z0 = const_scalar_int(m, MTLC_TYPE_INT32, 0);
    uint32_t i32 = type_int(m, 32);
    res = new_id(m);
    emitv(&m->functions, Op_Select, 5, i32, res, cond, one, z0);
  } else {
    mod_error(m, "SPIR-V: unsupported unary op '%s'", t);
    return;
  }
  if (in->dest.name) store_name(fn, in->dest.name, res);
}

static void emit_load(SpvFn *fn, const IRInstruction *in) {
  SpvMod *m = fn->m;
  MtlcTypeKind elem = load_store_elem(fn, &in->lhs, in);
  uint32_t addr = materialize(fn, &in->lhs, MTLC_TYPE_POINTER);
  uint32_t p = typed_global_ptr(fn, addr, elem);
  uint32_t et = kind_type(m, elem);
  uint32_t v = new_id(m);
  int align = kind_bits(elem) / 8;
  emitv(&m->functions, Op_Load, 5, et, v, p, (unsigned)MemAccess_Aligned,
        (unsigned)align);
  if (in->dest.name) store_name(fn, in->dest.name, v);
}

static void emit_store(SpvFn *fn, const IRInstruction *in) {
  SpvMod *m = fn->m;
  MtlcTypeKind elem = load_store_elem(fn, &in->dest, in);
  uint32_t addr = materialize(fn, &in->dest, MTLC_TYPE_POINTER);
  uint32_t val = materialize(fn, &in->lhs, elem);
  uint32_t p = typed_global_ptr(fn, addr, elem);
  int align = kind_bits(elem) / 8;
  emitv(&m->functions, Op_Store, 4, p, val, (unsigned)MemAccess_Aligned,
        (unsigned)align);
}

static void emit_cast(SpvFn *fn, const IRInstruction *in) {
  SpvDesc target = desc_from_typename(in->text);
  uint32_t id = materialize(fn, &in->lhs, target.kind);
  if (in->dest.name) store_name(fn, in->dest.name, id);
}

static void emit_call(SpvFn *fn, const IRInstruction *in) {
  SpvMod *m = fn->m;
  const char *callee = in->text;
  int bi = 0;
  int comp = sreg_component(callee, &bi);
  if (comp >= 0) {
    uint32_t var = builtin_var(m, bi);
    track_builtin(fn, var);
    uint32_t v3 = type_vec3_ulong(m);
    uint32_t u64 = type_int(m, 64);
    uint32_t loaded = new_id(m);
    emitv(&m->functions, Op_Load, 3, v3, loaded, var);
    uint32_t c = new_id(m);
    emitv(&m->functions, Op_CompositeExtract, 4, u64, c, loaded, (unsigned)comp);
    uint32_t u32 = type_int(m, 32);
    uint32_t r = new_id(m);
    emitv(&m->functions, Op_UConvert, 3, u32, r, c);
    if (in->dest.name) store_name(fn, in->dest.name, r);
    return;
  }
  if (callee && !strcmp(callee, "gpu_barrier")) {
    uint32_t sc = const_u32(m, Scope_Workgroup);
    uint32_t sem = const_u32(m, Sem_WorkgroupBarrier);
    emitv(&m->functions, Op_ControlBarrier, 3, sc, sc, sem);
    return;
  }
  if (callee && !strcmp(callee, "h2f") && in->argument_count >= 1) {
    /* reinterpret a uint16 fp16 bit-pattern (arrives zero-extended in an int)
     * as float32: truncate to 16 bits, bitcast to half, convert to float. */
    uint32_t a = materialize(fn, &in->arguments[0], MTLC_TYPE_UINT32);
    uint32_t i16 = type_int(m, 16);
    uint32_t h = type_float(m, 16);
    uint32_t f32 = type_float(m, 32);
    uint32_t t16 = new_id(m);
    emitv(&m->functions, Op_UConvert, 3, i16, t16, a);
    uint32_t half = new_id(m);
    emitv(&m->functions, Op_Bitcast, 3, h, half, t16);
    uint32_t r = new_id(m);
    emitv(&m->functions, Op_FConvert, 3, f32, r, half);
    if (in->dest.name) store_name(fn, in->dest.name, r);
    return;
  }
  if (callee && !strcmp(callee, "f2h") && in->argument_count >= 1) {
    uint32_t a = materialize(fn, &in->arguments[0], MTLC_TYPE_FLOAT32);
    uint32_t h = type_float(m, 16);
    uint32_t i16 = type_int(m, 16);
    uint32_t u32 = type_int(m, 32);
    uint32_t half = new_id(m);
    emitv(&m->functions, Op_FConvert, 3, h, half, a);
    uint32_t b16 = new_id(m);
    emitv(&m->functions, Op_Bitcast, 3, i16, b16, half);
    uint32_t r = new_id(m);
    emitv(&m->functions, Op_UConvert, 3, u32, r, b16);
    if (in->dest.name) store_name(fn, in->dest.name, r);
    return;
  }
  if (is_math_intrinsic(callee) && in->argument_count >= 1) {
    uint32_t a = materialize(fn, &in->arguments[0], MTLC_TYPE_FLOAT32);
    uint32_t f32 = type_float(m, 32);
    int num;
    if (!strcmp(callee, "sqrtf")) num = CL_sqrt;
    else if (!strcmp(callee, "rsqrtf")) num = CL_rsqrt;
    else if (!strcmp(callee, "fabsf")) num = CL_fabs;
    else if (!strcmp(callee, "sinf")) num = CL_sin;
    else if (!strcmp(callee, "cosf")) num = CL_cos;
    else if (!strcmp(callee, "logf")) num = CL_log;
    else num = CL_exp;
    uint32_t r = new_id(m);
    emitv(&m->functions, Op_ExtInst, 5, f32, r, m->opencl_ext, (unsigned)num, a);
    if (in->dest.name) store_name(fn, in->dest.name, r);
    return;
  }
  if (is_atomic_intrinsic(callee) && in->argument_count >= 3) {
    int is64 = !strcmp(callee, "atomic_min_u64");
    int isadd = !strcmp(callee, "atomic_add_u32");
    MtlcTypeKind vk = is64 ? MTLC_TYPE_UINT64 : MTLC_TYPE_UINT32;
    int elemsz = is64 ? 8 : 4;
    uint32_t buf = materialize(fn, &in->arguments[0], MTLC_TYPE_POINTER);
    uint32_t idx = materialize(fn, &in->arguments[1], MTLC_TYPE_INT64);
    uint32_t val = materialize(fn, &in->arguments[2], vk);
    uint32_t u64 = type_int(m, 64);
    uint32_t off = new_id(m);
    emitv(&m->functions, Op_IMul, 4, u64, off, idx,
          const_scalar_int(m, MTLC_TYPE_INT64, elemsz));
    uint32_t addr = new_id(m);
    emitv(&m->functions, Op_IAdd, 4, u64, addr, buf, off);
    uint32_t p = typed_global_ptr(fn, addr, vk);
    uint32_t vt = kind_type(m, vk);
    uint32_t sc = const_u32(m, Scope_Device);
    uint32_t sem = const_u32(m, Sem_None);
    uint32_t r = new_id(m);
    emitv(&m->functions, isadd ? Op_AtomicIAdd : Op_AtomicUMin, 6, vt, r, p, sc,
          sem, val);
    if (is64) m->use_atomics64 = 1;
    if (in->dest.name) store_name(fn, in->dest.name, r);
    return;
  }
  mod_error(m, "SPIR-V: unsupported call '%s'", callee ? callee : "?");
}

/* emit one non-terminator body instruction */
static void emit_body_instr(SpvFn *fn, const IRInstruction *in) {
  if (fn->m->error) return;
  switch (in->op) {
  case IR_OP_NOP:
  case IR_OP_LABEL:
  case IR_OP_DECLARE_LOCAL:
    break; /* declarations handled in the pre-pass */
  case IR_OP_ASSIGN: {
    /* store lhs into dest, coerced to dest's declared kind */
    SpvBind *db = find_bind(fn, in->dest.name);
    MtlcTypeKind dk = db ? db->d.kind : operand_desc(fn, &in->lhs).kind;
    uint32_t v = materialize(fn, &in->lhs, dk);
    if (in->dest.name) store_name(fn, in->dest.name, v);
    break;
  }
  case IR_OP_BINARY:
    emit_binary(fn, in);
    break;
  case IR_OP_UNARY:
    emit_unary(fn, in);
    break;
  case IR_OP_LOAD:
    emit_load(fn, in);
    break;
  case IR_OP_STORE:
    emit_store(fn, in);
    break;
  case IR_OP_CAST:
    emit_cast(fn, in);
    break;
  case IR_OP_CALL:
    emit_call(fn, in);
    break;
  case IR_OP_ADDRESS_OF:
    mod_error(fn->m, "SPIR-V: address-of (&local) not supported in kernels yet");
    break;
  default:
    mod_error(fn->m, "SPIR-V: unsupported IR opcode %d in kernel", in->op);
    break;
  }
}

/* ============================ structurizer ============================ */
typedef struct {
  size_t lo, hi;             /* instruction range [lo, hi) */
  const char *label;         /* leading label name, or NULL */
  const IRInstruction *term; /* terminator instruction, or NULL (fallthrough) */
} SpvBlock;

static int is_terminator(IROpcode op) {
  return op == IR_OP_JUMP || op == IR_OP_BRANCH_ZERO || op == IR_OP_BRANCH_EQ ||
         op == IR_OP_RETURN;
}

/* find the block index whose leading label matches `name` */
static long block_of_label(SpvBlock *blocks, size_t n, const char *name) {
  if (!name) return -1;
  for (size_t i = 0; i < n; i++) {
    if (blocks[i].label && strcmp(blocks[i].label, name) == 0) {
      return (long)i;
    }
  }
  return -1;
}

/* compute the branch condition of a conditional block as a scalar bool id.
 * BRANCH_ZERO: (cond != 0). BRANCH_EQ: (lhs == rhs). */
static uint32_t branch_cond_bool(SpvFn *fn, const IRInstruction *term) {
  SpvMod *m = fn->m;
  uint32_t bt = type_bool(m);
  uint32_t cond = new_id(m);
  if (term->op == IR_OP_BRANCH_ZERO) {
    SpvDesc d = operand_desc(fn, &term->lhs);
    if (kind_is_float(d.kind)) {
      uint32_t v = materialize(fn, &term->lhs, d.kind);
      uint32_t z = const_float(m, d.kind, 0.0);
      emitv(&m->functions, Op_FUnordNotEqual, 4, bt, cond, v, z);
    } else {
      MtlcTypeKind k = kind_bits(d.kind) == 64 ? MTLC_TYPE_INT64 : MTLC_TYPE_INT32;
      uint32_t v = materialize(fn, &term->lhs, k);
      uint32_t z = const_scalar_int(m, k, 0);
      emitv(&m->functions, Op_INotEqual, 4, bt, cond, v, z);
    }
  } else { /* BRANCH_EQ */
    SpvDesc la = operand_desc(fn, &term->lhs), ra = operand_desc(fn, &term->rhs);
    int is_float = kind_is_float(la.kind) || kind_is_float(ra.kind);
    MtlcTypeKind k =
        is_float ? ((la.kind == MTLC_TYPE_FLOAT64 || ra.kind == MTLC_TYPE_FLOAT64)
                        ? MTLC_TYPE_FLOAT64
                        : MTLC_TYPE_FLOAT32)
                 : (kind_bits(la.kind) == 64 || kind_bits(ra.kind) == 64
                        ? MTLC_TYPE_INT64
                        : MTLC_TYPE_INT32);
    uint32_t a = materialize(fn, &term->lhs, k);
    uint32_t b = materialize(fn, &term->rhs, k);
    emitv(&m->functions, is_float ? Op_FOrdEqual : Op_IEqual, 4, bt, cond, a, b);
  }
  return cond;
}

/* ---- pre-pass: register a descriptor for every named value ---- */
static void register_values(SpvFn *fn, IRFunction *func) {
  for (size_t p = 0; p < func->parameter_count; p++) {
    if (!func->parameter_names || !func->parameter_names[p]) continue;
    const char *tn = func->parameter_types ? func->parameter_types[p] : NULL;
    add_bind(fn, func->parameter_names[p], desc_from_typename(tn));
  }
  for (size_t i = 0; i < func->instruction_count; i++) {
    const IRInstruction *in = &func->instructions[i];
    if (in->op == IR_OP_DECLARE_LOCAL) {
      if (in->dest.name && !find_bind(fn, in->dest.name)) {
        add_bind(fn, in->dest.name, desc_from_typename(in->text));
      }
      continue;
    }
    if (!in->dest.name) continue;
    switch (in->op) {
    case IR_OP_BINARY:
    case IR_OP_UNARY:
    case IR_OP_LOAD:
    case IR_OP_ASSIGN:
    case IR_OP_CAST:
    case IR_OP_CALL:
      if (!find_bind(fn, in->dest.name)) {
        add_bind(fn, in->dest.name, result_desc(fn, in));
      }
      break;
    default:
      break;
    }
  }
}

/* ---- emit one kernel; returns its OpFunction id (0 on error) ---- */
static uint32_t emit_kernel(SpvMod *m, IRFunction *func) {
  SpvFn fn = {0};
  fn.m = m;

  /* ---- build basic blocks from the label/branch stream ---- */
  size_t count = func->instruction_count;
  size_t *starts = calloc(count + 1, sizeof(size_t));
  size_t nstarts = 0;
  starts[nstarts++] = 0;
  for (size_t i = 0; i < count; i++) {
    IROpcode op = func->instructions[i].op;
    if (op == IR_OP_LABEL && (nstarts == 0 || starts[nstarts - 1] != i)) {
      starts[nstarts++] = i;
    }
    if (is_terminator(op) && i + 1 < count &&
        (nstarts == 0 || starts[nstarts - 1] != i + 1)) {
      starts[nstarts++] = i + 1;
    }
  }
  SpvBlock *blocks = calloc(nstarts ? nstarts : 1, sizeof(SpvBlock));
  size_t nblocks = 0;
  for (size_t k = 0; k < nstarts; k++) {
    size_t lo = starts[k];
    size_t hi = (k + 1 < nstarts) ? starts[k + 1] : count;
    if (lo >= hi) continue;
    SpvBlock *bl = &blocks[nblocks++];
    bl->lo = lo;
    bl->hi = hi;
    bl->label = (func->instructions[lo].op == IR_OP_LABEL)
                    ? func->instructions[lo].text
                    : NULL;
    bl->term = is_terminator(func->instructions[hi - 1].op)
                   ? &func->instructions[hi - 1]
                   : NULL;
  }

  register_values(&fn, func);

  /* ---- ids for the dispatch skeleton ---- */
  uint32_t func_id = new_id(m);
  uint32_t entry_id = new_id(m);
  uint32_t header_id = new_id(m);
  uint32_t switch_id = new_id(m);
  uint32_t merge_id = new_id(m);
  uint32_t cont_id = new_id(m);
  uint32_t exit_id = new_id(m);
  uint32_t state_var = new_id(m);
  uint32_t *case_id = calloc(nblocks ? nblocks : 1, sizeof(uint32_t));
  for (size_t i = 0; i < nblocks; i++) case_id[i] = new_id(m);

  /* ---- parameter + function types ---- */
  uint32_t voidt = type_void(m);
  uint32_t *ptypes = calloc(func->parameter_count + 1, sizeof(uint32_t));
  uint32_t *pids = calloc(func->parameter_count + 1, sizeof(uint32_t));
  SpvDesc *pdesc = calloc(func->parameter_count + 1, sizeof(SpvDesc));
  for (size_t p = 0; p < func->parameter_count; p++) {
    const char *tn = func->parameter_types ? func->parameter_types[p] : NULL;
    SpvDesc d = desc_from_typename(tn);
    pdesc[p] = d;
    if (d.is_ptr) {
      ptypes[p] = type_pointer(m, SC_CrossWorkgroup, kind_type(m, d.elem));
    } else {
      ptypes[p] = kind_type(m, d.kind);
    }
    pids[p] = new_id(m);
  }
  Wb ftops = {0};
  wb_push(&ftops, voidt);
  for (size_t p = 0; p < func->parameter_count; p++) wb_push(&ftops, ptypes[p]);
  /* intern the function type by structural key */
  char ftkey[256];
  int kn = snprintf(ftkey, sizeof(ftkey), "fn:%u", voidt);
  for (size_t p = 0; p < func->parameter_count && kn < (int)sizeof(ftkey) - 12; p++) {
    kn += snprintf(ftkey + kn, sizeof(ftkey) - (size_t)kn, ":%u", ptypes[p]);
  }
  uint32_t functype = cache_get(m, ftkey);
  if (!functype) {
    functype = new_id(m);
    Wb ft = {0};
    wb_push(&ft, functype);
    wb_push(&ft, voidt);
    for (size_t p = 0; p < func->parameter_count; p++) wb_push(&ft, ptypes[p]);
    emit_ops(&m->typesconsts, Op_TypeFunction, &ft);
    wb_free(&ft);
    cache_put(m, ftkey, functype);
  }
  wb_free(&ftops);

  /* ---- OpFunction + parameters ---- */
  emitv(&m->functions, Op_Function, 4, voidt, func_id, 0u, functype);
  for (size_t p = 0; p < func->parameter_count; p++) {
    emitv(&m->functions, Op_FunctionParameter, 2, ptypes[p], pids[p]);
  }

  /* ---- entry block: declare variables, shadow params, init state ---- */
  emitv(&m->functions, Op_Label, 1, entry_id);
  uint32_t u32t = type_int(m, 32);
  uint32_t state_ptr_t = type_pointer(m, SC_Function, u32t);
  emitv(&m->functions, Op_Variable, 3, state_ptr_t, state_var, (unsigned)SC_Function);
  for (size_t i = 0; i < fn.nbinds; i++) {
    SpvBind *b = &fn.binds[i];
    uint32_t vt = kind_type(m, b->d.kind);
    uint32_t pt = type_pointer(m, SC_Function, vt);
    b->var_id = new_id(m);
    emitv(&m->functions, Op_Variable, 3, pt, b->var_id, (unsigned)SC_Function);
  }
  /* store incoming parameters into their shadow variables */
  for (size_t p = 0; p < func->parameter_count; p++) {
    if (!func->parameter_names || !func->parameter_names[p]) continue;
    SpvBind *b = find_bind(&fn, func->parameter_names[p]);
    if (!b) continue;
    if (pdesc[p].is_ptr) {
      uint32_t u64 = type_int(m, 64);
      uint32_t asint = new_id(m);
      emitv(&m->functions, Op_ConvertPtrToU, 3, u64, asint, pids[p]);
      emitv(&m->functions, Op_Store, 2, b->var_id, asint);
    } else {
      emitv(&m->functions, Op_Store, 2, b->var_id, pids[p]);
    }
  }
  emitv(&m->functions, Op_Store, 2, state_var, const_u32(m, 0));
  emitv(&m->functions, Op_Branch, 1, header_id);

  /* ---- loop header + switch dispatch ---- */
  emitv(&m->functions, Op_Label, 1, header_id);
  emitv(&m->functions, Op_LoopMerge, 3, exit_id, cont_id, 0u);
  emitv(&m->functions, Op_Branch, 1, switch_id);
  emitv(&m->functions, Op_Label, 1, switch_id);
  uint32_t st = new_id(m);
  emitv(&m->functions, Op_Load, 3, u32t, st, state_var);
  emitv(&m->functions, Op_SelectionMerge, 2, merge_id, 0u);
  {
    Wb sw = {0};
    wb_push(&sw, st);
    wb_push(&sw, exit_id); /* default: fall out of the loop */
    for (size_t i = 0; i < nblocks; i++) {
      wb_push(&sw, (uint32_t)i);
      wb_push(&sw, case_id[i]);
    }
    emit_ops(&m->functions, Op_Switch, &sw);
    wb_free(&sw);
  }

  /* ---- one SPIR-V block per IR block ---- */
  for (size_t i = 0; i < nblocks && !m->error; i++) {
    SpvBlock *bl = &blocks[i];
    emitv(&m->functions, Op_Label, 1, case_id[i]);
    for (size_t j = bl->lo; j < bl->hi && !m->error; j++) {
      const IRInstruction *in = &func->instructions[j];
      if (in == bl->term) break; /* terminator handled below */
      emit_body_instr(&fn, in);
    }
    /* successor: set state and branch to the selection merge (or the loop
     * exit for returns / the fall-off-the-end case). */
    const IRInstruction *term = bl->term;
    if (term && term->op == IR_OP_RETURN) {
      emitv(&m->functions, Op_Branch, 1, exit_id);
    } else if (term && term->op == IR_OP_JUMP) {
      long t = block_of_label(blocks, nblocks, term->text);
      if (t < 0) {
        mod_error(m, "SPIR-V: jump to unknown label '%s'",
                  term->text ? term->text : "?");
        break;
      }
      emitv(&m->functions, Op_Store, 2, state_var, const_u32(m, (uint32_t)t));
      emitv(&m->functions, Op_Branch, 1, merge_id);
    } else if (term &&
               (term->op == IR_OP_BRANCH_ZERO || term->op == IR_OP_BRANCH_EQ)) {
      long taken = block_of_label(blocks, nblocks, term->text);
      long fall = (i + 1 < nblocks) ? (long)(i + 1) : -1;
      if (taken < 0) {
        mod_error(m, "SPIR-V: branch to unknown label '%s'",
                  term->text ? term->text : "?");
        break;
      }
      uint32_t fall_id = const_u32(m, fall >= 0 ? (uint32_t)fall : 0);
      uint32_t taken_id = const_u32(m, (uint32_t)taken);
      uint32_t cond = branch_cond_bool(&fn, term);
      /* BRANCH_ZERO: take target when cond is false -> select(cond, fall, take).
       * BRANCH_EQ:   take target when cond is true  -> select(cond, take, fall). */
      uint32_t a = (term->op == IR_OP_BRANCH_ZERO) ? fall_id : taken_id;
      uint32_t b = (term->op == IR_OP_BRANCH_ZERO) ? taken_id : fall_id;
      uint32_t next = new_id(m);
      emitv(&m->functions, Op_Select, 5, u32t, next, cond, a, b);
      if (fall < 0) {
        /* no fall-through block: a false BRANCH_ZERO / false BRANCH_EQ falls
         * off the end -> exit. Store then branch; the exit path is fine. */
      }
      emitv(&m->functions, Op_Store, 2, state_var, next);
      emitv(&m->functions, Op_Branch, 1, merge_id);
    } else {
      /* fallthrough: advance to the next block, or exit at the end */
      if (i + 1 < nblocks) {
        emitv(&m->functions, Op_Store, 2, state_var,
              const_u32(m, (uint32_t)(i + 1)));
        emitv(&m->functions, Op_Branch, 1, merge_id);
      } else {
        emitv(&m->functions, Op_Branch, 1, exit_id);
      }
    }
  }

  /* ---- selection merge -> loop continue -> back-edge; then exit ---- */
  emitv(&m->functions, Op_Label, 1, merge_id);
  emitv(&m->functions, Op_Branch, 1, cont_id);
  emitv(&m->functions, Op_Label, 1, cont_id);
  emitv(&m->functions, Op_Branch, 1, header_id);
  emitv(&m->functions, Op_Label, 1, exit_id);
  emitv(&m->functions, Op_Return, 0);
  emitv(&m->functions, Op_FunctionEnd, 0);

  /* ---- entry point (with the BuiltIn Input vars this kernel touched) ---- */
  if (!m->error) {
    Wb ep = {0};
    wb_push(&ep, (uint32_t)ExecModel_Kernel);
    wb_push(&ep, func_id);
    wb_str(&ep, func->name ? func->name : "kernel");
    for (size_t i = 0; i < fn.nused_builtins; i++) {
      wb_push(&ep, fn.used_builtins[i]);
    }
    emit_ops(&m->entrypoints, Op_EntryPoint, &ep);
    wb_free(&ep);
  }

  for (size_t i = 0; i < fn.nbinds; i++) free(fn.binds[i].name);
  free(fn.binds);
  free(starts);
  free(blocks);
  free(case_id);
  free(ptypes);
  free(pids);
  free(pdesc);
  return m->error ? 0 : func_id;
}

/* ============================ driver ============================ */
static void write_word_le(FILE *out, uint32_t w) {
  fputc((int)(w & 0xff), out);
  fputc((int)((w >> 8) & 0xff), out);
  fputc((int)((w >> 16) & 0xff), out);
  fputc((int)((w >> 24) & 0xff), out);
}
static void write_section(FILE *out, const Wb *w) {
  for (size_t i = 0; i < w->len; i++) write_word_le(out, w->data[i]);
}

int spirv_emit_program(IRProgram *program, CodeGenerator *generator, FILE *out,
                       char **error) {
  (void)generator;
  if (error) *error = NULL;
  if (!program || !out) {
    if (error) *error = strdup("spirv_emit_program: null program/out");
    return 0;
  }

  SpvMod m = {0};
  m.next_id = 1;

  /* OpenCL.std extended instruction set (imported once, used by the f32 math
   * intrinsics; harmless if unused). */
  m.opencl_ext = new_id(&m);
  {
    Wb ei = {0};
    wb_push(&ei, m.opencl_ext);
    wb_str(&ei, "OpenCL.std");
    emit_ops(&m.extimports, Op_ExtInstImport, &ei);
    wb_free(&ei);
  }

  for (size_t i = 0; i < program->function_count && !m.error; i++) {
    emit_kernel(&m, program->functions[i]);
  }

  if (m.error) {
    if (error) *error = m.error;
    else free(m.error);
    goto cleanup;
  }

  /* capabilities depend on which widths/features the module actually used */
  emitv(&m.caps, Op_Capability, 1, (unsigned)Cap_Addresses);
  emitv(&m.caps, Op_Capability, 1, (unsigned)Cap_Kernel);
  emitv(&m.caps, Op_Capability, 1, (unsigned)Cap_Int64);
  if (m.use_int8) emitv(&m.caps, Op_Capability, 1, (unsigned)Cap_Int8);
  if (m.use_int16) emitv(&m.caps, Op_Capability, 1, (unsigned)Cap_Int16);
  if (m.use_float16) emitv(&m.caps, Op_Capability, 1, (unsigned)Cap_Float16);
  if (m.use_float64) emitv(&m.caps, Op_Capability, 1, (unsigned)Cap_Float64);
  if (m.use_atomics64) emitv(&m.caps, Op_Capability, 1, (unsigned)Cap_Int64Atomics);

  emitv(&m.memmodel, Op_MemoryModel, 2, (unsigned)AddrModel_Physical64,
        (unsigned)MemModel_OpenCL);

  /* ---- assemble the module in mandated layout order ---- */
  write_word_le(out, SPV_MAGIC);
  write_word_le(out, SPV_VERSION_1_0);
  write_word_le(out, 0u);         /* generator magic (0 = none) */
  write_word_le(out, m.next_id);  /* id bound */
  write_word_le(out, 0u);         /* schema */
  write_section(out, &m.caps);
  write_section(out, &m.extimports);
  write_section(out, &m.memmodel);
  write_section(out, &m.entrypoints);
  write_section(out, &m.decorations);
  write_section(out, &m.typesconsts);
  write_section(out, &m.functions);

cleanup:
  wb_free(&m.caps);
  wb_free(&m.extimports);
  wb_free(&m.memmodel);
  wb_free(&m.entrypoints);
  wb_free(&m.decorations);
  wb_free(&m.typesconsts);
  wb_free(&m.functions);
  for (size_t i = 0; i < m.ncache; i++) free(m.cache[i].key);
  free(m.cache);
  return m.error ? 0 : 1;
}
