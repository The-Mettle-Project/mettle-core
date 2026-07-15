/* mtlc/build.h - a public IR builder for frontends.
 *
 * This is how a frontend that is NOT the reference Mettle frontend constructs
 * libmtlc IR without touching the backend's internal headers. You describe
 * functions and an imperative instruction stream through opaque handles; the
 * builder produces an MtlcModule (see mtlc/module.h) ready for the pipeline
 * (mtlc_optimize -> mtlc_emit_object -> mtlc_link_executable in mtlc/pipeline.h).
 *
 * The model mirrors the backend IR: values are SSA-like temporaries or named
 * locals/parameters, referenced by opaque MtlcValue handles; control flow is
 * explicit labels and branches (the frontend lowers its own if/while/for). A
 * function whose body you emit is a definition; declare an `extern` function to
 * reference a symbol linked from elsewhere (e.g. a C runtime routine).
 *
 *   MtlcBuilder *b = mtlc_builder_create();
 *   const MtlcType *i64 = mtlc_type_scalar(MTLC_TYPE_INT64);
 *   MtlcFn *f = mtlc_builder_function(b, "main", i64, NULL, NULL, 0, 0);
 *   MtlcValue r = mtlc_const_int(f, i64, 42);
 *   mtlc_return(f, r);
 *   MtlcModule *m = mtlc_builder_finish(b);   // b is consumed
 */
#ifndef MTLC_BUILD_H
#define MTLC_BUILD_H

#include "module.h"
#include "type.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MtlcBuilder MtlcBuilder;
typedef struct MtlcFn MtlcFn;

/* An opaque handle to an IR value within one function builder. Values do not
 * cross function boundaries. MTLC_NO_VALUE is the "no value" sentinel (a void
 * return, an unset operand). */
typedef int MtlcValue;
#define MTLC_NO_VALUE (-1)

/* Create/destroy a builder. Destroying a builder you have not finished frees
 * everything it holds; mtlc_builder_finish consumes the builder instead. */
MtlcBuilder *mtlc_builder_create(void);
void mtlc_builder_destroy(MtlcBuilder *builder);

/* Declare a function. `return_type` may be mtlc_type_scalar(MTLC_TYPE_VOID).
 * `param_names`/`param_types` each hold `param_count` entries (pass NULL/NULL/0
 * for no parameters). `is_extern` != 0 declares a body-less external symbol
 * (return value is NULL; do not emit a body into it). Otherwise returns a
 * function builder to emit the body into. The first non-extern function named
 * "main" is treated as the program entry point by mtlc_link_executable. */
MtlcFn *mtlc_builder_function(MtlcBuilder *builder, const char *name,
                             const MtlcType *return_type,
                             const char *const *param_names,
                             const MtlcType *const *param_types,
                             size_t param_count, int is_extern);

/* Declare a module-level global variable of a scalar `type`, optionally with a
 * constant integer initializer (pass 0 for zero-initialized). `is_extern` != 0
 * declares a global defined elsewhere. Reference it inside a function with
 * mtlc_global_ref. */
void mtlc_builder_global(MtlcBuilder *builder, const char *name,
                        const MtlcType *type, long long init_value,
                        int is_extern);

/* ---- values ---- */

/* Reference parameter `index` (0-based) of this function as a value. */
MtlcValue mtlc_fn_param(MtlcFn *fn, size_t index);

/* An integer literal of `type`. */
MtlcValue mtlc_const_int(MtlcFn *fn, const MtlcType *type, long long value);

/* A floating literal; `type` selects float32 or float64. */
MtlcValue mtlc_const_float(MtlcFn *fn, const MtlcType *type, double value);

/* Declare a mutable local variable and return a value referring to it: reads
 * use the returned handle, mtlc_assign writes through it. */
MtlcValue mtlc_local(MtlcFn *fn, const char *name, const MtlcType *type);

/* Reference a module-level global declared with mtlc_builder_global. Reads use
 * the handle; mtlc_assign writes through it. */
MtlcValue mtlc_global_ref(MtlcFn *fn, const char *name);

/* ---- instructions ---- */

/* Store `value` into the storage `dest` refers to (a local or a parameter). */
void mtlc_assign(MtlcFn *fn, MtlcValue dest, MtlcValue value);

/* A binary op. `op` is one of: "+", "-", "*", "/", "%", "==", "!=", "<", "<=",
 * ">", ">=", "&&", "||", "&", "|", "^", "<<", ">>". `result_type` is the type
 * of the result (baked onto the instruction so codegen never re-derives it). */
MtlcValue mtlc_binary(MtlcFn *fn, const char *op, MtlcValue lhs, MtlcValue rhs,
                     const MtlcType *result_type);

/* A unary op: "-" (negate), "!" (logical not), "~" (bitwise not). */
MtlcValue mtlc_unary(MtlcFn *fn, const char *op, MtlcValue operand,
                    const MtlcType *result_type);

/* Call `callee` by name with `arg_count` arguments; returns the result value,
 * or MTLC_NO_VALUE when `return_type` is void. */
MtlcValue mtlc_call(MtlcFn *fn, const char *callee, const MtlcValue *args,
                   size_t arg_count, const MtlcType *return_type);

/* Real address of a function symbol (defined or extern-declared): usable as
 * a callback for OS/CRT APIs and with mtlc_call_indirect. */
MtlcValue mtlc_function_address(MtlcFn *fn, const char *name);

/* Call through a function-pointer value with `arg_count` arguments. Without a
 * typed function-pointer symbol, arguments classify as integer/pointer. */
MtlcValue mtlc_call_indirect(MtlcFn *fn, MtlcValue callee,
                             const MtlcValue *args, size_t arg_count,
                             const MtlcType *return_type);

/* Convert `value` to `type` (integer width/sign changes, int<->float,
 * int<->pointer). */
MtlcValue mtlc_cast(MtlcFn *fn, MtlcValue value, const MtlcType *type);

/* The address of local/parameter `storage`, as a pointer value. */
MtlcValue mtlc_address_of(MtlcFn *fn, MtlcValue storage,
                         const MtlcType *pointer_type);

/* Load a scalar of `elem_type` from the address held in `address` (a pointer
 * value, e.g. a parameter, an mtlc_address_of result, or malloc'd memory). */
MtlcValue mtlc_load(MtlcFn *fn, MtlcValue address, const MtlcType *elem_type);

/* Store scalar `value` of `elem_type` to the address held in `address`. */
void mtlc_store(MtlcFn *fn, MtlcValue address, MtlcValue value,
               const MtlcType *elem_type);

/* ---- control flow ---- */

/* Define a label at the current position. */
void mtlc_label(MtlcFn *fn, const char *label);
/* Unconditional branch to `label`. */
void mtlc_jump(MtlcFn *fn, const char *label);
/* Branch to `label` when `cond` is zero; fall through otherwise. */
void mtlc_branch_if_zero(MtlcFn *fn, MtlcValue cond, const char *label);
/* Return `value` (or MTLC_NO_VALUE for a void return). */
void mtlc_return(MtlcFn *fn, MtlcValue value);

/* Finish building: populate the module's type registry and symbol table and
 * return the module. The builder is consumed and must not be used afterwards
 * (do not also call mtlc_builder_destroy). Returns NULL on error. */
MtlcModule *mtlc_builder_finish(MtlcBuilder *builder);

#ifdef __cplusplus
}
#endif

#endif /* MTLC_BUILD_H */
