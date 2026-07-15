/* mtlc_build.c - implementation of the public IR builder (include/mtlc/build.h).
 *
 * Part of libmtlc. This is the frontend-agnostic path for CONSTRUCTING IR: it
 * turns the opaque builder calls into a backend IRProgram, then populates the
 * module type registry and symbol table the code generators read -- doing, for
 * an arbitrary frontend, exactly what src/frontend/mtlc_lower_module.c does for
 * the reference Mettle frontend, but from the builder's own declared types
 * instead of a frontend AST. It is frontend-free: it includes only the public
 * mtlc/ headers and the backend IR. */
#include "mtlc/build.h"
#include "mtlc/module.h"

#include "common.h"
#include "ir/ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A function declaration recorded for the module symbol table. Bodies (when not
 * extern) are emitted directly into `ir`. */
typedef struct {
  char *name;
  const MtlcType *return_type;
  char **param_names;         /* owned copies */
  const MtlcType **param_types; /* borrowed (immortal singletons) */
  size_t param_count;
  int is_extern;
  int has_body;
} FnDecl;

struct MtlcFn {
  IRFunction *ir;      /* borrowed; owned by the program */
  MtlcBuilder *builder;
  const MtlcType *return_type;
  char **param_names;  /* borrowed from the FnDecl */
  size_t param_count;
  IROperand *values;   /* value-handle table; each owns its operand */
  size_t value_count, value_capacity;
};

/* A module-level global variable declaration. */
typedef struct {
  char *name;
  const MtlcType *type;
  long long init_value;
  int is_extern;
} GlobalDecl;

struct MtlcBuilder {
  IRProgram *program;  /* owned until finish */
  MtlcFn **fns;        /* body builders (non-extern), owned */
  size_t fn_count, fn_capacity;
  FnDecl *decls;       /* every declaration, owned */
  size_t decl_count, decl_capacity;
  GlobalDecl *globals; /* module globals, owned */
  size_t global_count, global_capacity;
  /* every distinct MtlcType* that passed through the builder API, registered
   * by name into the module type registry at finish so codegen can resolve
   * any type NAME the IR carries (parameter types, DECLARE_LOCAL/CAST text) */
  const MtlcType **seen_types;
  size_t seen_count, seen_capacity;
  int temp_counter;
  int error;
};

/* The parseable NAME of a type: its canonical name when set (scalars carry
 * "int64", interned pointers carry "int64*"), else the kind name. */
static const char *type_name(const MtlcType *t) {
  if (!t) {
    return "int64";
  }
  return t->name ? t->name : mtlc_type_kind_name(t->kind);
}

static void record_type(MtlcBuilder *b, const MtlcType *t) {
  if (!b || !t) {
    return;
  }
  for (size_t i = 0; i < b->seen_count; i++) {
    if (b->seen_types[i] == t) {
      return;
    }
  }
  if (b->seen_count == b->seen_capacity) {
    size_t next = b->seen_capacity ? b->seen_capacity * 2 : 16;
    const MtlcType **grown =
        realloc(b->seen_types, next * sizeof(*b->seen_types));
    if (!grown) {
      b->error = 1;
      return;
    }
    b->seen_types = grown;
    b->seen_capacity = next;
  }
  b->seen_types[b->seen_count++] = t;
  /* pointer chains: seeing "int64*" implies "int64" should resolve too */
  if (t->base_type) {
    record_type(b, t->base_type);
  }
}

/* ------------------------------------------------------------------ builder */

MtlcBuilder *mtlc_builder_create(void) {
  MtlcBuilder *b = (MtlcBuilder *)calloc(1, sizeof(MtlcBuilder));
  if (!b) {
    return NULL;
  }
  b->program = ir_program_create();
  if (!b->program) {
    free(b);
    return NULL;
  }
  return b;
}

static void fn_destroy(MtlcFn *fn) {
  if (!fn) {
    return;
  }
  for (size_t i = 0; i < fn->value_count; i++) {
    ir_operand_destroy(&fn->values[i]);
  }
  free(fn->values);
  free(fn);
}

