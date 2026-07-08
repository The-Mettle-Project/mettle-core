/* mtlc_api.c - implementation of the public libmtlc entry points (include/mtlc).
 *
 * Part of libmtlc. This translation unit is the seam between the stable public
 * API and the backend's internal IR/optimizer entry points. It is frontend-free:
 * it includes only backend headers (ir.h, ir_optimize.h, ml_opt.h) and the public
 * mtlc/ headers. */
#include "mtlc/mtlc.h"

#include "ir/ir.h"
#include "ir/ir_optimize.h"
#include "ir/ml_opt.h"

#include <stdlib.h>

/* ------------------------------------------------------------------ version */

const char *mtlc_version(void) { return "libmtlc 0.1.0"; }

/* ------------------------------------------------------------------- target */

MtlcObjectFormat mtlc_host_object_format(void) {
#ifdef _WIN32
  return MTLC_OBJ_COFF;
#else
  return MTLC_OBJ_ELF;
#endif
}

MtlcLinkTarget mtlc_host_link_target(void) {
#ifdef _WIN32
  return MTLC_LINK_PE;
#else
  return MTLC_LINK_ELF;
#endif
}

const char *mtlc_arch_name(MtlcArch arch) {
  switch (arch) {
  case MTLC_ARCH_X86_64:
    return "x86-64";
  case MTLC_ARCH_ARM64:
    return "arm64";
  case MTLC_ARCH_PTX:
    return "ptx";
  case MTLC_ARCH_SPIRV:
    return "spirv";
  }
  return "?";
}

/* ------------------------------------------------------------------ context */

struct MtlcContext {
  int opt_level;
  int ml_opt;
  int whole_program;
  int explain;
  const char *explain_focus_file;
};

MtlcContext *mtlc_context_create(void) {
  MtlcContext *ctx = (MtlcContext *)calloc(1, sizeof(MtlcContext));
  return ctx; /* zero-initialized: no opt, no ml, not whole-program */
}

void mtlc_context_destroy(MtlcContext *ctx) { free(ctx); }

void mtlc_context_set_opt_level(MtlcContext *ctx, int level) {
  if (ctx) {
    ctx->opt_level = level;
  }
}
int mtlc_context_opt_level(const MtlcContext *ctx) {
  return ctx ? ctx->opt_level : 0;
}

void mtlc_context_set_ml_opt(MtlcContext *ctx, int enabled) {
  if (ctx) {
    ctx->ml_opt = enabled ? 1 : 0;
  }
}
int mtlc_context_ml_opt(const MtlcContext *ctx) {
  return ctx ? ctx->ml_opt : 0;
}

void mtlc_context_set_whole_program(MtlcContext *ctx, int enabled) {
  if (ctx) {
    ctx->whole_program = enabled ? 1 : 0;
  }
}
int mtlc_context_whole_program(const MtlcContext *ctx) {
  return ctx ? ctx->whole_program : 0;
}

void mtlc_context_set_explain(MtlcContext *ctx, int enabled,
                              const char *focus_file) {
  if (ctx) {
    ctx->explain = enabled ? 1 : 0;
    ctx->explain_focus_file = focus_file;
  }
}
int mtlc_context_explain(const MtlcContext *ctx) {
  return ctx ? ctx->explain : 0;
}
const char *mtlc_context_explain_focus_file(const MtlcContext *ctx) {
  return ctx ? ctx->explain_focus_file : NULL;
}

/* ------------------------------------------------------------------- module */

struct MtlcModule {
  IRProgram *ir; /* owned */
};

MtlcModule *mtlc_module_adopt_ir(void *ir_program) {
  MtlcModule *m = (MtlcModule *)calloc(1, sizeof(MtlcModule));
  if (!m) {
    return NULL;
  }
  m->ir = (IRProgram *)ir_program;
  return m;
}

void *mtlc_module_ir(MtlcModule *module) { return module ? module->ir : NULL; }

size_t mtlc_module_function_count(const MtlcModule *module) {
  return (module && module->ir) ? module->ir->function_count : 0;
}

void mtlc_module_destroy(MtlcModule *module) {
  if (!module) {
    return;
  }
  if (module->ir) {
    ir_program_destroy(module->ir);
  }
  free(module);
}

/* ----------------------------------------------------------------- pipeline */

int mtlc_optimize(MtlcContext *ctx, MtlcModule *module) {
  if (!module || !module->ir) {
    return 0;
  }
  IROptimizeOptions options;
  /* Zero every field so future additions default off. */
  IROptimizeOptions zero = {0};
  options = zero;
  options.whole_program = ctx ? ctx->whole_program : 0;
  options.explain = ctx ? ctx->explain : 0;
  options.explain_focus_file = ctx ? ctx->explain_focus_file : NULL;
  return ir_optimize_program(module->ir, &options);
}

int mtlc_apply_ml_opt(MtlcContext *ctx, MtlcModule *module,
                      MtlcMlOptStats *stats) {
  (void)ctx; /* the caller decides when to run the ML pass */
  if (!module || !module->ir) {
    return 0;
  }
  MLOptStats s = {0};
  int ok = ir_apply_ml_opt(module->ir, &s);
  if (ok) {
    ir_hoist_constants(module->ir);
  }
  if (stats) {
    stats->proposals = s.proposals;
    stats->validated = s.validated;
    stats->proven = s.proven;
    stats->rejected = s.rejected;
    stats->skipped = s.skipped;
  }
  return ok;
}
