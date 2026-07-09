/* mtlc_lower_module.c - fill the backend IR's type registry + module symbol
 * table from the frontend, so codegen needs neither the AST nor the frontend
 * type/symbol tables. FRONTEND-side adapter (driver, not libmtlc). */
#include "frontend/mtlc_lower_module.h"
#include "frontend/mtlc_frontend.h" // mtlc_type_from_frontend

#include <stdlib.h>
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
    register_named_type(program, tc, fn->return_type_name);
    for (size_t i = 0; i < fn->instruction_count; i++) {
      register_named_type(program, tc, fn->instructions[i].text);
    }
  }
}

/* ------------------------------------------------------------------ */
/* Module symbol table + global initializer evaluation.                */
/* ------------------------------------------------------------------ */

/* A folded numeric constant, mirroring code_generator's BinaryNumericConstant.
 * A float value is stored as its double; callers reinterpret to bits for a
 * 32/64-bit float global. */
typedef struct {
  int is_float;
  long long int_value;
  double float_value;
} NumConst;

static void num_from_int(NumConst *c, long long v) {
  c->is_float = 0;
  c->int_value = v;
  c->float_value = (double)v;
}
static void num_from_double(NumConst *c, double v) {
  c->is_float = 1;
  c->int_value = (long long)v;
  c->float_value = v;
}

/* IEEE-754 width of a frontend type: 32/64 for float32/float64, else 0. */
static int frontend_type_float_bits(const Type *t) {
  if (!t) {
    return 0;
  }
  if (t->kind == TYPE_FLOAT32 && t->size == 4) {
    return 32;
  }
  if (t->kind == TYPE_FLOAT64 && t->size == 8) {
    return 64;
  }
  return 0;
}

static int num_is_float(const NumConst *v, ASTNode *expression) {
  if (v && v->is_float) {
    return 1;
  }
  return expression && expression->resolved_type &&
         frontend_type_float_bits(expression->resolved_type) != 0;
}

/* Reinterpret a stored module-symbol initializer back into a NumConst for
 * identifier references inside another initializer. */
static int module_symbol_numeric(const IRProgram *program, const char *name,
                                  NumConst *out) {
  const IRModuleSymbol *s = ir_program_lookup_symbol(program, name);
  if (!s) {
    return 0;
  }
  if (s->kind == IR_MODSYM_CONSTANT) {
    num_from_int(out, s->const_value);
    return 1;
  }
  if (s->kind == IR_MODSYM_VARIABLE && s->has_initializer) {
    if (s->init_is_float) {
      double d;
      memcpy(&d, &s->init_bits, sizeof(d));
      num_from_double(out, d);
    } else {
      num_from_int(out, s->init_bits);
    }
    return 1;
  }
  return 0;
}

/* Evaluate a constant global initializer expression to a NumConst. Ported from
 * code_generator_binary_eval_numeric_global_initializer (globals.c), with
 * identifier references resolved against the module symbols added so far. */
