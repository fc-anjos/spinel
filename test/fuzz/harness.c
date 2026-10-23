/* libFuzzer host over the fuzz kernel (kernel.rb, emitted with --ext-entry).
   FUZZ_TARGET names the entry this binary drives: marshal, unpack or regexp.
   A Ruby raise is a handled outcome, caught through the try helper; only a
   sanitizer report, a signal or a timeout is a finding. */
#include <stddef.h>
#include <stdint.h>
#include "k.h"

#define FUZZ_CAT2(a, b) a##b
#define FUZZ_CAT(a, b) FUZZ_CAT2(a, b)
#define FUZZ_ENTRY FUZZ_CAT(sp_FuzzKernel_s_, FUZZ_TARGET)

typedef struct { const char *s; } call_t;
static void call_entry(void *p) {
  FUZZ_ENTRY(((call_t *)p)->s);
}

int LLVMFuzzerInitialize(int *argc, char ***argv) {
  (void)argc; (void)argv;
  Init_fuzz_kernel();
  return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  call_t c = { sp_str_from_bytes((const char *)data, size) };
  const char *cls = 0, *msg = 0;
  Init_fuzz_kernel_try(call_entry, &c, &cls, &msg);
  return 0;
}