void mtlc_builder_destroy(MtlcBuilder *builder) {
  if (!builder) {
    return;
  }
  for (size_t i = 0; i < builder->fn_count; i++) {
    fn_destroy(builder->fns[i]);
  }
  free(builder->fns);
  for (size_t i = 0; i < builder->decl_count; i++) {
    free(builder->decls[i].name);
    for (size_t p = 0; p < builder->decls[i].param_count; p++) {
      free(builder->decls[i].param_names[p]);
    }
    free(builder->decls[i].param_names);
    free(builder->decls[i].param_types);
  }
  free(builder->decls);
  for (size_t i = 0; i < builder->global_count; i++) {
    free(builder->globals[i].name);
  }
  free(builder->globals);
  free(builder->seen_types);
  if (builder->program) {
    ir_program_destroy(builder->program);
  }
  free(builder);
}

void mtlc_builder_global(MtlcBuilder *builder, const char *name,
                        const MtlcType *type, long long init_value,
                        int is_extern) {
  if (!builder || builder->error || !name || !type) {
    if (builder) {
      builder->error = 1;
    }
    return;
  }
  if (builder->global_count == builder->global_capacity) {
    size_t next = builder->global_capacity ? builder->global_capacity * 2 : 8;
    GlobalDecl *grown = realloc(builder->globals, next * sizeof(GlobalDecl));
    if (!grown) {
      builder->error = 1;
      return;
    }
    builder->globals = grown;
    builder->global_capacity = next;
  }
  GlobalDecl *g = &builder->globals[builder->global_count++];
  g->name = mettle_strdup(name);
  g->type = type;
  g->init_value = init_value;
  g->is_extern = is_extern ? 1 : 0;
  record_type(builder, type);
}

MtlcFn *mtlc_builder_function(MtlcBuilder *builder, const char *name,
                             const MtlcType *return_type,
                             const char *const *param_names,
                             const MtlcType *const *param_types,
                             size_t param_count, int is_extern) {
  if (!builder || builder->error || !name || !return_type) {
    return NULL;
  }

  /* record the declaration (used at finish for the module symbol table) */
  if (builder->decl_count == builder->decl_capacity) {
    size_t next = builder->decl_capacity ? builder->decl_capacity * 2 : 8;
    FnDecl *grown = realloc(builder->decls, next * sizeof(FnDecl));
    if (!grown) {
      builder->error = 1;
      return NULL;
    }
    builder->decls = grown;
    builder->decl_capacity = next;
  }
  FnDecl *decl = &builder->decls[builder->decl_count];
  memset(decl, 0, sizeof(*decl));
  decl->name = mettle_strdup(name);
  decl->return_type = return_type;
  decl->param_count = param_count;
  decl->is_extern = is_extern ? 1 : 0;
  decl->has_body = is_extern ? 0 : 1;
  if (param_count > 0) {
    decl->param_names = calloc(param_count, sizeof(char *));
    decl->param_types = calloc(param_count, sizeof(MtlcType *));
    for (size_t i = 0; i < param_count; i++) {
      decl->param_names[i] =
          mettle_strdup(param_names && param_names[i] ? param_names[i] : "");
      decl->param_types[i] = param_types ? param_types[i] : NULL;
    }
  }
  builder->decl_count++;

  if (is_extern) {
    return NULL; /* body-less: nothing to emit into */
  }

  /* create the IR function and set its signature by type NAME (resolved against
   * the type registry at codegen time) */
  IRFunction *irf = ir_function_create(name);
  if (!irf) {
    builder->error = 1;
    return NULL;
  }
  if (param_count > 0) {
    const char **type_names = calloc(param_count, sizeof(char *));
    for (size_t i = 0; i < param_count; i++) {
      type_names[i] = type_name(param_types ? param_types[i] : NULL);
      if (param_types && param_types[i]) {
        record_type(builder, param_types[i]);
      }
    }
    ir_function_set_parameters(irf, (const char **)decl->param_names,
                               type_names, param_count);
    free(type_names);
  }
  irf->return_type_name = mettle_strdup(type_name(return_type));
  record_type(builder, return_type);
  if (!ir_program_add_function(builder->program, irf)) {
    ir_function_destroy(irf);
    builder->error = 1;
    return NULL;
  }

  MtlcFn *fn = calloc(1, sizeof(MtlcFn));
  if (!fn) {
    builder->error = 1;
    return NULL;
  }
  fn->ir = irf;
  fn->builder = builder;
  fn->return_type = return_type;
  fn->param_names = decl->param_names;
  fn->param_count = param_count;

  if (builder->fn_count == builder->fn_capacity) {
    size_t next = builder->fn_capacity ? builder->fn_capacity * 2 : 8;
    MtlcFn **grown = realloc(builder->fns, next * sizeof(MtlcFn *));
    if (!grown) {
      fn_destroy(fn);
      builder->error = 1;
      return NULL;
    }
    builder->fns = grown;
    builder->fn_capacity = next;
  }
  builder->fns[builder->fn_count++] = fn;
  return fn;
}

