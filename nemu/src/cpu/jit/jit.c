#include "cpu/jit.h"

#ifdef CONFIG_JIT

typedef struct {
  bool enabled;
  TB tb_cache[JIT_TB_CACHE_SIZE];
  JITStats stats;
} JITState;

static JITState jit_state;

static inline uint32_t tb_index(vaddr_t eip) {
  return (eip >> 2) & (JIT_TB_CACHE_SIZE - 1);
}

void jit_init(void) {
  jit_state.enabled = true;
  memset(&jit_state.stats, 0, sizeof(jit_state.stats));
  jit_invalidate_all();
}

void jit_reset(void) {
  jit_invalidate_all();
}

void jit_invalidate_all(void) {
  for (int i = 0; i < JIT_TB_CACHE_SIZE; i ++) {
    tb_invalidate(&jit_state.tb_cache[i]);
  }
  jit_state.stats.invalidations ++;
}

TB *tb_lookup(vaddr_t eip) {
  jit_state.stats.lookups ++;

  TB *tb = &jit_state.tb_cache[tb_index(eip)];
  if (tb->valid && tb->guest_start == eip) {
    tb->hit_count ++;
    jit_state.stats.hits ++;
    return tb;
  }

  jit_state.stats.misses ++;
  return NULL;
}

TB *tb_alloc(vaddr_t eip) {
  TB *tb = &jit_state.tb_cache[tb_index(eip)];
  tb_invalidate(tb);

  tb->valid = true;
  tb->guest_start = eip;
  tb->guest_end = eip;
  tb->nr_instr = 0;
  tb->hit_count = 0;
  tb->host_code = NULL;
  tb->host_size = 0;
  jit_state.stats.translations ++;

  return tb;
}

void tb_invalidate(TB *tb) {
  if (tb == NULL) {
    return;
  }

  if (tb->valid) {
    jit_state.stats.total_instr += tb->nr_instr;
  }
  memset(tb, 0, sizeof(*tb));
}

const JITStats *jit_get_stats(void) {
  return &jit_state.stats;
}

#endif
