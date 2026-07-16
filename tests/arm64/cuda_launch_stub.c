/* Hardware-free provider for tests/test_gpu_dispatch.mettle. On AArch64 it
 * additionally verifies the AAPCS64 placement of all eleven cuLaunchKernel
 * arguments, including the three overflow arguments carried on the stack. */
#include <stdint.h>

static int launch_calls;

int cuLaunchKernel(int64_t function, uint32_t gx, uint32_t gy, uint32_t gz,
                   uint32_t bx, uint32_t by, uint32_t bz,
                   uint32_t shared_bytes, int64_t stream,
                   int64_t *kernel_params, int64_t *extra) {
  int valid = function == 0 && kernel_params != 0 && extra == 0;
  if (launch_calls == 0) {
    valid = valid && gx == 4 && gy == 1 && gz == 1 && bx == 256 &&
            by == 1 && bz == 1 && shared_bytes == 0 && stream == 0;
  } else if (launch_calls == 1) {
    valid = valid && gx == 7 && gy == 3 && gz == 2 && bx == 32 &&
            by == 4 && bz == 1 && shared_bytes == 4096 && stream == 99;
  } else {
    valid = 0;
  }
  if (!valid) {
    return 97;
  }
  launch_calls++;
  return 0;
}

int gpu_stub_finish(void) { return launch_calls == 2 ? 0 : 96; }