/* ------------------------------------------------------------------- values */

static MtlcValue push_value(MtlcFn *fn, IROperand op) {
  if (fn->value_count == fn->value_capacity) {
    size_t next = fn->value_capacity ? fn->value_capacity * 2 : 16;
    IROperand *grown = realloc(fn->values, next * sizeof(IROperand));
    if (!grown) {
      ir_operand_destroy(&op);
      fn->builder->error = 1;
      return MTLC_NO_VALUE;
    }
    fn->values = grown;
    fn->value_capacity = next;
  }
  fn->values[fn->value_count] = op; /* takes ownership of op's name */
  return (MtlcValue)fn->value_count++;
}

static const IROperand *value_operand(MtlcFn *fn, MtlcValue v) {
  if (v < 0 || (size_t)v >= fn->value_count) {
    return NULL;
  }
  return &fn->values[v];
}

/* Append a stack-built instruction. Its dest/lhs/rhs/arguments operands are
 * shallow copies that ALIAS handle-table entries (or borrowed literals); the
 * append clones every operand and strdup's the text, so the stack instruction
 * is discarded without a destroy. */
static void emit(MtlcFn *fn, const IRInstruction *inst) {
  if (fn->builder->error) {
    return;
  }
  if (!ir_function_append_instruction(fn->ir, inst)) {
    fn->builder->error = 1;
  }
}

static MtlcValue fresh_temp(MtlcFn *fn) {
  char name[32];
  snprintf(name, sizeof(name), ".t%d", fn->builder->temp_counter++);
  return push_value(fn, ir_operand_temp(name));
}

MtlcValue mtlc_fn_param(MtlcFn *fn, size_t index) {
  if (!fn || index >= fn->param_count) {
    if (fn) {
      fn->builder->error = 1;
    }
    return MTLC_NO_VALUE;
  }
  return push_value(fn, ir_operand_symbol(fn->param_names[index]));
}

MtlcValue mtlc_const_int(MtlcFn *fn, const MtlcType *type, long long value) {
  (void)type; /* an int literal carries its width at its use site */
  if (!fn) {
    return MTLC_NO_VALUE;
  }
  return push_value(fn, ir_operand_int(value));
}

MtlcValue mtlc_local(MtlcFn *fn, const char *name, const MtlcType *type) {
  if (!fn || !name || !type) {
    if (fn) {
      fn->builder->error = 1;
    }
    return MTLC_NO_VALUE;
  }
  record_type(fn->builder, type);
  MtlcValue res = push_value(fn, ir_operand_symbol(name));
  const IROperand *dest = value_operand(fn, res);
  if (!dest) {
    return MTLC_NO_VALUE;
  }
  IRInstruction inst = {0};
  inst.op = IR_OP_DECLARE_LOCAL;
  inst.dest = *dest;
  inst.text = (char *)type_name(type);
  inst.value_type = (MtlcType *)type;
  emit(fn, &inst);
  return res;
}

MtlcValue mtlc_global_ref(MtlcFn *fn, const char *name) {
  if (!fn || !name) {
    if (fn) {
      fn->builder->error = 1;
    }
    return MTLC_NO_VALUE;
  }
  return push_value(fn, ir_operand_symbol(name));
}

MtlcValue mtlc_const_float(MtlcFn *fn, const MtlcType *type, double value) {
  if (!fn || !type) {
    if (fn) {
      fn->builder->error = 1;
    }
    return MTLC_NO_VALUE;
  }
  int bits = (type->kind == MTLC_TYPE_FLOAT32) ? 32 : 64;
  return push_value(fn, ir_operand_float_sized(value, bits));
}

/* --------------------------------------------------------------- instructions */

