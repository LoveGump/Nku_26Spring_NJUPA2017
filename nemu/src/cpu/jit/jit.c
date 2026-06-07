#include "cpu/jit.h"

#ifdef CONFIG_JIT

#include <errno.h>
#include <linux/memfd.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#define JIT_GUEST_PAGE_SHIFT 12
#define JIT_GUEST_PAGE_COUNT (1u << (32 - JIT_GUEST_PAGE_SHIFT))
#define JIT_EFLAGS_LOGIC_MASK ((1u << 0) | (1u << 6) | (1u << 7) | (1u << 11))

typedef struct {
  bool enabled;
  TB *active_tb;
  bool ignoring_sealed_tb;
  vaddr_t ignore_until;
  uint8_t *code_cache;
  uint8_t *code_exec_cache;
  size_t code_capacity;
  size_t code_size;
  /* 记录每个 guest 4KB 页包含的有效 TB 数量，供写路径快速排除数据页。 */
  uint16_t code_page_refs[JIT_GUEST_PAGE_COUNT];
  JITStats stats;
} JITState;

static JITState jit_state;
/* 供 jit.h 中的内联快路径直接访问，减少 cpu_exec 主循环里的函数调用。 */
bool jit_cached_exec_active;
uint64_t jit_direct_instr_count;
TB jit_tb_cache[JIT_TB_CACHE_SIZE];
uint64_t jit_lookup_count;
uint64_t jit_hit_count;
uint64_t jit_miss_count;

static inline uint32_t tb_index(vaddr_t eip) {
  return (eip >> 2) & (JIT_TB_CACHE_SIZE - 1);
}

static inline size_t align_up(size_t value, size_t align) {
  return (value + align - 1) & ~(align - 1);
}

static void tb_update_code_page_refs(TB *tb, int delta) {
  if (tb == NULL || !tb->valid || !tb->sealed || tb->guest_end <= tb->guest_start) {
    return;
  }

  uint32_t first = tb->guest_start >> JIT_GUEST_PAGE_SHIFT;
  uint32_t last = (tb->guest_end - 1) >> JIT_GUEST_PAGE_SHIFT;
  for (uint32_t page = first; page <= last; page ++) {
    if (delta > 0) {
      Assert(jit_state.code_page_refs[page] != UINT16_MAX,
          "too many JIT TBs on guest page 0x%x", page);
      jit_state.code_page_refs[page] ++;
    }
    else {
      Assert(jit_state.code_page_refs[page] > 0,
          "JIT code page ref underflow on guest page 0x%x", page);
      jit_state.code_page_refs[page] --;
    }
  }
}

static bool jit_range_may_touch_code(vaddr_t addr, uint32_t len) {
  if (len == 0) {
    return false;
  }

  /*
   * 大多数 guest 写入落在栈或数据页。先按页引用表过滤，
   * 只有写到包含 TB 指令字节的页时，才扫描 TB cache 做精确判断。
   */
  uint32_t first = addr >> JIT_GUEST_PAGE_SHIFT;
  uint32_t last = (addr + len - 1) >> JIT_GUEST_PAGE_SHIFT;
  for (uint32_t page = first; page <= last; page ++) {
    if (jit_state.code_page_refs[page] != 0) {
      return true;
    }
  }

  return false;
}

static void jit_code_make_executable(void) {
  /*
   * x86 指令缓存与数据缓存保持一致；双映射下生成代码无需再调用 mprotect()。
   * 保留该接口，让各 codegen 分支的收尾逻辑保持统一。
   */
}

