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

static void emit_mov_ecx_m32_rax(uint8_t **cursor) {
  emit_u8(cursor, 0x8b);
  emit_u8(cursor, 0x08);
}

static void emit_mov_m32_rax_ecx(uint8_t **cursor) {
  emit_u8(cursor, 0x89);
  emit_u8(cursor, 0x08);
}

static void emit_mov_edi_imm32(uint8_t **cursor, uint32_t value) {
  emit_u8(cursor, 0xbf);
  emit_u32(cursor, value);
}

static void emit_mov_esi_imm32(uint8_t **cursor, uint32_t value) {
  emit_u8(cursor, 0xbe);
  emit_u32(cursor, value);
}

static void emit_mov_edx_imm32(uint8_t **cursor, uint32_t value) {
  emit_u8(cursor, 0xba);
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

  /* code cache 可能离 helper 超过 rel32 范围，使用绝对间接调用更稳。 */
  emit_mov_rax_imm64(cursor, (uint64_t)(uintptr_t)target);
  emit_u8(cursor, 0xff);
  emit_u8(cursor, 0xd0);

  emit_u8(cursor, 0x48);
  emit_u8(cursor, 0x83);
  emit_u8(cursor, 0xc4);
  emit_u8(cursor, 0x08);
}

enum {
  JIT_ARITH_ADD = 0,
  JIT_ARITH_SUB = 1,
};

enum {
  JIT_LOGIC_OR = 0,
  JIT_LOGIC_AND = 1,
  JIT_LOGIC_XOR = 2,
};

enum {
  JIT_UNARY_INC = 0,
  JIT_UNARY_DEC = 1,
};

enum {
  JIT_COMPARE_CMP = 0,
  JIT_COMPARE_TEST = 1,
};

enum {
  JIT_MEM_MOFFS_READ = 0,
  JIT_MEM_MOFFS_WRITE = 1,
};

enum {
  JIT_MEM_RM_READ = 0,
  JIT_MEM_RM_WRITE = 1,
};

static int jit_helper_arith_r2r(uint32_t info, uint32_t exit_eip) {
  /* info: bit[2:0]=src，bit[5:3]=dest，bit[6]=add/sub。 */
  uint8_t src = info & 0x7;
  uint8_t dest = (info >> 3) & 0x7;
  uint8_t op = (info >> 6) & 0x1;

  uint32_t dest_val = cpu.gpr[dest]._32;
  uint32_t src_val = cpu.gpr[src]._32;
  uint32_t result = 0;

  if (op == JIT_ARITH_ADD) {
    result = dest_val + src_val;
    /* add: 结果回绕表示无符号进位；同号输入得到异号结果表示溢出。 */
    cpu.CF = result < dest_val;
    cpu.OF = ((~(dest_val ^ src_val) & (dest_val ^ result)) >> 31) & 0x1;
  }
  else {
    result = dest_val - src_val;
    /* sub: 被减数小于减数表示借位；异号输入且结果变号表示溢出。 */
    cpu.CF = dest_val < src_val;
    cpu.OF = (((dest_val ^ src_val) & (dest_val ^ result)) >> 31) & 0x1;
  }

  cpu.gpr[dest]._32 = result;
  cpu.ZF = result == 0;
  cpu.SF = (result >> 31) & 0x1;
  cpu.eip = exit_eip;
  return JIT_EXEC_OK;
}

static int jit_helper_logic_r2r(uint32_t info, uint32_t exit_eip) {
  /* info: bit[2:0]=src，bit[5:3]=dest，bit[7:6]=or/and/xor。 */
  uint8_t src = info & 0x7;
  uint8_t dest = (info >> 3) & 0x7;
  uint8_t op = (info >> 6) & 0x3;

  uint32_t dest_val = cpu.gpr[dest]._32;
  uint32_t src_val = cpu.gpr[src]._32;
  uint32_t result = 0;

  if (op == JIT_LOGIC_OR) {
    result = dest_val | src_val;
  }
  else if (op == JIT_LOGIC_AND) {
    result = dest_val & src_val;
  }
  else {
    result = dest_val ^ src_val;
  }

  cpu.gpr[dest]._32 = result;
  /* x86 逻辑运算按结果更新 ZF/SF，并把 CF/OF 清零。 */
  cpu.CF = 0;
  cpu.OF = 0;
  cpu.ZF = result == 0;
  cpu.SF = (result >> 31) & 0x1;
  cpu.eip = exit_eip;
  return JIT_EXEC_OK;
}