void mtlc_assign(MtlcFn *fn, MtlcValue dest, MtlcValue value) {
  if (!fn) {
    return;
  }
  const IROperand *d = value_operand(fn, dest);
  const IROperand *v = value_operand(fn, value);
  if (!d || !v) {
    fn->builder->error = 1;
    return;
  }
  IRInstruction inst = {0};
  inst.op = IR_OP_ASSIGN;
  inst.dest = *d;
  inst.lhs = *v;
  emit(fn, &inst);
}

MtlcValue mtlc_binary(MtlcFn *fn, const char *op, MtlcValue lhs, MtlcValue rhs,
                     const MtlcType *result_type) {
  if (!fn || !op || !result_type) {
    if (fn) {
      fn->builder->error = 1;
    }
    return MTLC_NO_VALUE;
  }
  const IROperand *l = value_operand(fn, lhs);
  const IROperand *r = value_operand(fn, rhs);
  if (!l || !r) {
    fn->builder->error = 1;
    return MTLC_NO_VALUE;
  }
  record_type(fn->builder, result_type);
  /* copy operands before push_value may realloc the table out from under them */
  IROperand lc = *l, rc = *r;
  MtlcValue res = fresh_temp(fn);
  const IROperand *dest = value_operand(fn, res);
  if (!dest) {
    return MTLC_NO_VALUE;
  }
  IRInstruction inst = {0};
  inst.op = IR_OP_BINARY;
  inst.dest = *dest;
  inst.lhs = lc;
  inst.rhs = rc;
  inst.text = (char *)op;
  inst.value_type = (MtlcType *)result_type;
  if (mtlc_type_is_float(result_type)) {
    inst.is_float = 1;
    inst.float_bits = (int)(result_type->size * 8);
  } else if (mtlc_type_is_unsigned(result_type)) {
    /* unsigned result type selects unsigned / % >> and compares */
    inst.is_unsigned = 1;
  }
  emit(fn, &inst);
  return res;
}

MtlcValue mtlc_unary(MtlcFn *fn, const char *op, MtlcValue operand,
                    const MtlcType *result_type) {
  if (!fn || !op || !result_type) {
    if (fn) {
      fn->builder->error = 1;
    }
    return MTLC_NO_VALUE;
  }
  const IROperand *o = value_operand(fn, operand);
  if (!o) {
    fn->builder->error = 1;
    return MTLC_NO_VALUE;
  }
  IROperand oc = *o;
  MtlcValue res = fresh_temp(fn);
  const IROperand *dest = value_operand(fn, res);
  if (!dest) {
    return MTLC_NO_VALUE;
  }
  IRInstruction inst = {0};
  inst.op = IR_OP_UNARY;
  inst.dest = *dest;
  inst.lhs = oc;
  inst.text = (char *)op;
  inst.value_type = (MtlcType *)result_type;
  if (mtlc_type_is_float(result_type)) {
    inst.is_float = 1;
    inst.float_bits = (int)(result_type->size * 8);
  }
  emit(fn, &inst);
  return res;
}

MtlcValue mtlc_call(MtlcFn *fn, const char *callee, const MtlcValue *args,
                   size_t arg_count, const MtlcType *return_type) {
  if (!fn || !callee || !return_type) {
    if (fn) {
      fn->builder->error = 1;
    }
    return MTLC_NO_VALUE;
  }
  IROperand *argv = NULL;
  if (arg_count > 0) {
    argv = calloc(arg_count, sizeof(IROperand));
    if (!argv) {
      fn->builder->error = 1;
      return MTLC_NO_VALUE;
    }
    for (size_t i = 0; i < arg_count; i++) {
      const IROperand *a = value_operand(fn, args[i]);
      if (!a) {
        free(argv);
        fn->builder->error = 1;
        return MTLC_NO_VALUE;
      }
      argv[i] = *a; /* shallow alias; append clones */
    }
  }
  record_type(fn->builder, return_type);
  int is_void = (return_type->kind == MTLC_TYPE_VOID);
  MtlcValue res = is_void ? MTLC_NO_VALUE : fresh_temp(fn);
  IRInstruction inst = {0};
  inst.op = IR_OP_CALL;
  if (!is_void) {
    const IROperand *dest = value_operand(fn, res);
    if (dest) {
      inst.dest = *dest;
    }
  }
  inst.text = (char *)callee;
  inst.arguments = argv;
  inst.argument_count = arg_count;
  inst.value_type = (MtlcType *)return_type;
  emit(fn, &inst);
  free(argv); /* elements were cloned by append; free the container only */
  return res;
}

