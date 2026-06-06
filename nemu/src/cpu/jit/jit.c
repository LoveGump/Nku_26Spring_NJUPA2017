#include "cpu/jit.h"

#ifdef CONFIG_JIT

typedef struct {
  bool enabled;
  uint64_t invalidations;
} JITState;

static JITState jit_state;

void jit_init(void) {
  jit_state.enabled = true;
  jit_state.invalidations = 0;
}

void jit_reset(void) {
  jit_invalidate_all();
}

void jit_invalidate_all(void) {
  jit_state.invalidations ++;
}

#endif
