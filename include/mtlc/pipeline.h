/* mtlc/pipeline.h - the backend pipeline: optimize -> codegen -> link.
 *
 * These are the frontend-agnostic entry points into libmtlc. Phase 1 exposes the
 * fully decoupled optimization stages (the classical optimizer and the GNN-driven
 * ML optimizer). The codegen and link stages are still orchestrated by the
 * reference driver (src/main.c) while codegen is decoupled from the frontend
 * type system; they become first-class mtlc_* entry points in Phase 2.
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

#ifdef __cplusplus
}
#endif

#endif /* MTLC_PIPELINE_H */