static int jit_helper_unary_reg(uint32_t info, uint32_t exit_eip) {
  /* info: bit[2:0]=register，bit[3]=inc/dec。 */
  uint8_t reg = info & 0x7;
  uint8_t op = (info >> 3) & 0x1;
  uint32_t old_value = cpu.gpr[reg]._32;
  uint32_t result = 0;

  if (op == JIT_UNARY_INC) {
    /* 最大正数加一变成最小负数时产生有符号溢出。 */
    result = old_value + 1;
    cpu.OF = old_value == 0x7fffffff;
  }
  else {
    /* 最小负数减一变成最大正数时产生有符号溢出。 */
    result = old_value - 1;
    cpu.OF = old_value == 0x80000000;
  }

  /* inc/dec 不修改 CF，只更新结果相关的 ZF、SF、OF。 */
  cpu.gpr[reg]._32 = result;
  cpu.ZF = result == 0;
  cpu.SF = (result >> 31) & 0x1;
  cpu.eip = exit_eip;
  return JIT_EXEC_OK;
}

static int jit_helper_compare_r2r(uint32_t info, uint32_t exit_eip) {
  /* info: bit[2:0]=src，bit[5:3]=dest，bit[6]=cmp/test。 */
  uint8_t src = info & 0x7;
  uint8_t dest = (info >> 3) & 0x7;
  uint8_t op = (info >> 6) & 0x1;
  uint32_t dest_val = cpu.gpr[dest]._32;
  uint32_t src_val = cpu.gpr[src]._32;
  uint32_t result = 0;

  if (op == JIT_COMPARE_CMP) {
    /* cmp 复用 sub 的标志位公式，但不写回减法结果。 */
    result = dest_val - src_val;
    cpu.CF = dest_val < src_val;
    cpu.OF = (((dest_val ^ src_val) & (dest_val ^ result)) >> 31) & 0x1;
  }
  else {
    /* test 复用 and 的标志位语义，CF/OF 固定清零。 */
    result = dest_val & src_val;
    cpu.CF = 0;
    cpu.OF = 0;
  }

  /* cmp/test 只更新标志位，不把临时结果写回客户寄存器。 */
  cpu.ZF = result == 0;
  cpu.SF = (result >> 31) & 0x1;
  cpu.eip = exit_eip;
  return JIT_EXEC_OK;
}

static int jit_helper_moffs32(uint32_t addr, uint32_t exit_eip, uint32_t op) {
  /* moffs32 是指令里直接携带的线性地址；访存必须继续走 vaddr_*。 */
  if (op == JIT_MEM_MOFFS_READ) {
    cpu.eax = vaddr_read(addr, 4);
  }
  else {
    vaddr_write(addr, 4, cpu.eax);
  }

  cpu.eip = exit_eip;
  return JIT_EXEC_OK;
}

static int jit_helper_mov_rm32(uint32_t info, uint32_t disp, uint32_t exit_eip) {
  /*
   * info:
   * bit[0]=read/write，bit[3:1]=reg，bit[6:4]=base，bit[7]=has_base，
   * bit[10:8]=index，bit[11]=has_index，bit[13:12]=scale。
   */
  uint8_t op = info & 0x1;
  uint8_t reg = (info >> 1) & 0x7;
  uint8_t base = (info >> 4) & 0x7;
  uint8_t has_base = (info >> 7) & 0x1;
  uint8_t index = (info >> 8) & 0x7;
  uint8_t has_index = (info >> 11) & 0x1;
  uint8_t scale = (info >> 12) & 0x3;
  /* 有符号位移先扩展到 32 位，再叠加可选基址寄存器形成有效地址。 */
  uint32_t addr = (uint32_t)(int32_t)disp;

  if (has_base) {
    addr += cpu.gpr[base]._32;
  }
  if (has_index) {
    /* SIB 的 scale 编码就是左移位数: 0/1/2/3 分别表示乘 1/2/4/8。 */
    addr += cpu.gpr[index]._32 << scale;
  }

  if (op == JIT_MEM_RM_READ) {
    cpu.gpr[reg]._32 = vaddr_read(addr, 4);
  }
  else {
    vaddr_write(addr, 4, cpu.gpr[reg]._32);
  }

  cpu.eip = exit_eip;
  return JIT_EXEC_OK;
}

static bool tb_is_single_nop(TB *tb) {
  /* 第一版 codegen 只处理无副作用的单字节 nop。 */
  return tb->nr_instr == 1 &&
    tb->guest_end == tb->guest_start + 1 &&
    vaddr_read(tb->guest_start, 1) == 0x90;
}

