/* mtlc_api.c - implementation of the public libmtlc entry points (include/mtlc).
 *
 * Part of libmtlc. This translation unit is the seam between the stable public
 * API and the backend's internal IR/optimizer entry points. It is frontend-free:
 * it includes only backend headers (ir.h, ir_optimize.h, ml_opt.h) and the public
 * mtlc/ headers. */
#include "mtlc/mtlc.h"

#include "codegen/binary/startup.h"
#include "codegen/binary_emitter.h"
#include "codegen/code_generator.h"
#include "ir/ir.h"
#include "ir/ir_optimize.h"
#include "ir/ml_opt.h"
#include "linker/pe_emitter.h"
#include "linker/symbol_resolve.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* ------------------------------------------------------------- codegen + link */

int mtlc_emit_object(MtlcContext *ctx, MtlcModule *module, const char *path) {
  (void)ctx;
  if (!module || !module->ir || !path) {
    return 0;
  }
  CodeGenerator *gen = code_generator_create();
  if (!gen) {
    return 0;
  }
  code_generator_set_ir_program(gen, module->ir);
  if (!code_generator_generate_program(gen)) {
    fprintf(stderr, "mtlc: code generation failed: %s\n",
            gen->error_message ? gen->error_message : "unknown error");
    code_generator_destroy(gen);
    return 0;
  }
  BinaryEmitter *be = code_generator_get_binary_emitter(gen);
  int ok = binary_emitter_write_object_file(be, path);
  if (!ok) {
    fprintf(stderr, "mtlc: could not write object '%s': %s\n", path,
            binary_emitter_get_error(be) ? binary_emitter_get_error(be)
                                          : "unknown error");
  }
  code_generator_destroy(gen);
  return ok;
}

/* Link `object_paths` + the C-runtime startup object into a PE executable using
 * libmtlc's internal linker. Imports are resolved by DLL name, so no Windows SDK
 * import libraries are needed. */
static int link_pe_internal(const char **object_paths, size_t object_count,
                            const char *output_path) {
  static const char *const import_dlls[] = {"kernel32.dll", "ucrtbase.dll",
                                             "msvcrt.dll"};
  LinkResolutionOptions resolution_options = {"mainCRTStartup", 16u, 1};
  LinkResolution *resolution = NULL;
  PeEmissionOptions emission = {0};
  char *error_message = NULL;
  int ok = 0;

  if (!link_resolution_build(object_paths, object_count, &resolution_options,
                             &resolution, &error_message)) {
    fprintf(stderr, "mtlc: internal linker resolution failed: %s\n",
            error_message ? error_message : "unknown error");
    free(error_message);
    return 0;
  }
  emission.import_dll_names = (const char **)import_dlls;
  emission.import_dll_count = sizeof(import_dlls) / sizeof(import_dlls[0]);
  if (!pe_emit_executable(resolution, output_path, &emission, &error_message)) {
    fprintf(stderr, "mtlc: internal linker PE emission failed: %s\n",
            error_message ? error_message : "unknown error");
    free(error_message);
    link_resolution_destroy(resolution);
    return 0;
  }
  ok = 1;
  link_resolution_destroy(resolution);
  return ok;
}

int mtlc_build_executable(MtlcContext *ctx, MtlcModule *module,
                          const char *output_path) {
  (void)ctx;
  if (!module || !module->ir || !output_path) {
    return 0;
  }
  int wants_argc_argv = module->ir->main_wants_argc_argv;

  /* temp paths derived from the output path */
  size_t base_len = strlen(output_path);
  char *obj_path = (char *)malloc(base_len + 16);
  char *startup_path = (char *)malloc(base_len + 24);
  if (!obj_path || !startup_path) {
    free(obj_path);
    free(startup_path);
    return 0;
  }
#ifdef _WIN32
  snprintf(obj_path, base_len + 16, "%s.mtlcobj.obj", output_path);
  snprintf(startup_path, base_len + 24, "%s.mtlcstart.obj", output_path);
#else
  snprintf(obj_path, base_len + 16, "%s.mtlcobj.o", output_path);
  snprintf(startup_path, base_len + 24, "%s.mtlcstart.o", output_path);
#endif

  int result = 0;
  if (!mtlc_emit_object(ctx, module, obj_path)) {
    goto done;
  }

#ifdef _WIN32
  /* binary_write_program_startup_object returns 0 on success (non-zero fails). */
  if (binary_write_program_startup_object(startup_path, 0, 0,
                                          wants_argc_argv) != 0) {
    fprintf(stderr, "mtlc: could not write startup object\n");
    goto done;
  }
  {
    const char *objects[2] = {startup_path, obj_path};
    result = link_pe_internal(objects, 2, output_path);
  }
#else
  /* ELF hosts: the system C toolchain provides crt startup + links the object. */
  {
    size_t cmd_len = strlen(obj_path) + strlen(output_path) + 64;
    char *cmd = (char *)malloc(cmd_len);
    if (cmd) {
      snprintf(cmd, cmd_len, "cc -no-pie \"%s\" -o \"%s\"", obj_path,
               output_path);
      result = (system(cmd) == 0);
      free(cmd);
    }
    (void)wants_argc_argv;
  }
#endif

done:
  remove(obj_path);
  remove(startup_path);
  free(obj_path);
  free(startup_path);
  return result;
}
