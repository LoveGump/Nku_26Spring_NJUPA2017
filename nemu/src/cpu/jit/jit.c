#include "cpu/jit.h"

#ifdef CONFIG_JIT

typedef struct {
  bool enabled;
  TB tb_cache[JIT_TB_CACHE_SIZE];
  TB *active_tb;
  bool ignoring_sealed_tb;
  vaddr_t ignore_until;
  JITStats stats;
} JITState;

static JITState jit_state;

static inline uint32_t tb_index(vaddr_t eip) {
  return (eip >> 2) & (JIT_TB_CACHE_SIZE - 1);
}

static void tb_seal(TB *tb) {
  if (tb == NULL || !tb->valid || tb->sealed) {
    return;
  }

  tb->sealed = true;
  jit_state.stats.sealed_tbs ++;
  if (tb->nr_instr > jit_state.stats.max_tb_instr) {
    jit_state.stats.max_tb_instr = tb->nr_instr;
  }
  if (jit_state.active_tb == tb) {
    jit_state.active_tb = NULL;
  }
}

void jit_init(void) {
  jit_state.enabled = true;
  jit_state.active_tb = NULL;
  jit_state.ignoring_sealed_tb = false;
  jit_state.ignore_until = 0;
  memset(&jit_state.stats, 0, sizeof(jit_state.stats));
  jit_invalidate_all();
}

void jit_reset(void) {
  jit_invalidate_all();
}

void jit_invalidate_all(void) {
  jit_state.active_tb = NULL;
  jit_state.ignoring_sealed_tb = false;
  jit_state.ignore_until = 0;

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
  tb->sealed = false;
  tb->guest_start = eip;
  tb->guest_end = eip;
  tb->exit_eip = eip;
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

TB *jit_lookup_sealed(vaddr_t eip) {
  TB *tb = tb_lookup(eip);
  if (tb == NULL || !tb->sealed) {
    return NULL;
  }

  return tb;
}

void jit_begin_tb_exec(TB *tb) {
  if (tb == NULL || !tb->valid || !tb->sealed) {
    return;
  }

  jit_state.ignoring_sealed_tb = true;
  jit_state.ignore_until = tb->guest_end;
  jit_state.stats.executed_tbs ++;
}

void jit_end_tb_exec(uint32_t nr_instr, bool aborted) {
  jit_state.ignoring_sealed_tb = false;
  jit_state.ignore_until = 0;
  jit_state.stats.executed_instr += nr_instr;
  if (aborted) {
    jit_state.stats.aborted_tbs ++;
  }
}

void jit_record_instr(vaddr_t start, vaddr_t end, vaddr_t next_eip, bool end_of_tb) {
  if (!jit_state.enabled || start == end) {
    return;
  }

  if (jit_state.ignoring_sealed_tb) {
    if (end_of_tb || end >= jit_state.ignore_until) {
      jit_state.ignoring_sealed_tb = false;
      jit_state.ignore_until = 0;
    }
    return;
  }

  TB *tb = jit_state.active_tb;
  if (tb == NULL || !tb->valid || tb->sealed || tb->exit_eip != start) {
    tb = tb_lookup(start);
    if (tb != NULL && tb->sealed) {
      jit_state.ignoring_sealed_tb = true;
      jit_state.ignore_until = tb->guest_end;
      if (end_of_tb || end >= jit_state.ignore_until) {
        jit_state.ignoring_sealed_tb = false;
        jit_state.ignore_until = 0;
      }
      return;
    }

    tb = tb_alloc(start);
    jit_state.active_tb = tb;
  }

  tb->guest_end = end;
  tb->exit_eip = next_eip;
  tb->nr_instr ++;
  jit_state.stats.recorded_instr ++;

  if (end_of_tb || tb->nr_instr >= JIT_MAX_TB_INSTR) {
    tb_seal(tb);
  }
}

#endif