/* Real address of a function symbol (defined here or a declared extern):
 * IR_OP_ADDRESS_OF on a function name lowers to a RIP-relative lea with a
 * relocation, so the value is callable by the OS/CRT and comparable. */
MtlcValue mtlc_function_address(MtlcFn *fn, const char *name) {
  if (!fn || !name) {
    if (fn) {
      fn->builder->error = 1;
    }
    return MTLC_NO_VALUE;
  }
  MtlcValue res = fresh_temp(fn);
  const IROperand *dest = value_operand(fn, res);
  if (!dest) {
    return MTLC_NO_VALUE;
  }
  IRInstruction inst = {0};
  inst.op = IR_OP_ADDRESS_OF;
  inst.dest = *dest;
  inst.lhs = ir_operand_symbol(name);
  if (inst.lhs.kind != IR_OPERAND_SYMBOL) {
    fn->builder->error = 1;
    return MTLC_NO_VALUE;
  }
  emit(fn, &inst);
  ir_operand_destroy(&inst.lhs);
  return res;
}

/* Call through a function-pointer VALUE (e.g. one produced by
 * mtlc_function_address or loaded from memory). Arguments follow the same
 * ABI classification as direct calls; without a typed fn-pointer symbol the
 * backend classifies every argument as integer/pointer. */
MtlcValue mtlc_call_indirect(MtlcFn *fn, MtlcValue callee,
                             const MtlcValue *args, size_t arg_count,
                             const MtlcType *return_type) {
  if (!fn || !return_type) {
    if (fn) {
      fn->builder->error = 1;
    }
    return MTLC_NO_VALUE;
  }
  const IROperand *code = value_operand(fn, callee);
  if (!code) {
    fn->builder->error = 1;
    return MTLC_NO_VALUE;
  }
  IROperand code_copy = *code;
  IROperand *argv = NULL;
  if (arg_count > 0) {
    argv = calloc(arg_count, sizeof(IROperand));
    if (!argv) {
      fn->builder->error = 1;
      return MTLC_NO_VALUE;
    }
    for (size_t i = 0; i < arg_count; i++) {
      const IROperand *a = value_operand(fn, args[i]);
      if (!a) {
        free(argv);
        fn->builder->error = 1;
        return MTLC_NO_VALUE;
      }
      argv[i] = *a; /* shallow alias; append clones */
    }
  }
  record_type(fn->builder, return_type);
  int is_void = (return_type->kind == MTLC_TYPE_VOID);
  MtlcValue res = is_void ? MTLC_NO_VALUE : fresh_temp(fn);
  IRInstruction inst = {0};
  inst.op = IR_OP_CALL_INDIRECT;
  if (!is_void) {
    const IROperand *dest = value_operand(fn, res);
    if (dest) {
      inst.dest = *dest;
    }
  }
  inst.lhs = code_copy;
  inst.arguments = argv;
  inst.argument_count = arg_count;
  inst.value_type = (MtlcType *)return_type;
  emit(fn, &inst);
  free(argv);
  return res;
}

MtlcValue mtlc_cast(MtlcFn *fn, MtlcValue value, const MtlcType *type) {
  if (!fn || !type) {
    if (fn) {
      fn->builder->error = 1;
    }
    return MTLC_NO_VALUE;
  }
  const IROperand *v = value_operand(fn, value);
  if (!v) {
    fn->builder->error = 1;
    return MTLC_NO_VALUE;
  }
  record_type(fn->builder, type);
  IROperand vc = *v;
  MtlcValue res = fresh_temp(fn);
  const IROperand *dest = value_operand(fn, res);
  if (!dest) {
    return MTLC_NO_VALUE;
  }
  IRInstruction inst = {0};
  inst.op = IR_OP_CAST;
  inst.dest = *dest;
  inst.lhs = vc;
  inst.text = (char *)type_name(type);
  inst.value_type = (MtlcType *)type;
  if (mtlc_type_is_float(type)) {
    inst.is_float = 1;
    inst.float_bits = (int)(type->size * 8);
    fn->values[res].float_bits = inst.float_bits;
  }
  emit(fn, &inst);
  return res;
}

