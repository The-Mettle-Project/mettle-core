/* public_api_test.c - proves the FULL public libmtlc surface end to end, using
 * only <mtlc/...> headers + the static library (no internal headers, no Mettle
 * frontend). Driven by tests/run_tests.ps1 (the `public_api` gate).
 *
 *   public_api_test <outdir>
 *
 * Builds three modules through mtlc/build.h and emits through mtlc/pipeline.h:
 *   1. native:  globals (read+write), extern malloc + putchar (libc via the
 *      internal PE linker), pointer load/store, address-of local, float
 *      arithmetic + cast, optimizer on -> <outdir>/pubapi_native.exe
 *      (the harness runs it: expects exit code 42 and stdout "OK")
 *   2. gpu:     a float32* kernel -> PTX text (checked: ".entry") and a SPIR-V
 *      binary (checked: magic 0x07230203)
 *   3. arm64:   scalar add/main -> static AArch64 ELF (checked: \x7fELF magic)
 *
 * Exit code 0 = everything emitted and structurally verified. */
#include <mtlc/build.h>
#include <mtlc/mtlc.h>
#include <mtlc/pipeline.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(const char *what) {
  fprintf(stderr, "public_api_test: FAIL: %s\n", what);
  return 1;
}

static char *path_join(const char *dir, const char *name) {
  size_t n = strlen(dir) + strlen(name) + 2;
  char *p = malloc(n);
  snprintf(p, n, "%s/%s", dir, name);
  return p;
}

/* ---- module 1: the native program (exit 42, prints "OK") ---- */
static MtlcModule *build_native_module(void) {
  const MtlcType *i64 = mtlc_type_scalar(MTLC_TYPE_INT64);
  const MtlcType *i32 = mtlc_type_scalar(MTLC_TYPE_INT32);
  const MtlcType *f64 = mtlc_type_scalar(MTLC_TYPE_FLOAT64);
  const MtlcType *pi64 = mtlc_type_pointer(i64);
  MtlcBuilder *b = mtlc_builder_create();

  /* module global, read AND written below */
  mtlc_builder_global(b, "counter", i64, 40, /*extern=*/0);

  /* libc externs, resolved by the internal PE linker from msvcrt/ucrtbase */
  {
    const char *pn[] = {"size"};
    const MtlcType *pt[] = {i64};
    mtlc_builder_function(b, "malloc", pi64, pn, pt, 1, /*extern=*/1);
  }
  {
    const char *pn[] = {"c"};
    const MtlcType *pt[] = {i32};
    mtlc_builder_function(b, "putchar", i32, pn, pt, 1, /*extern=*/1);
  }

  MtlcFn *m = mtlc_builder_function(b, "main", i64, NULL, NULL, 0, 0);

  /* heap memory through an extern + pointer store/load:
   *   p = malloc(8); *p = 41; x = *p + 1;              -> x = 42 */
  MtlcValue eight = mtlc_const_int(m, i64, 8);
  MtlcValue p = mtlc_call(m, "malloc", &eight, 1, pi64);
  MtlcValue c41 = mtlc_const_int(m, i64, 41);
  mtlc_store(m, p, c41, i64);
  MtlcValue loaded = mtlc_load(m, p, i64);
  MtlcValue x = mtlc_local(m, "x", i64);
  mtlc_assign(m, x, mtlc_binary(m, "+", loaded, mtlc_const_int(m, i64, 1), i64));

  /* global read: x += counter - 40                      (+0) */
  MtlcValue g = mtlc_global_ref(m, "counter");
  MtlcValue gdiff = mtlc_binary(m, "-", g, mtlc_const_int(m, i64, 40), i64);
  mtlc_assign(m, x, mtlc_binary(m, "+", x, gdiff, i64));

  /* float arithmetic + cast: (2.5 * 4.0) -> 10.0 -> 10; x += 10 - 10  (+0) */
  MtlcValue fprod = mtlc_binary(m, "*", mtlc_const_float(m, f64, 2.5),
                                mtlc_const_float(m, f64, 4.0), f64);
  MtlcValue fi = mtlc_cast(m, fprod, i64);
  MtlcValue fdiff = mtlc_binary(m, "-", fi, mtlc_const_int(m, i64, 10), i64);
  mtlc_assign(m, x, mtlc_binary(m, "+", x, fdiff, i64));

  /* address-of a local + store/load through the pointer: l=7; *&l=8; (+0) */
  MtlcValue l = mtlc_local(m, "l", i64);
  mtlc_assign(m, l, mtlc_const_int(m, i64, 7));
  MtlcValue pl = mtlc_address_of(m, l, pi64);
  mtlc_store(m, pl, mtlc_const_int(m, i64, 8), i64);
  MtlcValue lv = mtlc_load(m, pl, i64);
  MtlcValue ldiff = mtlc_binary(m, "-", lv, mtlc_const_int(m, i64, 8), i64);
  mtlc_assign(m, x, mtlc_binary(m, "+", x, ldiff, i64));

  /* global WRITE + read-back: counter = x; x = counter */
  mtlc_assign(m, g, x);
  mtlc_assign(m, x, mtlc_global_ref(m, "counter"));

  /* print "OK\n" through the putchar extern */
  MtlcValue cO = mtlc_const_int(m, i32, 'O');
  mtlc_call(m, "putchar", &cO, 1, i32);
  MtlcValue cK = mtlc_const_int(m, i32, 'K');
  mtlc_call(m, "putchar", &cK, 1, i32);
  MtlcValue nl = mtlc_const_int(m, i32, '\n');
  mtlc_call(m, "putchar", &nl, 1, i32);

  mtlc_return(m, x); /* 42 */
  return mtlc_builder_finish(b);
}