static int eval_numeric(const IRProgram *program, ASTNode *expression,
                        NumConst *out) {
  if (!expression || !out) {
    return 0;
  }
  switch (expression->type) {
  case AST_NUMBER_LITERAL: {
    NumberLiteral *literal = (NumberLiteral *)expression->data;
    if (!literal) {
      return 0;
    }
    if (literal->is_float) {
      num_from_double(out, literal->float_value);
    } else {
      num_from_int(out, literal->int_value);
    }
    return 1;
  }
  case AST_IDENTIFIER: {
    Identifier *identifier = (Identifier *)expression->data;
    return identifier && identifier->name &&
           module_symbol_numeric(program, identifier->name, out);
  }
  case AST_UNARY_EXPRESSION: {
    UnaryExpression *unary = (UnaryExpression *)expression->data;
    NumConst operand = {0};
    if (!unary || !unary->operator|| !unary->operand ||
        !eval_numeric(program, unary->operand, &operand)) {
      return 0;
    }
    if (strcmp(unary->operator, "+") == 0) {
      *out = operand;
      return 1;
    }
    if (strcmp(unary->operator, "-") == 0) {
      if (num_is_float(&operand, expression)) {
        num_from_double(out, -(operand.is_float ? operand.float_value
                                                : (double)operand.int_value));
      } else {
        num_from_int(out, -operand.int_value);
      }
      return 1;
    }
    if (strcmp(unary->operator, "!") == 0) {
      int is_zero = operand.is_float ? (operand.float_value == 0.0)
                                     : (operand.int_value == 0);
      num_from_int(out, is_zero);
      return 1;
    }
    if (strcmp(unary->operator, "~") == 0 && !operand.is_float) {
      num_from_int(out, ~operand.int_value);
      return 1;
    }
    return 0;
  }
  case AST_BINARY_EXPRESSION: {
    BinaryExpression *binary = (BinaryExpression *)expression->data;
    NumConst left = {0};
    NumConst right = {0};
    if (!binary || !binary->operator|| !binary->left || !binary->right ||
        !eval_numeric(program, binary->left, &left) ||
        !eval_numeric(program, binary->right, &right)) {
      return 0;
    }
    if (left.is_float || right.is_float || num_is_float(NULL, expression)) {
      double l = left.is_float ? left.float_value : (double)left.int_value;
      double r = right.is_float ? right.float_value : (double)right.int_value;
      const char *o = binary->operator;
      if (strcmp(o, "+") == 0) {
        num_from_double(out, l + r);
      } else if (strcmp(o, "-") == 0) {
        num_from_double(out, l - r);
      } else if (strcmp(o, "*") == 0) {
        num_from_double(out, l * r);
      } else if (strcmp(o, "/") == 0) {
        num_from_double(out, l / r);
      } else if (strcmp(o, "==") == 0) {
        num_from_int(out, l == r);
      } else if (strcmp(o, "!=") == 0) {
        num_from_int(out, l != r);
      } else if (strcmp(o, "<") == 0) {
        num_from_int(out, l < r);
      } else if (strcmp(o, "<=") == 0) {
        num_from_int(out, l <= r);
      } else if (strcmp(o, ">") == 0) {
        num_from_int(out, l > r);
      } else if (strcmp(o, ">=") == 0) {
        num_from_int(out, l >= r);
      } else {
        return 0;
      }
      return 1;
    }
    long long l = left.int_value, r = right.int_value;
    const char *o = binary->operator;
    if (strcmp(o, "+") == 0) {
      num_from_int(out, l + r);
    } else if (strcmp(o, "-") == 0) {
      num_from_int(out, l - r);
    } else if (strcmp(o, "*") == 0) {
      num_from_int(out, l * r);
    } else if (strcmp(o, "/") == 0) {
      if (r == 0) {
        return 0;
      }
      num_from_int(out, l / r);
    } else if (strcmp(o, "%") == 0) {
      if (r == 0) {
        return 0;
      }
      num_from_int(out, l % r);
    } else if (strcmp(o, "==") == 0) {
      num_from_int(out, l == r);
    } else if (strcmp(o, "!=") == 0) {
      num_from_int(out, l != r);
    } else if (strcmp(o, "<") == 0) {
      num_from_int(out, l < r);
    } else if (strcmp(o, "<=") == 0) {
      num_from_int(out, l <= r);
    } else if (strcmp(o, ">") == 0) {
      num_from_int(out, l > r);
    } else if (strcmp(o, ">=") == 0) {
      num_from_int(out, l >= r);
    } else if (strcmp(o, "&&") == 0) {
      num_from_int(out, l != 0 && r != 0);
    } else if (strcmp(o, "||") == 0) {
      num_from_int(out, l != 0 || r != 0);
    } else if (strcmp(o, "&") == 0) {
      num_from_int(out, l & r);
    } else if (strcmp(o, "|") == 0) {
      num_from_int(out, l | r);
    } else if (strcmp(o, "^") == 0) {
      num_from_int(out, l ^ r);
    } else {
      return 0;
    }
    return 1;
  }
  case AST_CAST_EXPRESSION: {
    CastExpression *cast = (CastExpression *)expression->data;
    NumConst operand = {0};
    if (!cast || !cast->operand ||
        !eval_numeric(program, cast->operand, &operand)) {
      return 0;
    }
    int target_float_bits =
        expression->resolved_type
            ? frontend_type_float_bits(expression->resolved_type)
            : 0;
    if (target_float_bits != 0) {
      num_from_double(out, operand.is_float ? operand.float_value
                                            : (double)operand.int_value);
    } else {
      num_from_int(out, operand.is_float ? (long long)operand.float_value
                                         : operand.int_value);
    }
    return 1;
  }
  default:
    return 0;
  }
}

/* Build a borrowed-MtlcType parameter array for a function symbol. Returns a
 * malloc'd array (the caller frees it after ir_program_add_symbol copies it) and
 * sets *count; NULL when the function has no parameters. */
static MtlcType **build_param_types(const Symbol *s, size_t *count) {
  *count = 0;
  if (!s || s->kind != SYMBOL_FUNCTION ||
      s->data.function.parameter_count == 0 ||
      !s->data.function.parameter_types) {
    return NULL;
  }
  size_t n = s->data.function.parameter_count;
  MtlcType **arr = (MtlcType **)malloc(n * sizeof(MtlcType *));
  if (!arr) {
    return NULL;
  }
  for (size_t i = 0; i < n; i++) {
    arr[i] = mtlc_type_from_frontend(s->data.function.parameter_types[i]);
  }
  *count = n;
  return arr;
}