MtlcValue mtlc_address_of(MtlcFn *fn, MtlcValue storage,
                         const MtlcType *pointer_type) {
  if (!fn || !pointer_type) {
    if (fn) {
      fn->builder->error = 1;
    }
    return MTLC_NO_VALUE;
  }
  const IROperand *s = value_operand(fn, storage);
  if (!s) {
    fn->builder->error = 1;
    return MTLC_NO_VALUE;
  }
  record_type(fn->builder, pointer_type);
  IROperand sc = *s;
  MtlcValue res = fresh_temp(fn);
  const IROperand *dest = value_operand(fn, res);
  if (!dest) {
    return MTLC_NO_VALUE;
  }
  IRInstruction inst = {0};
  inst.op = IR_OP_ADDRESS_OF;
  inst.dest = *dest;
  inst.lhs = sc;
  inst.value_type = (MtlcType *)pointer_type;
  emit(fn, &inst);
  return res;
}

/* Apply the load/store scalar flags the code generators key on: the element's
 * byte size travels in `rhs`, floats set is_float+float_bits, and unsigned
 * integer elements set is_unsigned (so a 32-bit load zero-extends). Mirrors
 * ir_lowering's shape exactly. */
static void apply_mem_flags(IRInstruction *inst, const MtlcType *elem) {
  inst->rhs = ir_operand_int((long long)mtlc_type_size(elem));
  if (mtlc_type_is_float(elem)) {
    inst->is_float = 1;
    inst->float_bits = (int)(elem->size * 8);
  } else if (mtlc_type_is_unsigned(elem)) {
    inst->is_unsigned = 1;
  }
}

MtlcValue mtlc_load(MtlcFn *fn, MtlcValue address, const MtlcType *elem_type) {
  if (!fn || !elem_type) {
    if (fn) {
      fn->builder->error = 1;
    }
    return MTLC_NO_VALUE;
  }
  const IROperand *a = value_operand(fn, address);
  if (!a) {
    fn->builder->error = 1;
    return MTLC_NO_VALUE;
  }
  record_type(fn->builder, elem_type);
  IROperand ac = *a;
  MtlcValue res = fresh_temp(fn);
  const IROperand *dest = value_operand(fn, res);
  if (!dest) {
    return MTLC_NO_VALUE;
  }
  IRInstruction inst = {0};
  inst.op = IR_OP_LOAD;
  inst.dest = *dest;
  inst.lhs = ac;
  apply_mem_flags(&inst, elem_type);
  inst.value_type = (MtlcType *)elem_type;
  if (inst.is_float) {
    fn->values[res].float_bits = inst.float_bits;
  }
  emit(fn, &inst);
  return res;
}

void mtlc_store(MtlcFn *fn, MtlcValue address, MtlcValue value,
               const MtlcType *elem_type) {
  if (!fn || !elem_type) {
    if (fn) {
      fn->builder->error = 1;
    }
    return;
  }
  const IROperand *a = value_operand(fn, address);
  const IROperand *v = value_operand(fn, value);
  if (!a || !v) {
    fn->builder->error = 1;
    return;
  }
  record_type(fn->builder, elem_type);
  IRInstruction inst = {0};
  inst.op = IR_OP_STORE;
  inst.dest = *a;
  inst.lhs = *v;
  apply_mem_flags(&inst, elem_type);
  emit(fn, &inst);
}

/* --------------------------------------------------------------- control flow */

void mtlc_label(MtlcFn *fn, const char *label) {
  if (!fn || !label) {
    return;
  }
  IRInstruction inst = {0};
  inst.op = IR_OP_LABEL;
  inst.text = (char *)label;
  emit(fn, &inst);
}

void mtlc_jump(MtlcFn *fn, const char *label) {
  if (!fn || !label) {
    return;
  }
  IRInstruction inst = {0};
  inst.op = IR_OP_JUMP;
  inst.text = (char *)label;
  emit(fn, &inst);
}

void mtlc_branch_if_zero(MtlcFn *fn, MtlcValue cond, const char *label) {
  if (!fn || !label) {
    return;
  }
  const IROperand *c = value_operand(fn, cond);
  if (!c) {
    fn->builder->error = 1;
    return;
  }
  IRInstruction inst = {0};
  inst.op = IR_OP_BRANCH_ZERO;
  inst.lhs = *c;
  inst.text = (char *)label;
  emit(fn, &inst);
}

