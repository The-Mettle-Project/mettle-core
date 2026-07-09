/* mtlc/context.h - a backend session handle.
 *
 * MtlcContext replaces the process-global compiler state the backend historically
 * relied on (explain sinks, ML-opt model paths, optimization knobs). Holding this
 * state on an explicit handle is what lets any frontend drive libmtlc, and is a
 * prerequisite for reentrancy: the backend keeps its remaining mutable
 * per-compile state thread-local, so separate threads can each drive a context.
 *
 * The handle owns the high-level knobs below and is threaded through the mtlc_*
 * pipeline entry points (optimize, ML-opt, codegen, link) in mtlc/pipeline.h.
 */
#ifndef MTLC_CONTEXT_H
#define MTLC_CONTEXT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MtlcContext MtlcContext;

/* Create/destroy a backend session. Returns NULL on allocation failure. */
MtlcContext *mtlc_context_create(void);
void mtlc_context_destroy(MtlcContext *ctx);

/* Optimization level: 0 = none, >=1 = the classical pipeline. */
void mtlc_context_set_opt_level(MtlcContext *ctx, int level);
int mtlc_context_opt_level(const MtlcContext *ctx);

/* Enable the GNN-driven, translation-validation-gated ML optimizer. */
void mtlc_context_set_ml_opt(MtlcContext *ctx, int enabled);
int mtlc_context_ml_opt(const MtlcContext *ctx);

/* Whole-program mode: set when a single executable link with a known entry
 * point makes every call site visible (gates whole-program transforms). */
void mtlc_context_set_whole_program(MtlcContext *ctx, int enabled);
int mtlc_context_whole_program(const MtlcContext *ctx);

/* --explain: report each optimization decision. focus_file, when non-NULL,
 * limits remarks to that source file. The string is borrowed, not copied. */
void mtlc_context_set_explain(MtlcContext *ctx, int enabled,
                              const char *focus_file);
int mtlc_context_explain(const MtlcContext *ctx);
const char *mtlc_context_explain_focus_file(const MtlcContext *ctx);

#ifdef __cplusplus
}
#endif

#endif /* MTLC_CONTEXT_H */
