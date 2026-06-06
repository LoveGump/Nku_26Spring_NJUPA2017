#include "cpu/jit.h"

#ifdef CONFIG_JIT

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

typedef struct {
  bool enabled;
  TB tb_cache[JIT_TB_CACHE_SIZE];
  TB *active_tb;
  bool ignoring_sealed_tb;
  vaddr_t ignore_until;
  uint8_t *code_cache;
  size_t code_capacity;
  size_t code_size;
  bool code_writable;
  JITStats stats;
} JITState;

static JITState jit_state;

static inline uint32_t tb_index(vaddr_t eip) {
  return (eip >> 2) & (JIT_TB_CACHE_SIZE - 1);
}

static inline size_t align_up(size_t value, size_t align) {
  return (value + align - 1) & ~(align - 1);
}

static void jit_code_protect(int prot, bool writable) {
  if (jit_state.code_cache == NULL) {
    return;
  }

  /* code cache 在写入和执行之间切换权限，避免长期保持 RWX。 */
  int ret = mprotect(jit_state.code_cache, jit_state.code_capacity, prot);
  Assert(ret == 0, "mprotect JIT code cache failed: %s", strerror(errno));
  jit_state.code_writable = writable;
}

static void jit_code_make_writable(void) {
  if (!jit_state.code_writable) {
    jit_code_protect(PROT_READ | PROT_WRITE, true);
  }
}

static void jit_code_make_executable(void) {
  if (jit_state.code_writable) {
    jit_code_protect(PROT_READ | PROT_EXEC, false);
  }
}