/* ---- module 2: a GPU kernel (for PTX + SPIR-V) ---- */
static MtlcModule *build_gpu_module(void) {
  const MtlcType *f32 = mtlc_type_scalar(MTLC_TYPE_FLOAT32);
  const MtlcType *pf32 = mtlc_type_pointer(f32);
  const MtlcType *voidt = mtlc_type_scalar(MTLC_TYPE_VOID);
  MtlcBuilder *b = mtlc_builder_create();
  const char *pn[] = {"a"};
  const MtlcType *pt[] = {pf32};
  MtlcFn *k = mtlc_builder_function(b, "scale2", voidt, pn, pt, 1, 0);
  MtlcValue a = mtlc_fn_param(k, 0);
  MtlcValue v = mtlc_load(k, a, f32);
  MtlcValue v2 = mtlc_binary(k, "*", v, mtlc_const_float(k, f32, 2.0), f32);
  mtlc_store(k, a, v2, f32);
  mtlc_return(k, MTLC_NO_VALUE);
  return mtlc_builder_finish(b);
}

/* ---- module 3: scalar arithmetic (for the AArch64 ELF) ---- */
static MtlcModule *build_arm64_module(void) {
  const MtlcType *i64 = mtlc_type_scalar(MTLC_TYPE_INT64);
  MtlcBuilder *b = mtlc_builder_create();
  const char *pn[] = {"a", "b"};
  const MtlcType *pt[] = {i64, i64};
  MtlcFn *add = mtlc_builder_function(b, "add", i64, pn, pt, 2, 0);
  mtlc_return(add, mtlc_binary(add, "+", mtlc_fn_param(add, 0),
                               mtlc_fn_param(add, 1), i64));
  MtlcFn *m = mtlc_builder_function(b, "main", i64, NULL, NULL, 0, 0);
  MtlcValue args[2];
  args[0] = mtlc_const_int(m, i64, 30);
  args[1] = mtlc_const_int(m, i64, 12);
  mtlc_return(m, mtlc_call(m, "add", args, 2, i64));
  return mtlc_builder_finish(b);
}

static int file_starts_with(const char *path, const unsigned char *magic,
                            size_t n) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    return 0;
  }
  unsigned char buf[8] = {0};
  size_t got = fread(buf, 1, n, f);
  fclose(f);
  return got == n && memcmp(buf, magic, n) == 0;
}

static int file_contains(const char *path, const char *needle) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    return 0;
  }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = malloc((size_t)n + 1);
  size_t got = fread(buf, 1, (size_t)n, f);
  buf[got] = 0;
  fclose(f);
  int found = strstr(buf, needle) != NULL;
  free(buf);
  return found;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <outdir>\n", argv[0]);
    return 2;
  }
  const char *outdir = argv[1];
  MtlcContext *ctx = mtlc_context_create();
  mtlc_context_set_opt_level(ctx, 1);
  mtlc_context_set_whole_program(ctx, 1);

  /* 1. native executable (optimized) */
  {
    MtlcModule *m = build_native_module();
    if (!m) {
      return fail("native module construction");
    }
    if (!mtlc_optimize(ctx, m)) {
      return fail("mtlc_optimize");
    }
    char *exe = path_join(outdir, "pubapi_native.exe");
    if (!mtlc_build_executable(ctx, m, exe)) {
      return fail("mtlc_build_executable");
    }
    printf("native: %s\n", exe);
    free(exe);
    mtlc_module_destroy(m);
  }

  /* 2. PTX + SPIR-V from the same kernel module shape (unoptimized IR) */
  {
    MtlcModule *m = build_gpu_module();
    if (!m) {
      return fail("gpu module construction");
    }
    char *ptx = path_join(outdir, "pubapi_kernel.ptx");
    char *spv = path_join(outdir, "pubapi_kernel.spv");
    if (!mtlc_emit(ctx, m, MTLC_ARCH_PTX, ptx)) {
      return fail("mtlc_emit PTX");
    }
    if (!file_contains(ptx, ".visible .entry scale2")) {
      return fail("PTX output missing the kernel entry");
    }
    if (!mtlc_emit(ctx, m, MTLC_ARCH_SPIRV, spv)) {
      return fail("mtlc_emit SPIR-V");
    }
    const unsigned char spv_magic[4] = {0x03, 0x02, 0x23, 0x07}; /* LE */
    if (!file_starts_with(spv, spv_magic, 4)) {
      return fail("SPIR-V output missing the module magic");
    }
    printf("gpu: %s, %s\n", ptx, spv);
    free(ptx);
    free(spv);
    mtlc_module_destroy(m);
  }

  /* 3. AArch64 static ELF (unoptimized IR) */
  {
    MtlcModule *m = build_arm64_module();
    if (!m) {
      return fail("arm64 module construction");
    }
    char *elf = path_join(outdir, "pubapi_arm64.elf");
    if (!mtlc_emit(ctx, m, MTLC_ARCH_ARM64, elf)) {
      return fail("mtlc_emit ARM64");
    }
    const unsigned char elf_magic[4] = {0x7f, 'E', 'L', 'F'};
    if (!file_starts_with(elf, elf_magic, 4)) {
      return fail("ARM64 output missing the ELF magic");
    }
    printf("arm64: %s\n", elf);
    free(elf);
    mtlc_module_destroy(m);
  }

  mtlc_context_destroy(ctx);
  printf("public_api_test: all surfaces OK\n");
  return 0;
}
