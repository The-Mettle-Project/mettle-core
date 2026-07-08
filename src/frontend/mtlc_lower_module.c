/* mtlc_lower_module.c - fill the backend IR's type registry + module symbol
 * table from the frontend, so codegen needs neither the AST nor the frontend
 * type/symbol tables. FRONTEND-side adapter (driver, not libmtlc). */
#include "frontend/mtlc_lower_module.h"
#include "frontend/mtlc_frontend.h" // mtlc_type_from_frontend

#include <string.h>

/* Register `name` -> its MtlcType in the program type registry, resolving it via
 * the frontend type checker (which parses primitives, structs/enums, and the
 * composite fn(...)->R / T[] / T* forms). No-op for names that are not types. */
static void register_named_type(IRProgram *program, TypeChecker *tc,
                                const char *name) {
  if (!name || !name[0] || ir_program_lookup_type(program, name)) {
    return;
  }
  Type *t = type_checker_get_type_by_name(tc, name);
  if (t) {
    ir_program_register_type(program, name, mtlc_type_from_frontend(t));
  }
}

/* Register every type name the code generators may resolve: the primitives (for
 * get_resolved_type's defaults) plus every type name that appears in the IR as a
 * function parameter type or an instruction's text (IR_OP_CAST /
 * IR_OP_DECLARE_LOCAL carry a type name there). */
static void populate_type_registry(IRProgram *program, TypeChecker *tc) {
  static const char *const builtins[] = {
      "bool",   "int8",    "int16",   "int32",   "int64",
      "uint8",  "uint16",  "uint32",  "uint64",  "float32",
      "float64", "string", "cstring", "void"};
  for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
    register_named_type(program, tc, builtins[i]);
  }

  for (size_t f = 0; f < program->function_count; f++) {
    IRFunction *fn = program->functions[f];
    if (!fn) {
      continue;
    }
    for (size_t p = 0; p < fn->parameter_count; p++) {
      if (fn->parameter_types) {
        register_named_type(program, tc, fn->parameter_types[p]);
      }
    }
    for (size_t i = 0; i < fn->instruction_count; i++) {
      register_named_type(program, tc, fn->instructions[i].text);
    }
  }
}

void mtlc_lower_populate_module(IRProgram *program, ASTNode *ast_program,
                                TypeChecker *tc, SymbolTable *st) {
  if (!program || !tc) {
    return;
  }
  populate_type_registry(program, tc);

  /* Module symbol table + global initializers: Milestone B. */
  (void)ast_program;
  (void)st;
}