static bool tb_is_single_mov_i2r(TB *tb, uint8_t *reg, uint32_t *imm) {
  /* 0xb8..0xbf 是 mov imm32 -> r32，编码固定为 opcode + imm32。 */
  if (tb->nr_instr != 1 || tb->guest_end != tb->guest_start + 5) {
    return false;
  }

  uint8_t opcode = vaddr_read(tb->guest_start, 1);
  if (opcode < 0xb8 || opcode > 0xbf) {
    return false;
  }

  *reg = opcode & 0x7;
  /* x86 guest 立即数为小端，vaddr_read(..., 4) 正好还原成 uint32_t。 */
  *imm = vaddr_read(tb->guest_start + 1, 4);
  return true;
}

static bool tb_is_single_mov_r2r(TB *tb, uint8_t *src, uint8_t *dest) {
  /* 0x89/0x8b 共用 ModR/M；第一版只接受 mod=3 的纯寄存器形态。 */
  if (tb->nr_instr != 1 || tb->guest_end != tb->guest_start + 2) {
    return false;
  }

  uint8_t opcode = vaddr_read(tb->guest_start, 1);
  if (opcode != 0x89 && opcode != 0x8b) {
    return false;
  }

  uint8_t modrm = vaddr_read(tb->guest_start + 1, 1);
  if ((modrm >> 6) != 3) {
    return false;
  }

  uint8_t reg = (modrm >> 3) & 0x7;
  uint8_t rm = modrm & 0x7;
  /* 0x89: r/m32 <- r32；0x8b: r32 <- r/m32。 */
  if (opcode == 0x89) {
    *src = reg;
    *dest = rm;
  }
  else {
    *src = rm;
    *dest = reg;
  }

  return true;
}

static bool tb_is_single_arith_r2r(TB *tb, uint8_t *op, uint8_t *src, uint8_t *dest) {
  /* 仅支持 add/sub 的 32 位、两字节、ModR/M mod=3 寄存器编码。 */
  if (tb->nr_instr != 1 || tb->guest_end != tb->guest_start + 2) {
    return false;
  }

  uint8_t opcode = vaddr_read(tb->guest_start, 1);
  if (opcode != 0x01 && opcode != 0x03 && opcode != 0x29 && opcode != 0x2b) {
    return false;
  }

  uint8_t modrm = vaddr_read(tb->guest_start + 1, 1);
  if ((modrm >> 6) != 3) {
    return false;
  }

  uint8_t reg = (modrm >> 3) & 0x7;
  uint8_t rm = modrm & 0x7;
  /* 0x01/0x29 写 r/m；0x03/0x2b 写 reg。 */
  if (opcode == 0x01 || opcode == 0x29) {
    *src = reg;
    *dest = rm;
  }
  else {
    *src = rm;
    *dest = reg;
  }
  *op = (opcode == 0x01 || opcode == 0x03) ? JIT_ARITH_ADD : JIT_ARITH_SUB;

  return true;
}

static bool tb_is_single_logic_r2r(TB *tb, uint8_t *op, uint8_t *src, uint8_t *dest) {
  /* 只识别 OR/AND/XOR 的 32 位、ModR/M mod=3 寄存器形式。 */
  if (tb->nr_instr != 1 || tb->guest_end != tb->guest_start + 2) {
    return false;
  }

  uint8_t opcode = vaddr_read(tb->guest_start, 1);
  if (opcode != 0x09 && opcode != 0x0b &&
      opcode != 0x21 && opcode != 0x23 &&
      opcode != 0x31 && opcode != 0x33) {
    return false;
  }

  uint8_t modrm = vaddr_read(tb->guest_start + 1, 1);
  if ((modrm >> 6) != 3) {
    return false;
  }

  uint8_t reg = (modrm >> 3) & 0x7;
  uint8_t rm = modrm & 0x7;
  /* 低方向 opcode 写 r/m，高方向 opcode 写 reg。 */
  if (opcode == 0x09 || opcode == 0x21 || opcode == 0x31) {
    *src = reg;
    *dest = rm;
  }
  else {
    *src = rm;
    *dest = reg;
  }

  if (opcode == 0x09 || opcode == 0x0b) {
    *op = JIT_LOGIC_OR;
  }
  else if (opcode == 0x21 || opcode == 0x23) {
    *op = JIT_LOGIC_AND;
  }
  else {
    *op = JIT_LOGIC_XOR;
  }

  return true;
}

