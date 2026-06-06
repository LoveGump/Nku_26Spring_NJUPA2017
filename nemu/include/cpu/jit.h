#ifndef __CPU_JIT_H__
#define __CPU_JIT_H__

#include "nemu.h"

#ifdef CONFIG_JIT

#define JIT_TB_CACHE_SIZE 4096

typedef struct TranslationBlock {
  bool valid;
  vaddr_t guest_start;
  vaddr_t guest_end;
  uint32_t nr_instr;
  uint64_t hit_count;
  void *host_code;
  uint32_t host_size;
} TB;

typedef struct {
  uint64_t lookups;
  uint64_t hits;
  uint64_t misses;
  uint64_t translations;
  uint64_t invalidations;
  uint64_t total_instr;
} JITStats;

void jit_init(void);
void jit_reset(void);
void jit_invalidate_all(void);
TB *tb_lookup(vaddr_t eip);
TB *tb_alloc(vaddr_t eip);
void tb_invalidate(TB *tb);
const JITStats *jit_get_stats(void);

#else

static inline void jit_init(void) {}
static inline void jit_reset(void) {}
static inline void jit_invalidate_all(void) {}

#endif

#endif