static void jit_code_init(void) {
  if (jit_state.code_cache != NULL) {
    return;
  }

  /* 第一版先使用固定大小的线性 code cache，后续不够时整体清空重来。 */
  jit_state.code_capacity = JIT_CODE_CACHE_SIZE;
  jit_state.code_cache = mmap(NULL, jit_state.code_capacity,
      PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  Assert(jit_state.code_cache != MAP_FAILED, "mmap JIT code cache failed: %s", strerror(errno));
  jit_state.code_size = 0;
  jit_state.code_writable = true;
}

static void jit_code_reset(void) {
  jit_code_make_writable();
  jit_state.code_size = 0;
  jit_state.stats.code_bytes = 0;
  jit_state.stats.code_flushes ++;
}

static uint8_t *jit_code_alloc(size_t size) {
  size_t aligned_size = align_up(size, 16);
  if (jit_state.code_size + aligned_size > jit_state.code_capacity) {
    /* 简化淘汰策略：代码缓存满时丢弃所有 TB 和 host code。 */
    jit_invalidate_all();
    jit_code_reset();
  }

  Assert(jit_state.code_size + aligned_size <= jit_state.code_capacity,
      "JIT code block is too large: %zu bytes", size);
  jit_code_make_writable();

  uint8_t *ptr = jit_state.code_cache + jit_state.code_size;
  jit_state.code_size += aligned_size;
  jit_state.stats.code_bytes = jit_state.code_size;
  return ptr;
}

static inline void emit_u8(uint8_t **cursor, uint8_t value) {
  *(*cursor)++ = value;
}

static inline void emit_u32(uint8_t **cursor, uint32_t value) {
  memcpy(*cursor, &value, sizeof(value));
  *cursor += sizeof(value);
}

static inline void __attribute__((unused)) emit_u64(uint8_t **cursor, uint64_t value) {
  memcpy(*cursor, &value, sizeof(value));
  *cursor += sizeof(value);
}

static void emit_mov_rax_imm64(uint8_t **cursor, uint64_t value) {
  emit_u8(cursor, 0x48);
  emit_u8(cursor, 0xb8);
  emit_u64(cursor, value);
}

static void emit_mov_m32_rax_imm32(uint8_t **cursor, uint32_t value) {
  emit_u8(cursor, 0xc7);
  emit_u8(cursor, 0x00);
  emit_u32(cursor, value);
}

static void emit_ret(uint8_t **cursor) {
  emit_u8(cursor, 0xc3);
}

static void emit_return_status(uint8_t **cursor, int status) {
  /* x86-64: mov eax, imm32; ret。返回值按 SysV ABI 放在 eax。 */
  emit_u8(cursor, 0xb8);
  emit_u32(cursor, (uint32_t)status);
  emit_ret(cursor);
}

static void __attribute__((unused)) emit_call(uint8_t **cursor, void *target) {
  /* call 前调整栈，让被调 C helper 看到 16 字节对齐的栈。 */
  emit_u8(cursor, 0x48);
  emit_u8(cursor, 0x83);
  emit_u8(cursor, 0xec);
  emit_u8(cursor, 0x08);

  uint8_t *next = *cursor + 5;
  intptr_t rel = (uint8_t *)target - next;
  Assert(rel >= INT32_MIN && rel <= INT32_MAX, "JIT call target is out of rel32 range");
  emit_u8(cursor, 0xe8);
  emit_u32(cursor, (uint32_t)(int32_t)rel);

  emit_u8(cursor, 0x48);
  emit_u8(cursor, 0x83);
  emit_u8(cursor, 0xc4);
  emit_u8(cursor, 0x08);
}

static bool tb_is_single_nop(TB *tb) {
  /* 第一版 codegen 只处理无副作用的单字节 nop。 */
  return tb->nr_instr == 1 &&
    tb->guest_end == tb->guest_start + 1 &&
    vaddr_read(tb->guest_start, 1) == 0x90;
}

static void jit_compile_tb(TB *tb) {
  if (tb == NULL || !tb->valid || !tb->sealed || tb->host_code != NULL) {
    return;
  }

  if (!tb_is_single_nop(tb)) {
    return;
  }

  uint8_t *code = jit_code_alloc(32);
  uint8_t *cursor = code;

  /* nop 不修改通用寄存器和 EFLAGS，native 代码只需要推进 eip。 */
  emit_mov_rax_imm64(&cursor, (uint64_t)(uintptr_t)&cpu.eip);
  emit_mov_m32_rax_imm32(&cursor, tb->exit_eip);
  emit_return_status(&cursor, JIT_EXEC_OK);
  jit_code_make_executable();

  tb->host_code = code;
  tb->host_size = cursor - code;
  jit_state.stats.native_tbs ++;
  jit_state.stats.native_instr += tb->nr_instr;
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
  jit_compile_tb(tb);
}

void jit_init(void) {
  memset(&jit_state, 0, sizeof(jit_state));
  jit_state.enabled = true;
  jit_state.active_tb = NULL;
  jit_state.ignoring_sealed_tb = false;
  jit_state.ignore_until = 0;
  jit_code_init();
  jit_invalidate_all();
  int ret = jit_code_self_test();
  Assert(ret == JIT_EXEC_OK, "JIT code self-test failed: %d", ret);
}

void jit_reset(void) {
  jit_invalidate_all();
  jit_code_reset();
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

jit_func_t jit_emit_return(int status, uint32_t *host_size) {
  uint8_t *code = jit_code_alloc(16);
  uint8_t *cursor = code;
  emit_return_status(&cursor, status);
  /* 写完立刻切到 RX，当前阶段只验证最小 native code 可执行。 */
  jit_code_make_executable();

  if (host_size != NULL) {
    *host_size = cursor - code;
  }
  return (jit_func_t)code;
}

int jit_code_self_test(void) {
  uint32_t host_size = 0;
  jit_func_t fn = jit_emit_return(JIT_EXEC_OK, &host_size);
  Assert(fn != NULL && host_size == 6, "unexpected JIT self-test code size: %u", host_size);
  /* 直接调用刚生成的 host code，确认 mmap/mprotect/codegen 三件事都可用。 */
  int ret = fn();
  jit_state.stats.code_self_tests ++;
  return ret;
}

TB *jit_lookup_sealed(vaddr_t eip) {
  TB *tb = tb_lookup(eip);
  if (tb == NULL || !tb->sealed) {
    return NULL;
  }

  return tb;
}

bool jit_tb_has_native(TB *tb) {
  return tb != NULL && tb->valid && tb->sealed && tb->host_code != NULL;
}

int jit_exec_native(TB *tb) {
  if (!jit_tb_has_native(tb)) {
    jit_state.stats.native_fallbacks ++;
    return JIT_EXEC_FALLBACK;
  }

  jit_func_t fn = (jit_func_t)tb->host_code;
  jit_state.stats.native_calls ++;
  return fn();
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