static bool tb_is_single_unary_reg(TB *tb, uint8_t *op, uint8_t *reg) {
  /* 0x40..0x47 编码 INC，0x48..0x4f 编码 DEC，低 3 位选择寄存器。 */
  if (tb->nr_instr != 1 || tb->guest_end != tb->guest_start + 1) {
    return false;
  }

  uint8_t opcode = vaddr_read(tb->guest_start, 1);
  if (opcode < 0x40 || opcode > 0x4f) {
    return false;
  }

  *reg = opcode & 0x7;
  *op = opcode < 0x48 ? JIT_UNARY_INC : JIT_UNARY_DEC;
  return true;
}

static bool tb_is_single_compare_r2r(TB *tb, uint8_t *op, uint8_t *src, uint8_t *dest) {
  /* 只识别寄存器形态的 cmp/test；访存形态留给解释器处理。 */
  if (tb->nr_instr != 1 || tb->guest_end != tb->guest_start + 2) {
    return false;
  }

  uint8_t opcode = vaddr_read(tb->guest_start, 1);
  if (opcode != 0x39 && opcode != 0x3b && opcode != 0x85) {
    return false;
  }

  uint8_t modrm = vaddr_read(tb->guest_start + 1, 1);
  if ((modrm >> 6) != 3) {
    return false;
  }

  uint8_t reg = (modrm >> 3) & 0x7;
  uint8_t rm = modrm & 0x7;
  /* 0x39/0x85 写 flags(r/m -/and reg)；0x3b 写 flags(reg - r/m)。 */
  if (opcode == 0x3b) {
    *src = rm;
    *dest = reg;
  }
  else {
    *src = reg;
    *dest = rm;
  }
  *op = opcode == 0x85 ? JIT_COMPARE_TEST : JIT_COMPARE_CMP;

  return true;
}

static bool tb_is_single_moffs32(TB *tb, uint8_t *op, uint32_t *addr) {
  /* 0xa1: eax <- moffs32；0xa3: moffs32 <- eax，编码均为 opcode + imm32。 */
  if (tb->nr_instr != 1 || tb->guest_end != tb->guest_start + 5) {
    return false;
  }

  uint8_t opcode = vaddr_read(tb->guest_start, 1);
  if (opcode != 0xa1 && opcode != 0xa3) {
    return false;
  }

  *op = opcode == 0xa1 ? JIT_MEM_MOFFS_READ : JIT_MEM_MOFFS_WRITE;
  /* 指令里的 moffs32 按小端存放，vaddr_read(..., 4) 还原直接地址。 */
  *addr = vaddr_read(tb->guest_start + 1, 4);
  return true;
}

static bool tb_is_single_mov_rm32(TB *tb, uint8_t *op, uint8_t *reg,
    uint8_t *has_base, uint8_t *base, uint8_t *has_index,
    uint8_t *index, uint8_t *scale, int32_t *disp) {
  /* 当前只识别单条 32 位 mov r/m32 <-> r32，确保 TB 边界和指令长度一致。 */
  if (tb->nr_instr != 1 || tb->guest_end < tb->guest_start + 2) {
    return false;
  }

  uint8_t opcode = vaddr_read(tb->guest_start, 1);
  if (opcode != 0x89 && opcode != 0x8b) {
    return false;
  }

  uint8_t modrm = vaddr_read(tb->guest_start + 1, 1);
  uint8_t mod = modrm >> 6;
  uint8_t rm = modrm & 0x7;
  if (mod == 3) {
    /* mod=3 是寄存器形式，已经由寄存器 mov 路径处理。 */
    return false;
  }

  *reg = (modrm >> 3) & 0x7;
  *base = rm;
  *has_base = true;
  *index = 0;
  *has_index = false;
  *scale = 0;
  *disp = 0;
  uint32_t len = 2;

  if (rm == R_ESP) {
    /* rm=esp 表示后接 SIB 字节，地址形如 base + index * scale + disp。 */
    if (tb->guest_end < tb->guest_start + 3) {
      return false;
    }

    uint8_t sib = vaddr_read(tb->guest_start + 2, 1);
    *scale = sib >> 6;
    *index = (sib >> 3) & 0x7;
    *base = sib & 0x7;
    /* x86 规定 SIB index=100b 不表示 esp，而表示没有 index 项。 */
    *has_index = *index != R_ESP;
    *has_base = !(mod == 0 && *base == R_EBP);
    len = 3;

    if (mod == 0 && *base == R_EBP) {
      /* SIB 中 mod=00 base=101 同样表示无基址，后面跟 disp32。 */
      *disp = vaddr_read(tb->guest_start + 3, 4);
      len = 7;
    }
    else if (mod == 1) {
      *disp = (int8_t)vaddr_read(tb->guest_start + 3, 1);
      len = 4;
    }
    else if (mod == 2) {
      *disp = vaddr_read(tb->guest_start + 3, 4);
      len = 7;
    }
  }
  else {
    if (mod == 0 && rm == R_EBP) {
      /* mod=00 rm=101 在 32 位地址编码中不是 ebp，而是 disp32 绝对地址。 */
      *has_base = false;
      *disp = vaddr_read(tb->guest_start + 2, 4);
      len = 6;
    }
    else if (mod == 1) {
      /* disp8 需要按有符号数扩展，例如 -4(%ebp)。 */
      *disp = (int8_t)vaddr_read(tb->guest_start + 2, 1);
      len = 3;
    }
    else if (mod == 2) {
      *disp = vaddr_read(tb->guest_start + 2, 4);
      len = 6;
    }
  }

  if (tb->nr_instr != 1 || tb->guest_end != tb->guest_start + len) {
    return false;
  }

  *op = opcode == 0x8b ? JIT_MEM_RM_READ : JIT_MEM_RM_WRITE;
  return true;
}