static void jit_code_init(void) {
  if (jit_state.code_cache != NULL) {
    return;
  }

  /*
   * 同一个 memfd 建立 RW 和 RX 两份映射：codegen 始终写 RW 地址，
   * native TB 始终执行 RX 地址，既遵守 W^X 又避免逐 TB mprotect()。
   */
  jit_state.code_capacity = JIT_CODE_CACHE_SIZE;
  int fd = syscall(SYS_memfd_create, "nemu-jit-code", MFD_CLOEXEC);
  Assert(fd >= 0, "memfd_create JIT code cache failed: %s", strerror(errno));
  int ret = ftruncate(fd, jit_state.code_capacity);
  Assert(ret == 0, "ftruncate JIT code cache failed: %s", strerror(errno));

  jit_state.code_cache = mmap(NULL, jit_state.code_capacity,
      PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  Assert(jit_state.code_cache != MAP_FAILED, "mmap JIT code cache failed: %s", strerror(errno));
  jit_state.code_exec_cache = mmap(NULL, jit_state.code_capacity,
      PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
  Assert(jit_state.code_exec_cache != MAP_FAILED,
      "mmap executable JIT code cache failed: %s", strerror(errno));
  close(fd);

  jit_state.code_size = 0;
}

static void jit_code_reset(void) {
  jit_state.code_size = 0;
  jit_state.stats.code_bytes = 0;
  jit_state.stats.code_flushes ++;
}

static void *jit_code_exec_ptr(uint8_t *write_ptr) {
  size_t offset = write_ptr - jit_state.code_cache;
  Assert(offset < jit_state.code_capacity, "invalid JIT code pointer");
  return jit_state.code_exec_cache + offset;
}

static uint8_t *jit_code_alloc(size_t size) {
  size_t aligned_size = align_up(size, 16);
  Assert(jit_state.code_size + aligned_size <= jit_state.code_capacity,
      "JIT code block is too large: %zu bytes", size);

  uint8_t *ptr = jit_state.code_cache + jit_state.code_size;
  jit_state.code_size += aligned_size;
  jit_state.stats.code_bytes = jit_state.code_size;
  return ptr;
}

static bool jit_code_prepare(size_t size) {
  size_t aligned_size = align_up(size, 16);
  if (jit_state.code_size + aligned_size <= jit_state.code_capacity) {
    return true;
  }

  /*
   * code cache 满时会同时清空所有 TB。调用方必须放弃当前 TB，
   * 不能继续使用已经被 tb_invalidate() 清零的指针。
   */
  jit_invalidate_all();
  jit_code_reset();
  return false;
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

static void emit_mov_edx_m32_rax(uint8_t **cursor) {
  emit_u8(cursor, 0x8b);
  emit_u8(cursor, 0x10);
}

static void emit_mov_m32_rax_ecx(uint8_t **cursor) {
  emit_u8(cursor, 0x89);
  emit_u8(cursor, 0x08);
}

static void emit_logic_ecx_edx(uint8_t **cursor, uint8_t op) {
  /*
   * 生成 ecx <- ecx op edx。x86 host 的 32 位 OR/AND/XOR 会按同样规则
   * 设置 ZF/SF 并清零 CF/OF，因此可以直接捕获这些 flags 写回 guest。
   */
  if (op == 0) {
    emit_u8(cursor, 0x09);
  }
  else if (op == 1) {
    emit_u8(cursor, 0x21);
  }
  else {
    emit_u8(cursor, 0x31);
  }
  emit_u8(cursor, 0xd1);
}

static void emit_pushfq_pop_rdx(uint8_t **cursor) {
  emit_u8(cursor, 0x9c);
  emit_u8(cursor, 0x5a);
}

static void emit_and_edx_imm32(uint8_t **cursor, uint32_t value) {
  emit_u8(cursor, 0x81);
  emit_u8(cursor, 0xe2);
  emit_u32(cursor, value);
}

static void emit_and_ecx_imm32(uint8_t **cursor, uint32_t value) {
  emit_u8(cursor, 0x81);
  emit_u8(cursor, 0xe1);
  emit_u32(cursor, value);
}

static void emit_or_ecx_edx(uint8_t **cursor) {
  emit_u8(cursor, 0x09);
  emit_u8(cursor, 0xd1);
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

static void emit_mov_ecx_imm32(uint8_t **cursor, uint32_t value) {
  emit_u8(cursor, 0xb9);
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
  JIT_GP1_ADD = 0,
  JIT_GP1_OR = 1,
  JIT_GP1_AND = 4,
  JIT_GP1_SUB = 5,
  JIT_GP1_XOR = 6,
  JIT_GP1_CMP = 7,
};

enum {
  JIT_MEM_MOFFS_READ = 0,
  JIT_MEM_MOFFS_WRITE = 1,
};

enum {
  JIT_MEM_RM_READ = 0,
  JIT_MEM_RM_WRITE = 1,
};

enum {
  JIT_CALL_DIRECT = 0,
  JIT_RET_NEAR = 1,
  JIT_RET_NEAR_IMM = 2,
};

static uint32_t jit_calc_rm_addr(uint32_t info, uint32_t disp);

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

static int jit_helper_gp1_imm8_reg(uint32_t info, uint32_t imm, uint32_t exit_eip) {
  /* 0x83 group1: bit[2:0]=register，bit[5:3]=子操作，imm 已符号扩展到 32 位。 */
  uint8_t reg = info & 0x7;
  uint8_t op = (info >> 3) & 0x7;
  uint32_t dest_val = cpu.gpr[reg]._32;
  uint32_t result = 0;
  bool write_back = op != JIT_GP1_CMP;

  if (op == JIT_GP1_ADD) {
    result = dest_val + imm;
    cpu.CF = result < dest_val;
    cpu.OF = ((~(dest_val ^ imm) & (dest_val ^ result)) >> 31) & 0x1;
  }
  else if (op == JIT_GP1_SUB || op == JIT_GP1_CMP) {
    result = dest_val - imm;
    cpu.CF = dest_val < imm;
    cpu.OF = (((dest_val ^ imm) & (dest_val ^ result)) >> 31) & 0x1;
  }
  else {
    if (op == JIT_GP1_OR) {
      result = dest_val | imm;
    }
    else if (op == JIT_GP1_AND) {
      result = dest_val & imm;
    }
    else {
      result = dest_val ^ imm;
    }
    cpu.CF = 0;
    cpu.OF = 0;
  }

  if (write_back) {
    cpu.gpr[reg]._32 = result;
  }
  cpu.ZF = result == 0;
  cpu.SF = (result >> 31) & 0x1;
  cpu.eip = exit_eip;
  return JIT_EXEC_OK;
}

static int jit_helper_gp1_imm8_rm32(uint32_t info, uint32_t disp,
    uint32_t imm, uint32_t exit_eip) {
  /*
   * 0x83 访存形态复用地址编码。bit[3:1] 在 mov 中表示寄存器，
   * 这里复用为 group1 子操作，地址计算不会读取这三位。
   */
  uint8_t op = (info >> 1) & 0x7;
  uint32_t addr = jit_calc_rm_addr(info, disp);
  uint32_t dest_val = vaddr_read(addr, 4);
  uint32_t result = 0;

  if (op == JIT_GP1_ADD) {
    result = dest_val + imm;
    cpu.CF = result < dest_val;
    cpu.OF = ((~(dest_val ^ imm) & (dest_val ^ result)) >> 31) & 0x1;
  }
  else if (op == JIT_GP1_SUB || op == JIT_GP1_CMP) {
    result = dest_val - imm;
    cpu.CF = dest_val < imm;
    cpu.OF = (((dest_val ^ imm) & (dest_val ^ result)) >> 31) & 0x1;
  }
  else {
    if (op == JIT_GP1_OR) {
      result = dest_val | imm;
    }
    else if (op == JIT_GP1_AND) {
      result = dest_val & imm;
    }
    else {
      result = dest_val ^ imm;
    }
    cpu.CF = 0;
    cpu.OF = 0;
  }

  if (op != JIT_GP1_CMP) {
    vaddr_write(addr, 4, result);
  }
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

static uint32_t jit_calc_rm_addr(uint32_t info, uint32_t disp) {
  /*
   * info:
   * bit[0]=read/write，bit[3:1]=reg，bit[6:4]=base，bit[7]=has_base，
   * bit[10:8]=index，bit[11]=has_index，bit[13:12]=scale。
   */
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

  return addr;
}

static int jit_helper_mov_rm32(uint32_t info, uint32_t disp, uint32_t exit_eip) {
  uint8_t op = info & 0x1;
  uint8_t reg = (info >> 1) & 0x7;
  uint32_t addr = jit_calc_rm_addr(info, disp);

  if (op == JIT_MEM_RM_READ) {
    cpu.gpr[reg]._32 = vaddr_read(addr, 4);
  }
  else {
    vaddr_write(addr, 4, cpu.gpr[reg]._32);
  }

  cpu.eip = exit_eip;
  return JIT_EXEC_OK;
}

static int jit_helper_lea_m2g(uint32_t info, uint32_t disp, uint32_t exit_eip) {
  /* lea 只计算有效地址并写入目标寄存器，不读取内存，也不修改 EFLAGS。 */
  uint8_t reg = (info >> 1) & 0x7;
  cpu.gpr[reg]._32 = jit_calc_rm_addr(info, disp);
  cpu.eip = exit_eip;
  return JIT_EXEC_OK;
}

static bool jit_eval_cc(uint8_t subcode) {
  /* subcode 与 x86 条件跳转低 4 位一致，最低位表示取反。 */
  bool invert = subcode & 0x1;
  bool taken = false;

  switch (subcode & 0xe) {
    case 0x0: taken = cpu.OF; break;
    case 0x2: taken = cpu.CF; break;
    case 0x4: taken = cpu.ZF; break;
    case 0x6: taken = cpu.CF || cpu.ZF; break;
    case 0x8: taken = cpu.SF; break;
    case 0xc: taken = cpu.SF != cpu.OF; break;
    case 0xe: taken = (cpu.SF != cpu.OF) || cpu.ZF; break;
    default:
      /* 当前 NEMU 没有 PF，PF/NP 条件保持回退解释器，不进入这里。 */
      assert(0);
  }

  return invert ? !taken : taken;
}

static int jit_helper_jcc(uint32_t subcode, uint32_t target, uint32_t fallthrough) {
  /* 条件跳转必须每次根据当前 EFLAGS 重新判断，不能固定使用 trace 出口。 */
  cpu.eip = jit_eval_cc(subcode) ? target : fallthrough;
  return JIT_EXEC_OK;
}

static int jit_helper_call_ret(uint32_t op, uint32_t target, uint32_t fallthrough) {
  if (op == JIT_CALL_DIRECT) {
    /* call rel32 压入返回地址，再跳到直接目标。 */
    cpu.esp -= 4;
    vaddr_write(cpu.esp, 4, fallthrough);
    cpu.eip = target;
  }
  else {
    /* ret 的目标由运行时栈内容决定，不能在翻译阶段固定。 */
    cpu.eip = vaddr_read(cpu.esp, 4);
    cpu.esp += 4;
    if (op == JIT_RET_NEAR_IMM) {
      /* ret imm16 再额外弹出调用参数；target 参数复用为清栈字节数。 */
      cpu.esp += target;
    }
  }

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

static bool tb_is_single_gp1_imm8_reg(TB *tb, uint8_t *op, uint8_t *reg, uint32_t *imm) {
  /* 0x83 /digit ib: 32 位 r/m 操作数配符号扩展 imm8；这里先只支持寄存器形式。 */
  if (tb->nr_instr != 1 || tb->guest_end != tb->guest_start + 3) {
    return false;
  }

  uint8_t opcode = vaddr_read(tb->guest_start, 1);
  if (opcode != 0x83) {
    return false;
  }

  uint8_t modrm = vaddr_read(tb->guest_start + 1, 1);
  if ((modrm >> 6) != 3) {
    return false;
  }

  uint8_t subop = (modrm >> 3) & 0x7;
  if (subop != JIT_GP1_ADD && subop != JIT_GP1_OR &&
      subop != JIT_GP1_AND && subop != JIT_GP1_SUB &&
      subop != JIT_GP1_XOR && subop != JIT_GP1_CMP) {
    return false;
  }

  *op = subop;
  *reg = modrm & 0x7;
  *imm = (uint32_t)(int32_t)(int8_t)vaddr_read(tb->guest_start + 2, 1);
  return true;
}

static bool tb_is_single_gp1_imm8_rm32(TB *tb, uint8_t *op,
    uint8_t *has_base, uint8_t *base, uint8_t *has_index,
    uint8_t *index, uint8_t *scale, int32_t *disp, uint32_t *imm) {
  /* 0x83 的内存形式，先只支持 32 位 r/m32 和 imm8。 */
  if (tb->nr_instr != 1 || tb->guest_end < tb->guest_start + 3) {
    return false;
  }

  uint8_t opcode = vaddr_read(tb->guest_start, 1);
  if (opcode != 0x83) {
    return false;
  }

  uint8_t modrm = vaddr_read(tb->guest_start + 1, 1);
  uint8_t mod = modrm >> 6;
  uint8_t rm = modrm & 0x7;
  if (mod == 3) {
    return false;
  }

  uint8_t subop = (modrm >> 3) & 0x7;
  if (subop != JIT_GP1_ADD && subop != JIT_GP1_OR &&
      subop != JIT_GP1_AND && subop != JIT_GP1_SUB &&
      subop != JIT_GP1_XOR && subop != JIT_GP1_CMP) {
    return false;
  }

  *op = subop;
  *base = rm;
  *has_base = true;
  *index = 0;
  *has_index = false;
  *scale = 0;
  *disp = 0;
  uint32_t len = 2;

  if (rm == R_ESP) {
    if (tb->guest_end < tb->guest_start + 4) {
      return false;
    }

    uint8_t sib = vaddr_read(tb->guest_start + 2, 1);
    *scale = sib >> 6;
    *index = (sib >> 3) & 0x7;
    *base = sib & 0x7;
    *has_index = *index != R_ESP;
    *has_base = !(mod == 0 && *base == R_EBP);
    len = 3;

    if (mod == 0 && *base == R_EBP) {
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
      *has_base = false;
      *disp = vaddr_read(tb->guest_start + 2, 4);
      len = 6;
    }
    else if (mod == 1) {
      *disp = (int8_t)vaddr_read(tb->guest_start + 2, 1);
      len = 3;
    }
    else if (mod == 2) {
      *disp = vaddr_read(tb->guest_start + 2, 4);
      len = 6;
    }
  }

  if (tb->guest_end != tb->guest_start + len + 1) {
    return false;
  }

  *imm = (uint32_t)(int32_t)(int8_t)vaddr_read(tb->guest_start + len, 1);
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

static bool tb_is_single_lea_m2g(TB *tb, uint8_t *reg,
    uint8_t *has_base, uint8_t *base, uint8_t *has_index,
    uint8_t *index, uint8_t *scale, int32_t *disp) {
  /* lea 使用 ModR/M 的内存地址编码，但结果是地址本身，而不是地址处的内存。 */
  if (tb->nr_instr != 1 || tb->guest_end < tb->guest_start + 2) {
    return false;
  }

  uint8_t opcode = vaddr_read(tb->guest_start, 1);
  if (opcode != 0x8d) {
    return false;
  }

  uint8_t modrm = vaddr_read(tb->guest_start + 1, 1);
  uint8_t mod = modrm >> 6;
  uint8_t rm = modrm & 0x7;
  if (mod == 3) {
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
    if (tb->guest_end < tb->guest_start + 3) {
      return false;
    }

    uint8_t sib = vaddr_read(tb->guest_start + 2, 1);
    *scale = sib >> 6;
    *index = (sib >> 3) & 0x7;
    *base = sib & 0x7;
    *has_index = *index != R_ESP;
    *has_base = !(mod == 0 && *base == R_EBP);
    len = 3;

    if (mod == 0 && *base == R_EBP) {
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
      *has_base = false;
      *disp = vaddr_read(tb->guest_start + 2, 4);
      len = 6;
    }
    else if (mod == 1) {
      *disp = (int8_t)vaddr_read(tb->guest_start + 2, 1);
      len = 3;
    }
    else if (mod == 2) {
      *disp = vaddr_read(tb->guest_start + 2, 4);
      len = 6;
    }
  }

  return tb->guest_end == tb->guest_start + len;
}

static bool tb_is_single_direct_jmp(TB *tb) {
  /* 这里只处理编码里直接带相对位移的无条件跳转；jmp r/m32 仍回退解释器。 */
  if (tb->nr_instr != 1) {
    return false;
  }

  uint8_t opcode = vaddr_read(tb->guest_start, 1);
  if (opcode == 0xe9) {
    /* jmp rel32: 1 字节 opcode + 4 字节有符号位移，目标已记录在 exit_eip。 */
    return tb->guest_end == tb->guest_start + 5;
  }
  if (opcode == 0xeb) {
    /* jmp rel8: 短跳转，同样复用 trace 记录到的真实出口地址。 */
    return tb->guest_end == tb->guest_start + 2;
  }

  return false;
}

static bool tb_is_single_jcc(TB *tb, uint8_t *subcode,
    uint32_t *target, uint32_t *fallthrough) {
  if (tb->nr_instr != 1) {
    return false;
  }

  uint8_t opcode = vaddr_read(tb->guest_start, 1);
  if (opcode >= 0x70 && opcode <= 0x7f) {
    *subcode = opcode & 0xf;
    if ((*subcode & 0xe) == 0xa) {
      /* PF/NP 依赖当前 CPU_state 没有实现的 PF，保持解释器行为。 */
      return false;
    }
    if (tb->guest_end != tb->guest_start + 2) {
      return false;
    }

    /* rel8 是相对下一条指令的有符号位移。 */
    *fallthrough = tb->guest_start + 2;
    *target = *fallthrough + (int8_t)vaddr_read(tb->guest_start + 1, 1);
    return true;
  }

  if (opcode == 0x0f && tb->guest_end >= tb->guest_start + 2) {
    uint8_t opcode2 = vaddr_read(tb->guest_start + 1, 1);
    if (opcode2 < 0x80 || opcode2 > 0x8f) {
      return false;
    }

    *subcode = opcode2 & 0xf;
    if ((*subcode & 0xe) == 0xa) {
      return false;
    }
    if (tb->guest_end != tb->guest_start + 6) {
      return false;
    }

    /* 0f 8x 使用 rel32，同样相对 fallthrough 地址。 */
    *fallthrough = tb->guest_start + 6;
    *target = *fallthrough + (int32_t)vaddr_read(tb->guest_start + 2, 4);
    return true;
  }

  return false;
}

static bool tb_is_single_call_ret(TB *tb, uint8_t *op,
    uint32_t *target, uint32_t *fallthrough) {
  if (tb->nr_instr != 1) {
    return false;
  }

  uint8_t opcode = vaddr_read(tb->guest_start, 1);
  if (opcode == 0xe8) {
    if (tb->guest_end != tb->guest_start + 5) {
      return false;
    }

    *op = JIT_CALL_DIRECT;
    /* rel32 和 jmp 一样，相对下一条指令地址计算目标。 */
    *fallthrough = tb->guest_start + 5;
    *target = *fallthrough + (int32_t)vaddr_read(tb->guest_start + 1, 4);
    return true;
  }

  if (opcode == 0xc3) {
    /* ret 只有 opcode 自身，真实返回地址留到 helper 执行时从栈中读取。 */
    *op = JIT_RET_NEAR;
    *fallthrough = 0;
    *target = 0;
    return tb->guest_end == tb->guest_start + 1;
  }

  if (opcode == 0xc2) {
    if (tb->guest_end != tb->guest_start + 3) {
      return false;
    }

    /* ret imm16 的立即数表示弹出返回地址后还要额外清理的参数字节数。 */
    *op = JIT_RET_NEAR_IMM;
    *target = vaddr_read(tb->guest_start + 1, 2);
    *fallthrough = 0;
    return true;
  }

  return false;
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
  uint8_t gp1_op = 0;
  uint8_t gp1_reg = 0;
  uint32_t gp1_imm = 0;
  bool is_gp1_imm8_reg = tb_is_single_gp1_imm8_reg(tb, &gp1_op, &gp1_reg, &gp1_imm);
  uint8_t gp1_rm_op = 0;
  uint8_t gp1_rm_has_base = 0;
  uint8_t gp1_rm_base = 0;
  uint8_t gp1_rm_has_index = 0;
  uint8_t gp1_rm_index = 0;
  uint8_t gp1_rm_scale = 0;
  int32_t gp1_rm_disp = 0;
  uint32_t gp1_rm_imm = 0;
  bool is_gp1_imm8_rm32 = tb_is_single_gp1_imm8_rm32(tb, &gp1_rm_op,
      &gp1_rm_has_base, &gp1_rm_base, &gp1_rm_has_index, &gp1_rm_index,
      &gp1_rm_scale, &gp1_rm_disp, &gp1_rm_imm);
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
  uint8_t lea_reg = 0;
  uint8_t lea_has_base = 0;
  uint8_t lea_base = 0;
  uint8_t lea_has_index = 0;
  uint8_t lea_index = 0;
  uint8_t lea_scale = 0;
  int32_t lea_disp = 0;
  bool is_lea_m2g = tb_is_single_lea_m2g(tb, &lea_reg,
      &lea_has_base, &lea_base, &lea_has_index, &lea_index, &lea_scale, &lea_disp);
  bool is_direct_jmp = tb_is_single_direct_jmp(tb);
  uint8_t jcc_subcode = 0;
  uint32_t jcc_target = 0;
  uint32_t jcc_fallthrough = 0;
  bool is_jcc = tb_is_single_jcc(tb, &jcc_subcode, &jcc_target, &jcc_fallthrough);
  uint8_t call_op = 0;
  uint32_t call_target = 0;
  uint32_t call_fallthrough = 0;
  bool is_call_ret = tb_is_single_call_ret(tb, &call_op, &call_target, &call_fallthrough);
  if (!is_nop && !is_mov_i2r && !is_mov_r2r &&
      !is_arith_r2r && !is_logic_r2r && !is_unary_reg &&
      !is_compare_r2r && !is_gp1_imm8_reg && !is_moffs32 && !is_mov_rm32 &&
      !is_gp1_imm8_rm32 && !is_lea_m2g && !is_direct_jmp && !is_jcc && !is_call_ret) {
    return;
  }

  size_t native_size = is_logic_r2r ? 128 : 64;
  if (!jit_code_prepare(native_size)) {
    return;
  }

  uint8_t *code = jit_code_alloc(native_size);
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

    tb->host_code = jit_code_exec_ptr(code);
    tb->host_size = cursor - code;
    jit_state.stats.native_tbs ++;
    jit_state.stats.native_instr += tb->nr_instr;
    return;
  }
  else if (is_logic_r2r) {
    /*
     * 逻辑寄存器指令的语义很适合直接生成 host code:
     * 32 位 OR/AND/XOR 本身会按 x86 规则更新 ZF/SF，并清零 CF/OF。
     * 这里用 pushfq 抓取 host flags，再只合并 guest 关心的四个标志位。
     */
    emit_mov_rax_imm64(&cursor, (uint64_t)(uintptr_t)&cpu.gpr[logic_dest]._32);
    emit_mov_ecx_m32_rax(&cursor);
    emit_mov_rax_imm64(&cursor, (uint64_t)(uintptr_t)&cpu.gpr[logic_src]._32);
    emit_mov_edx_m32_rax(&cursor);
    emit_logic_ecx_edx(&cursor, logic_op);
    emit_pushfq_pop_rdx(&cursor);
    emit_mov_rax_imm64(&cursor, (uint64_t)(uintptr_t)&cpu.gpr[logic_dest]._32);
    emit_mov_m32_rax_ecx(&cursor);
    emit_and_edx_imm32(&cursor, JIT_EFLAGS_LOGIC_MASK);
    emit_mov_rax_imm64(&cursor, (uint64_t)(uintptr_t)&cpu.eflags);
    emit_mov_ecx_m32_rax(&cursor);
    emit_and_ecx_imm32(&cursor, ~JIT_EFLAGS_LOGIC_MASK);
    emit_or_ecx_edx(&cursor);
    emit_mov_m32_rax_ecx(&cursor);
  }
  else if (is_unary_reg) {
    uint32_t info = unary_reg | (unary_op << 3);
    emit_mov_edi_imm32(&cursor, info);
    emit_mov_esi_imm32(&cursor, tb->exit_eip);
    emit_call(&cursor, jit_helper_unary_reg);
    emit_ret(&cursor);
    jit_code_make_executable();

    tb->host_code = jit_code_exec_ptr(code);
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

    tb->host_code = jit_code_exec_ptr(code);
    tb->host_size = cursor - code;
    jit_state.stats.native_tbs ++;
    jit_state.stats.native_instr += tb->nr_instr;
    return;
  }
  else if (is_gp1_imm8_reg) {
    /* 0x83 的 imm8 由 helper 使用 32 位符号扩展值参与运算。 */
    uint32_t info = gp1_reg | (gp1_op << 3);
    emit_mov_edi_imm32(&cursor, info);
    emit_mov_esi_imm32(&cursor, gp1_imm);
    emit_mov_edx_imm32(&cursor, tb->exit_eip);
    emit_call(&cursor, jit_helper_gp1_imm8_reg);
    emit_ret(&cursor);
    jit_code_make_executable();

    tb->host_code = jit_code_exec_ptr(code);
    tb->host_size = cursor - code;
    jit_state.stats.native_tbs ++;
    jit_state.stats.native_instr += tb->nr_instr;
    return;
  }
  else if (is_gp1_imm8_rm32) {
    uint32_t info = (gp1_rm_op << 1) | (gp1_rm_base << 4) |
      (gp1_rm_has_base << 7) | (gp1_rm_index << 8) | (gp1_rm_has_index << 11) |
      (gp1_rm_scale << 12);
    emit_mov_edi_imm32(&cursor, info);
    emit_mov_esi_imm32(&cursor, gp1_rm_disp);
    emit_mov_edx_imm32(&cursor, gp1_rm_imm);
    emit_mov_ecx_imm32(&cursor, tb->exit_eip);
    emit_call(&cursor, jit_helper_gp1_imm8_rm32);
    emit_ret(&cursor);
    jit_code_make_executable();

    tb->host_code = jit_code_exec_ptr(code);
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

    tb->host_code = jit_code_exec_ptr(code);
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

    tb->host_code = jit_code_exec_ptr(code);
    tb->host_size = cursor - code;
    jit_state.stats.native_tbs ++;
    jit_state.stats.native_instr += tb->nr_instr;
    return;
  }
  else if (is_lea_m2g) {
    uint32_t info = JIT_MEM_RM_READ | (lea_reg << 1) | (lea_base << 4) |
      (lea_has_base << 7) | (lea_index << 8) | (lea_has_index << 11) |
      (lea_scale << 12);
    emit_mov_edi_imm32(&cursor, info);
    emit_mov_esi_imm32(&cursor, lea_disp);
    emit_mov_edx_imm32(&cursor, tb->exit_eip);
    emit_call(&cursor, jit_helper_lea_m2g);
    emit_ret(&cursor);
    jit_code_make_executable();

    tb->host_code = jit_code_exec_ptr(code);
    tb->host_size = cursor - code;
    jit_state.stats.native_tbs ++;
    jit_state.stats.native_instr += tb->nr_instr;
    return;
  }
  else if (is_direct_jmp) {
    /* 直接跳转的目标来自解释器 trace，避免在 JIT 中重复实现符号位移计算。 */
    emit_mov_rax_imm64(&cursor, (uint64_t)(uintptr_t)&cpu.eip);
    emit_mov_m32_rax_imm32(&cursor, tb->exit_eip);
    emit_return_status(&cursor, JIT_EXEC_OK);
    jit_code_make_executable();

    tb->host_code = jit_code_exec_ptr(code);
    tb->host_size = cursor - code;
    jit_state.stats.native_tbs ++;
    jit_state.stats.native_instr += tb->nr_instr;
    return;
  }
  else if (is_jcc) {
    /* 条件跳转通过 helper 读取 guest EFLAGS，选择 taken 或 fallthrough。 */
    emit_mov_edi_imm32(&cursor, jcc_subcode);
    emit_mov_esi_imm32(&cursor, jcc_target);
    emit_mov_edx_imm32(&cursor, jcc_fallthrough);
    emit_call(&cursor, jit_helper_jcc);
    emit_ret(&cursor);
    jit_code_make_executable();

    tb->host_code = jit_code_exec_ptr(code);
    tb->host_size = cursor - code;
    jit_state.stats.native_tbs ++;
    jit_state.stats.native_instr += tb->nr_instr;
    return;
  }
  else if (is_call_ret) {
    /* call/ret 通过 helper 维护 guest 栈，避免绕过 vaddr_read/write。 */
    emit_mov_edi_imm32(&cursor, call_op);
    emit_mov_esi_imm32(&cursor, call_target);
    emit_mov_edx_imm32(&cursor, call_fallthrough);
    emit_call(&cursor, jit_helper_call_ret);
    emit_ret(&cursor);
    jit_code_make_executable();

    tb->host_code = jit_code_exec_ptr(code);
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

  tb->host_code = jit_code_exec_ptr(code);
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
  tb_update_code_page_refs(tb, 1);
  if (jit_state.active_tb == tb) {
    jit_state.active_tb = NULL;
  }
}

void jit_init(void) {
  memset(&jit_state, 0, sizeof(jit_state));
  memset(jit_tb_cache, 0, sizeof(jit_tb_cache));
  jit_cached_exec_active = false;
  jit_direct_instr_count = 0;
  jit_lookup_count = 0;
  jit_hit_count = 0;
  jit_miss_count = 0;
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
  jit_cached_exec_active = false;
  jit_invalidate_all();
  jit_code_reset();
}

void jit_invalidate_all(void) {
  jit_cached_exec_active = false;
  jit_state.active_tb = NULL;
  jit_state.ignoring_sealed_tb = false;
  jit_state.ignore_until = 0;

  for (int i = 0; i < JIT_TB_CACHE_SIZE; i ++) {
    tb_invalidate(&jit_tb_cache[i]);
  }
  memset(jit_state.code_page_refs, 0, sizeof(jit_state.code_page_refs));
  jit_state.stats.invalidations ++;
}

void jit_invalidate_range(vaddr_t addr, uint32_t len) {
  if (len == 0) {
    return;
  }

  if (!jit_range_may_touch_code(addr, len)) {
    return;
  }

  /*
   * 页引用表只负责快速过滤；命中代码页后仍按精确字节范围判断，
   * 避免同页中的普通数据写入误伤不重叠的 TB。
   */
  vaddr_t end = addr + len;
  bool invalidated = false;
  for (int i = 0; i < JIT_TB_CACHE_SIZE; i ++) {
    TB *tb = &jit_tb_cache[i];
    if (!tb->valid) {
      continue;
    }

    if (addr < tb->guest_end && end > tb->guest_start) {
      if (jit_state.active_tb == tb) {
        jit_state.active_tb = NULL;
      }
      tb_invalidate(tb);
      invalidated = true;
    }
  }

  if (invalidated) {
    jit_state.ignoring_sealed_tb = false;
    jit_state.ignore_until = 0;
    jit_state.stats.invalidations ++;
  }
}

TB *tb_lookup(vaddr_t eip) {
  jit_lookup_count ++;

  TB *tb = &jit_tb_cache[tb_index(eip)];
  if (tb->valid && tb->guest_start == eip) {
    tb->hit_count ++;
    jit_hit_count ++;
    return tb;
  }

  jit_miss_count ++;
  return NULL;
}

TB *tb_alloc(vaddr_t eip) {
  TB *tb = &jit_tb_cache[tb_index(eip)];
  tb_invalidate(tb);

  tb->valid = true;
  tb->sealed = false;
  tb->compile_attempted = false;
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
    tb_update_code_page_refs(tb, -1);
  }
  memset(tb, 0, sizeof(*tb));
}

const JITStats *jit_get_stats(void) {
  /* 高频计数在 header 中内联累加，查询时再同步到统计快照。 */
  jit_state.stats.direct_instr = jit_direct_instr_count;
  jit_state.stats.lookups = jit_lookup_count;
  jit_state.stats.hits = jit_hit_count;
  jit_state.stats.misses = jit_miss_count;
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
  return (jit_func_t)jit_code_exec_ptr(code);
}

int jit_code_self_test(void) {
  uint32_t host_size = 0;
  jit_func_t fn = jit_emit_return(JIT_EXEC_OK, &host_size);
  Assert(fn != NULL && host_size == 6, "unexpected JIT self-test code size: %u", host_size);
  /* 直接调用刚生成的 host code，确认 RW/RX 双映射和 codegen 均可用。 */
  int ret = fn();
  jit_state.stats.code_self_tests ++;
  return ret;
}

TB *jit_prepare_hot_tb(TB *tb, vaddr_t eip) {
  /*
   * 只为真正重复执行的热点 TB 生成 native code，避免一次性代码消耗
   * 编译时间和 code cache；不支持翻译的 TB 也只尝试编译一次。
   */
  if (!tb->compile_attempted && tb->hit_count >= JIT_COMPILE_HIT_THRESHOLD) {
    tb->compile_attempted = true;
    jit_state.stats.compile_attempts ++;
    jit_compile_tb(tb);
    /* 编译时 code cache 可能刷新，当前 TB 此时已失效。 */
    if (!tb->valid || !tb->sealed || tb->guest_start != eip) {
      return NULL;
    }
    if (tb->host_code == NULL) {
      jit_state.stats.compile_unsupported ++;
      jit_state.stats.unsupported_opcode[vaddr_read(tb->guest_start, 1)] ++;
      uint32_t nr_instr = tb->nr_instr <= JIT_MAX_TB_INSTR ? tb->nr_instr : JIT_MAX_TB_INSTR;
      jit_state.stats.unsupported_instr_count[nr_instr] ++;
    }
  }

  return tb;
}

int jit_exec_native(TB *tb) {
  /* 调用方已验证 host_code；helper 可能失效当前 TB，因此先保存动态指令数。 */
  uint32_t nr_instr = tb->nr_instr;
  jit_func_t fn = (jit_func_t)tb->host_code;
  jit_state.stats.native_calls ++;
  int ret = fn();
  if (ret == JIT_EXEC_OK) {
    jit_state.stats.native_executed_instr += nr_instr;
  }
  else if (ret == JIT_EXEC_FALLBACK) {
    jit_state.stats.native_fallbacks ++;
  }
  return ret;
}

void jit_begin_tb_exec(TB *tb) {
  if (tb == NULL || !tb->valid || !tb->sealed) {
    return;
  }

  jit_state.ignoring_sealed_tb = true;
  jit_state.ignore_until = tb->guest_end;
  jit_cached_exec_active = true;
  jit_state.stats.executed_tbs ++;
}

void jit_end_tb_exec(uint32_t nr_instr, bool aborted) {
  jit_cached_exec_active = false;
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
