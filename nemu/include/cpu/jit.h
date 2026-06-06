#ifndef __CPU_JIT_H__
#define __CPU_JIT_H__

#include "nemu.h"

#ifdef CONFIG_JIT

#define JIT_TB_CACHE_SIZE 4096
#define JIT_MAX_TB_INSTR 16
#define JIT_CODE_CACHE_SIZE (1024 * 1024)
/* TB 至少被缓存命中三次后才值得生成 native code。 */
#define JIT_COMPILE_HIT_THRESHOLD 3

typedef int (*jit_func_t)(void);

/* native TB 返回值，用于告诉解释器是否需要回退或停止。 */
enum {
  JIT_EXEC_OK = 0,
  JIT_EXEC_FALLBACK = 1,
  JIT_EXEC_ABORT = 2,
  JIT_EXEC_END = 3,
};

typedef struct TranslationBlock {
  bool valid;
  bool sealed;
  /* 无论支持与否，每个 TB 最多尝试一次 native 编译。 */
  bool compile_attempted;
  vaddr_t guest_start;
  vaddr_t guest_end;
  /* trace 实际执行完后的下一条 eip，可能不同于顺序地址 guest_end。 */
  vaddr_t exit_eip;
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
  uint64_t recorded_instr;
  uint64_t sealed_tbs;
  uint64_t executed_tbs;
  uint64_t executed_instr;
  uint64_t aborted_tbs;
  uint64_t direct_instr;
  uint64_t code_bytes;
  uint64_t code_flushes;
  uint64_t code_self_tests;
  uint64_t compile_attempts;
  uint64_t compile_unsupported;
  uint64_t unsupported_opcode[256];
  uint64_t native_tbs;
  uint64_t native_instr;
  uint64_t native_calls;
  uint64_t native_executed_instr;
  uint64_t native_fallbacks;
  uint32_t max_tb_instr;
} JITStats;

void jit_init(void);
void jit_reset(void);
void jit_invalidate_all(void);
void jit_invalidate_range(vaddr_t addr, uint32_t len);
TB *tb_lookup(vaddr_t eip);
TB *tb_alloc(vaddr_t eip);
void tb_invalidate(TB *tb);
const JITStats *jit_get_stats(void);
void jit_record_instr(vaddr_t start, vaddr_t end, vaddr_t next_eip, bool end_of_tb);
void jit_record_direct_exec(void);
TB *jit_lookup_sealed(vaddr_t eip);
void jit_begin_tb_exec(TB *tb);
void jit_end_tb_exec(uint32_t nr_instr, bool aborted);
jit_func_t jit_emit_return(int status, uint32_t *host_size);
int jit_code_self_test(void);
bool jit_tb_has_native(TB *tb);
int jit_exec_native(TB *tb);

#else

static inline void jit_init(void) {}
static inline void jit_reset(void) {}
static inline void jit_invalidate_all(void) {}
static inline void jit_invalidate_range(vaddr_t addr, uint32_t len) {}
static inline void jit_record_instr(vaddr_t start, vaddr_t end, vaddr_t next_eip, bool end_of_tb) {}
static inline void jit_record_direct_exec(void) {}

#endif

#endif