static int program_has_function_body(const IRProgram *program,
                                     const char *name) {
  for (size_t i = 0; i < program->function_count; i++) {
    if (program->functions[i] && program->functions[i]->name &&
        strcmp(program->functions[i]->name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

static void populate_module_symbols(IRProgram *program, ASTNode *ast_program,
                                    TypeChecker *tc, SymbolTable *st) {
  Program *pdata = (Program *)ast_program->data;
  if (!pdata) {
    return;
  }
  for (size_t i = 0; i < pdata->declaration_count; i++) {
    ASTNode *decl = pdata->declarations[i];
    if (!decl) {
      continue;
    }
    if (decl->type == AST_FUNCTION_DECLARATION) {
      FunctionDeclaration *fd = (FunctionDeclaration *)decl->data;
      if (!fd || !fd->name) {
        continue;
      }
      Symbol *s = symbol_table_lookup(st, fd->name);
      IRModuleSymbol entry = {0};
      entry.name = fd->name;
      entry.kind = IR_MODSYM_FUNCTION;
      entry.is_extern = fd->is_extern;
      entry.has_body =
          fd->body != NULL && program_has_function_body(program, fd->name);
      entry.link_name = s ? s->link_name : NULL;
      entry.type = s ? mtlc_type_from_frontend(s->type) : NULL;
      if (s && s->kind == SYMBOL_FUNCTION) {
        entry.return_type =
            mtlc_type_from_frontend(s->data.function.return_type);
      }
      size_t pc = 0;
      MtlcType **params = build_param_types(s, &pc);
      entry.param_types = params;
      entry.param_count = pc;
      ir_program_add_symbol(program, &entry); /* copies param_types */
      free(params);
    } else if (decl->type == AST_VAR_DECLARATION) {
      VarDeclaration *vd = (VarDeclaration *)decl->data;
      if (!vd || !vd->name) {
        continue;
      }
      Symbol *s = symbol_table_lookup(st, vd->name);
      IRModuleSymbol entry = {0};
      entry.name = vd->name;
      entry.is_extern = vd->is_extern;
      entry.link_name = s ? s->link_name : NULL;
      if (s && s->kind == SYMBOL_CONSTANT) {
        entry.kind = IR_MODSYM_CONSTANT;
        entry.const_value = s->data.constant.value;
        entry.type = mtlc_type_from_frontend(s->type);
      } else {
        entry.kind = IR_MODSYM_VARIABLE;
        Type *vtype = s ? s->type : NULL;
        if (!vtype && vd->type_name) {
          vtype = type_checker_get_type_by_name(tc, vd->type_name);
        }
        entry.type = mtlc_type_from_frontend(vtype);
        if (!vd->is_extern && vd->initializer) {
          if (entry.type && entry.type->kind == MTLC_TYPE_STRING) {
            if (vd->initializer->type == AST_STRING_LITERAL) {
              StringLiteral *lit = (StringLiteral *)vd->initializer->data;
              entry.has_initializer = 1;
              entry.init_string = lit && lit->value ? lit->value : "";
            } else {
              entry.has_unfoldable_initializer = 1;
            }
          } else {
            NumConst c = {0};
            if (eval_numeric(program, vd->initializer, &c)) {
              entry.has_initializer = 1;
              entry.init_is_float = c.is_float;
              if (c.is_float) {
                double d = c.float_value;
                memcpy(&entry.init_bits, &d, sizeof(d));
              } else {
                entry.init_bits = c.int_value;
              }
            } else {
              entry.has_unfoldable_initializer = 1;
            }
          }
        }
      }
      ir_program_add_symbol(program, &entry);
    } else if (decl->type == AST_STRUCT_DECLARATION ||
               decl->type == AST_ENUM_DECLARATION) {
      /* Register user-defined named types so codegen can resolve them. */
      Symbol *s = NULL;
      if (decl->type == AST_STRUCT_DECLARATION) {
        StructDeclaration *sd = (StructDeclaration *)decl->data;
        if (sd && sd->name) {
          s = symbol_table_lookup(st, sd->name);
        }
      } else {
        EnumDeclaration *ed = (EnumDeclaration *)decl->data;
        if (ed && ed->name) {
          s = symbol_table_lookup(st, ed->name);
        }
      }
      if (s && s->name && s->type) {
        register_named_type(program, tc, s->name);
      }
    }
  }
}

/* main() takes (argc, argv) when its lowered signature has two parameters. */
static void populate_main_flag(IRProgram *program) {
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *fn = program->functions[i];
    if (fn && fn->name && strcmp(fn->name, "main") == 0) {
      program->main_wants_argc_argv = (fn->parameter_count == 2) ? 1 : 0;
      return;
    }
  }
}

void mtlc_lower_populate_module(IRProgram *program, ASTNode *ast_program,
                                TypeChecker *tc, SymbolTable *st) {
  if (!program || !tc) {
    return;
  }
  populate_type_registry(program, tc);
  if (ast_program && st) {
    populate_module_symbols(program, ast_program, tc, st);
  }
  populate_main_flag(program);
}
