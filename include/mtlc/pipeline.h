/* mtlc/pipeline.h - the backend pipeline: optimize -> codegen -> link.
 *
 * These are the frontend-agnostic entry points into libmtlc: the classical
 * optimizer, the GNN-driven ML optimizer, native object emission, and linking a
 * native executable. A frontend builds a module (mtlc/build.h), then drives it
 * through these stages -- see examples/calc for a complete non-Mettle frontend.
 */
#ifndef MTLC_PIPELINE_H
#define MTLC_PIPELINE_H

#include "context.h"
#include "module.h"
#include "target.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Run the classical optimizer on the module, using the knobs on `ctx`
 * (opt level, whole-program, explain). A NULL ctx uses conservative defaults
 * (optimize on, no explain, not whole-program). Returns 1 on success, 0 on
 * error (e.g. a violated `@simd!` contract, already reported to stderr). */
int mtlc_optimize(MtlcContext *ctx, MtlcModule *module);

/* Statistics from the ML optimizer (mirrors the backend's MLOptStats). */
typedef struct {
  int proposals;
  int validated;
  int proven;
  int rejected;
  int skipped;
} MtlcMlOptStats;

/* Run the GNN-driven, translation-validation-gated ML optimizer, then hoist
 * constants. No-op unless ml-opt is enabled on `ctx`. `stats` may be NULL.
 * Returns 1 on success, 0 on error. */
int mtlc_apply_ml_opt(MtlcContext *ctx, MtlcModule *module,
                      MtlcMlOptStats *stats);

/* Generate native code for the module and write a relocatable object file (the
 * host object format: COFF on Windows, ELF elsewhere) to `path`. Run
 * mtlc_optimize first for optimized code. Returns 1 on success, 0 on error
 * (message printed to stderr). */
int mtlc_emit_object(MtlcContext *ctx, MtlcModule *module, const char *path);

/* Compile the module all the way to a native executable at `output_path`: emit
 * a temporary object, synthesize the C-runtime startup that calls the program's
 * `main`, and link. On Windows this uses libmtlc's own internal PE linker
 * (imports resolved by DLL name -- no external toolchain); on ELF hosts it
 * invokes the system C compiler to link the object. Returns 1 on success, 0 on
 * error. */
int mtlc_build_executable(MtlcContext *ctx, MtlcModule *module,
                          const char *output_path);

#ifdef __cplusplus
}
#endif

#endif /* MTLC_PIPELINE_H */