static void jit_compile_tb(TB *tb) {
  if (tb == NULL || !tb->valid || !tb->sealed || tb->host_code != NULL) {
    return;
  }

  bool is_nop = tb_is_single_nop(tb);
  uint8_t reg = 0;
  uint32_t imm = 0;
  bool is_mov_i2r = tb_is_single_mov_i2r(tb, &reg, &imm);
  uint8_t src = 0;
  uint8_t dest = 0;
  bool is_mov_r2r = tb_is_single_mov_r2r(tb, &src, &dest);
  uint8_t arith_op = 0;
  uint8_t arith_src = 0;
  uint8_t arith_dest = 0;
  bool is_arith_r2r = tb_is_single_arith_r2r(tb, &arith_op, &arith_src, &arith_dest);
  uint8_t logic_op = 0;
  uint8_t logic_src = 0;
  uint8_t logic_dest = 0;
  bool is_logic_r2r = tb_is_single_logic_r2r(tb, &logic_op, &logic_src, &logic_dest);
  uint8_t unary_op = 0;
  uint8_t unary_reg = 0;
  bool is_unary_reg = tb_is_single_unary_reg(tb, &unary_op, &unary_reg);
  uint8_t compare_op = 0;
  uint8_t compare_src = 0;
  uint8_t compare_dest = 0;
  bool is_compare_r2r =
    tb_is_single_compare_r2r(tb, &compare_op, &compare_src, &compare_dest);
  uint8_t moffs_op = 0;
  uint32_t moffs_addr = 0;
  bool is_moffs32 = tb_is_single_moffs32(tb, &moffs_op, &moffs_addr);
  uint8_t mem_op = 0;
  uint8_t mem_reg = 0;
  uint8_t mem_has_base = 0;
  uint8_t mem_base = 0;
  uint8_t mem_has_index = 0;
  uint8_t mem_index = 0;
  uint8_t mem_scale = 0;
  int32_t mem_disp = 0;
  bool is_mov_rm32 = tb_is_single_mov_rm32(tb, &mem_op, &mem_reg,
      &mem_has_base, &mem_base, &mem_has_index, &mem_index, &mem_scale, &mem_disp);
  if (!is_nop && !is_mov_i2r && !is_mov_r2r &&
      !is_arith_r2r && !is_logic_r2r && !is_unary_reg &&
      !is_compare_r2r && !is_moffs32 && !is_mov_rm32) {
    return;
  }

  uint8_t *code = jit_code_alloc(64);
  uint8_t *cursor = code;

  if (is_mov_i2r) {
    /* mov imm32 -> r32 只写通用寄存器，不影响 EFLAGS。 */
    emit_mov_rax_imm64(&cursor, (uint64_t)(uintptr_t)&cpu.gpr[reg]._32);
    emit_mov_m32_rax_imm32(&cursor, imm);
  }
  else if (is_mov_r2r) {
    /* 只支持 ModR/M mod=3 的寄存器复制，不碰访存路径。 */
    emit_mov_rax_imm64(&cursor, (uint64_t)(uintptr_t)&cpu.gpr[src]._32);
    emit_mov_ecx_m32_rax(&cursor);
    emit_mov_rax_imm64(&cursor, (uint64_t)(uintptr_t)&cpu.gpr[dest]._32);
    emit_mov_m32_rax_ecx(&cursor);
  }
  else if (is_arith_r2r) {
    /* add/sub 需要维护 EFLAGS，先回到 C helper 保证语义一致。 */
    uint32_t info = arith_src | (arith_dest << 3) | (arith_op << 6);
    emit_mov_edi_imm32(&cursor, info);
    emit_mov_esi_imm32(&cursor, tb->exit_eip);
    emit_call(&cursor, jit_helper_arith_r2r);
    emit_ret(&cursor);
    jit_code_make_executable();

    tb->host_code = code;
    tb->host_size = cursor - code;
    jit_state.stats.native_tbs ++;
    jit_state.stats.native_instr += tb->nr_instr;
    return;
  }
  else if (is_logic_r2r) {
    /* 逻辑运算通过 helper 写结果、更新 ZF/SF，并清零 CF/OF。 */
    uint32_t info = logic_src | (logic_dest << 3) | (logic_op << 6);
    emit_mov_edi_imm32(&cursor, info);
    emit_mov_esi_imm32(&cursor, tb->exit_eip);
    emit_call(&cursor, jit_helper_logic_r2r);
    emit_ret(&cursor);
    jit_code_make_executable();

    tb->host_code = code;
    tb->host_size = cursor - code;
    jit_state.stats.native_tbs ++;
    jit_state.stats.native_instr += tb->nr_instr;
    return;
  }
  else if (is_unary_reg) {
    uint32_t info = unary_reg | (unary_op << 3);
    emit_mov_edi_imm32(&cursor, info);
    emit_mov_esi_imm32(&cursor, tb->exit_eip);
    emit_call(&cursor, jit_helper_unary_reg);
    emit_ret(&cursor);
    jit_code_make_executable();

    tb->host_code = code;
    tb->host_size = cursor - code;
    jit_state.stats.native_tbs ++;
    jit_state.stats.native_instr += tb->nr_instr;
    return;
  }
  else if (is_compare_r2r) {
    /* cmp/test 不写寄存器，只通过 helper 更新标志位并推进 eip。 */
    uint32_t info = compare_src | (compare_dest << 3) | (compare_op << 6);
    emit_mov_edi_imm32(&cursor, info);
    emit_mov_esi_imm32(&cursor, tb->exit_eip);
    emit_call(&cursor, jit_helper_compare_r2r);
    emit_ret(&cursor);
    jit_code_make_executable();

    tb->host_code = code;
    tb->host_size = cursor - code;
    jit_state.stats.native_tbs ++;
    jit_state.stats.native_instr += tb->nr_instr;
    return;
  }
  else if (is_moffs32) {
    /* 直接地址 mov 通过 vaddr_read/write helper 保留分页和 MMIO 语义。 */
    emit_mov_edi_imm32(&cursor, moffs_addr);
    emit_mov_esi_imm32(&cursor, tb->exit_eip);
    emit_mov_edx_imm32(&cursor, moffs_op);
    emit_call(&cursor, jit_helper_moffs32);
    emit_ret(&cursor);
    jit_code_make_executable();

    tb->host_code = code;
    tb->host_size = cursor - code;
    jit_state.stats.native_tbs ++;
    jit_state.stats.native_instr += tb->nr_instr;
    return;
  }
  else if (is_mov_rm32) {
    /* ModR/M/SIB 只在 helper 中计算有效地址，真正访存仍走 vaddr_*。 */
    uint32_t info = mem_op | (mem_reg << 1) | (mem_base << 4) |
      (mem_has_base << 7) | (mem_index << 8) | (mem_has_index << 11) |
      (mem_scale << 12);
    emit_mov_edi_imm32(&cursor, info);
    emit_mov_esi_imm32(&cursor, mem_disp);
    emit_mov_edx_imm32(&cursor, tb->exit_eip);
    emit_call(&cursor, jit_helper_mov_rm32);
    emit_ret(&cursor);
    jit_code_make_executable();

    tb->host_code = code;
    tb->host_size = cursor - code;
    jit_state.stats.native_tbs ++;
    jit_state.stats.native_instr += tb->nr_instr;
    return;
  }

  /* 目前支持的 native 指令都不改变控制流，最后统一推进 eip。 */
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
