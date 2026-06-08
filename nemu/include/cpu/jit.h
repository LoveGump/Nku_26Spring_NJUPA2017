#ifndef __CPU_JIT_H__
#define __CPU_JIT_H__

#include "nemu.h"

#ifdef CONFIG_JIT

#define JIT_TB_CACHE_BITS 12
#define JIT_TB_CACHE_SIZE (1u << JIT_TB_CACHE_BITS)
#define JIT_MAX_TB_INSTR 16
#define JIT_CODE_CACHE_SIZE (1024 * 1024)
/* TB 至少被缓存命中三次后才值得生成 native code。 */
#define JIT_COMPILE_HIT_THRESHOLD 3

static inline uint32_t jit_tb_index(vaddr_t eip) {
  /*
   * x86 指令并不按 4 字节对齐，不能丢弃 eip 低两位。
   * 乘法散列使用完整地址，减少相邻变长指令和不同代码页之间的槽位冲突。
   */
  return (uint32_t)(eip * 2654435761u) >> (32 - JIT_TB_CACHE_BITS);
}

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

static inline bool jit_tb_has_native(const TB *tb) {
  return tb != NULL && tb->valid && tb->sealed && tb->host_code != NULL;
}

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
  uint64_t unsupported_0f_opcode[256];
  uint64_t unsupported_instr_count[JIT_MAX_TB_INSTR + 1];
  uint64_t native_tbs;
  uint64_t native_instr;
  uint64_t native_calls;
  uint64_t native_executed_instr;
  uint64_t native_fallbacks;
  uint32_t max_tb_instr;
} JITStats;

/* 下面这些状态暴露给 header 内联热路径，避免每条指令跨函数更新统计。 */
extern bool jit_cached_exec_active;
extern uint64_t jit_direct_instr_count;
/* TB cache 和 lookup 计数同样走内联快路径，冷路径仍在 jit.c 中处理。 */
extern TB jit_tb_cache[JIT_TB_CACHE_SIZE];
extern uint64_t jit_lookup_count;
extern uint64_t jit_hit_count;
extern uint64_t jit_miss_count;

void jit_init(void);
void jit_reset(void);
void jit_invalidate_all(void);
void jit_invalidate_range(vaddr_t addr, uint32_t len);
TB *tb_lookup(vaddr_t eip);
TB *tb_alloc(vaddr_t eip);
void tb_invalidate(TB *tb);
const JITStats *jit_get_stats(void);
void jit_record_instr(vaddr_t start, vaddr_t end, vaddr_t next_eip, bool end_of_tb);
static inline void jit_maybe_record_instr(
    vaddr_t start, vaddr_t end, vaddr_t next_eip, bool end_of_tb) {
  if (!jit_cached_exec_active) {
    jit_record_instr(start, end, next_eip, end_of_tb);
  }
}
static inline void jit_record_direct_exec(void) {
  jit_direct_instr_count ++;
}
TB *jit_prepare_hot_tb(TB *tb, vaddr_t eip);
static inline TB *jit_lookup_sealed(vaddr_t eip) {
  jit_lookup_count ++;

  /* direct-mapped TB 命中检查是 cpu_exec() 的高频路径，保持在 header 内联。 */
  TB *tb = &jit_tb_cache[jit_tb_index(eip)];
  if (!tb->valid || tb->guest_start != eip) {
    jit_miss_count ++;
    return NULL;
  }

  tb->hit_count ++;
  jit_hit_count ++;
  if (!tb->sealed) {
    return NULL;
  }

  /* 达到热点阈值时才进入冷路径，避免普通命中跨函数。 */
  if (!tb->compile_attempted && tb->hit_count >= JIT_COMPILE_HIT_THRESHOLD) {
    return jit_prepare_hot_tb(tb, eip);
  }
  return tb;
}
void jit_begin_tb_exec(TB *tb);
void jit_end_tb_exec(uint32_t nr_instr, bool aborted);
jit_func_t jit_emit_return(int status, uint32_t *host_size);
int jit_code_self_test(void);
int jit_exec_native(TB *tb);

#else

static inline void jit_init(void) {}
static inline void jit_reset(void) {}
static inline void jit_invalidate_all(void) {}
static inline void jit_invalidate_range(vaddr_t addr, uint32_t len) {}
static inline void jit_record_instr(vaddr_t start, vaddr_t end, vaddr_t next_eip, bool end_of_tb) {}
static inline void jit_maybe_record_instr(
    vaddr_t start, vaddr_t end, vaddr_t next_eip, bool end_of_tb) {}
static inline void jit_record_direct_exec(void) {}

#endif

#endif