void mtlc_return(MtlcFn *fn, MtlcValue value) {
  if (!fn) {
    return;
  }
  IRInstruction inst = {0};
  inst.op = IR_OP_RETURN;
  if (value != MTLC_NO_VALUE) {
    const IROperand *v = value_operand(fn, value);
    if (!v) {
      fn->builder->error = 1;
      return;
    }
    inst.lhs = *v;
  }
  emit(fn, &inst);
}

/* -------------------------------------------------------------------- finish */

/* Register the canonical scalar type names so codegen can resolve every
 * parameter/return/local type by name (the frontend uses only scalars through
 * mtlc_type_scalar; composite types would be registered by their builder). */
static void register_scalar_types(IRProgram *program) {
  static const MtlcTypeKind kinds[] = {
      MTLC_TYPE_INT8,    MTLC_TYPE_INT16,   MTLC_TYPE_INT32,  MTLC_TYPE_INT64,
      MTLC_TYPE_UINT8,   MTLC_TYPE_UINT16,  MTLC_TYPE_UINT32, MTLC_TYPE_UINT64,
      MTLC_TYPE_BOOL,    MTLC_TYPE_FLOAT32, MTLC_TYPE_FLOAT64, MTLC_TYPE_STRING,
      MTLC_TYPE_VOID};
  for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
    const MtlcType *t = mtlc_type_scalar(kinds[i]);
    if (t) {
      ir_program_register_type(program, mtlc_type_kind_name(kinds[i]),
                               (MtlcType *)t);
    }
  }
}

MtlcModule *mtlc_builder_finish(MtlcBuilder *builder) {
  if (!builder) {
    return NULL;
  }
  if (builder->error) {
    mtlc_builder_destroy(builder);
    return NULL;
  }

  register_scalar_types(builder->program);
  /* every distinct type the builder saw, resolvable by its NAME (pointer
   * types like "int64*" included -- codegen resolves DECLARE_LOCAL/parameter
   * type names against this registry) */
  for (size_t i = 0; i < builder->seen_count; i++) {
    ir_program_register_type(builder->program, type_name(builder->seen_types[i]),
                             (MtlcType *)builder->seen_types[i]);
  }

  /* module symbol table: global variables */
  for (size_t i = 0; i < builder->global_count; i++) {
    GlobalDecl *g = &builder->globals[i];
    IRModuleSymbol entry;
    memset(&entry, 0, sizeof(entry));
    entry.name = g->name;
    entry.kind = IR_MODSYM_VARIABLE;
    entry.is_extern = g->is_extern;
    entry.type = (MtlcType *)g->type;
    if (!g->is_extern) {
      entry.has_initializer = 1;
      entry.init_is_float = 0;
      entry.init_bits = g->init_value;
    }
    ir_program_add_symbol(builder->program, &entry);
  }

  /* module symbol table: one entry per declared function */
  for (size_t i = 0; i < builder->decl_count; i++) {
    FnDecl *d = &builder->decls[i];
    IRModuleSymbol entry;
    memset(&entry, 0, sizeof(entry));
    entry.name = d->name;
    entry.kind = IR_MODSYM_FUNCTION;
    entry.is_extern = d->is_extern;
    entry.has_body = d->has_body;
    entry.return_type = (MtlcType *)d->return_type;
    entry.param_count = d->param_count;
    if (d->param_count > 0) {
      MtlcType **params = calloc(d->param_count, sizeof(MtlcType *));
      for (size_t p = 0; p < d->param_count; p++) {
        params[p] = (MtlcType *)d->param_types[p];
      }
      entry.param_types = params; /* add_symbol copies the array */
      ir_program_add_symbol(builder->program, &entry);
      free(params);
    } else {
      ir_program_add_symbol(builder->program, &entry);
    }
    /* main(argc, argv) is signalled by a two-parameter main */
    if (strcmp(d->name, "main") == 0 && !d->is_extern) {
      builder->program->main_wants_argc_argv = (d->param_count == 2) ? 1 : 0;
    }
  }

  IRProgram *program = builder->program;
  builder->program = NULL; /* ownership transfers to the module */

  MtlcModule *module = mtlc_module_adopt_ir(program);
  if (!module) {
    ir_program_destroy(program);
    mtlc_builder_destroy(builder);
    return NULL;
  }
  mtlc_builder_destroy(builder);
  return module;
}
