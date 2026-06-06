#include "nemu.h"
#include "cpu/decode.h"
#include "cpu/jit.h"
#include "monitor/monitor.h"
#include "monitor/watchpoint.h"

/* The assembly code of instructions executed is only output to the screen
 * when the number of instructions executed is less than this value.
 * This is useful when you use the `si' command.
 * You can modify this value as you want.
 */
#define MAX_INSTR_TO_PRINT 10
#define TIMER_IRQ 32

int nemu_state = NEMU_STOP;

void exec_wrapper(bool);
void raise_intr(uint8_t NO, vaddr_t ret_addr);

#ifdef CONFIG_JIT
static void handle_native_interrupt(void) {
  if (!cpu.INTR || !cpu.IF) {
    return;
  }

  /*
   * 与 exec_wrapper() 保持相同顺序：先完成当前 guest 指令，再响应中断。
   * 这里只处理进入本条指令前已挂起的请求；随后 device_update() 新产生的请求
   * 留到下一条 guest 指令结束后处理。
   * raise_intr() 通过 decoding.jmp_eip 返回 IDT 目标，这里直接完成 eip 切换。
   */
  cpu.INTR = false;
  raise_intr(TIMER_IRQ, cpu.eip);
  cpu.eip = decoding.jmp_eip;
  decoding.is_jmp = false;
}
#endif

static bool finish_one_instr(void) {
#ifdef DEBUG
  /* TODO(finished): check watchpoints here. */
  if (check_watchpoints()) {
    nemu_state = NEMU_STOP;
    return false;
  }
#endif

#ifdef HAS_IOE
  extern void device_update();
  device_update();
#endif

  return nemu_state == NEMU_RUNNING;
}

static bool exec_one_instr(bool print_flag) {
  /* Execute one instruction, including instruction fetch,
   * instruction decode, and the actual execution. */
  exec_wrapper(print_flag);
  return finish_one_instr();
}

#ifdef CONFIG_JIT
static uint64_t exec_native_tb(TB *tb, uint64_t n, bool print_flag) {
#ifdef DIFF_TEST
  /*
   * difftest 需要逐条同步，native TB 会跨过 exec_wrapper() 中的 difftest_step()；
   * cached TB 仍逐条解释执行，因此只禁用 native 路径。
   */
  return 0;
#endif

  if (print_flag || tb->nr_instr > n || !jit_tb_has_native(tb)) {
    return 0;
  }

  /* native helper 可能写内存并触发 TB 全量失效，先保存执行条数再调用。 */
  uint32_t nr_instr = tb->nr_instr;
  int ret = jit_exec_native(tb);
  if (ret != JIT_EXEC_OK) {
    return 0;
  }

  handle_native_interrupt();
  /* native TB 仍然按客户指令粒度保留 watchpoint 和设备更新语义。 */
  finish_one_instr();
  /* 控制流指令可能根据 EFLAGS 选择不同出口，eip 由 native code 自己写回。 */
  return nr_instr;
}

static uint64_t exec_cached_tb(TB *tb, uint64_t n, bool print_flag) {
  assert(tb != NULL && tb->valid && tb->sealed);

  /* 解释执行 TB 时也可能遇到 cr0/cr3 或内存写导致 TB 被清空，先做快照。 */
  uint32_t nr_instr = tb->nr_instr;
  vaddr_t guest_start = tb->guest_start;
  vaddr_t guest_end = tb->guest_end;
  vaddr_t exit_eip = tb->exit_eip;
  uint64_t limit = nr_instr < n ? nr_instr : n;
  uint64_t executed = 0;
  bool aborted = false;

  jit_begin_tb_exec(tb);

  for (; executed < limit; executed ++) {
    bool running = exec_one_instr(print_flag);
    if (!running) {
      executed ++;
      break;
    }

    bool reached_end = cpu.eip == exit_eip;
    bool outside_tb = cpu.eip < guest_start || cpu.eip > guest_end;

    if (executed + 1 < nr_instr && (outside_tb || reached_end)) {
      aborted = true;
      executed ++;
      break;
    }
  }

  if (executed == nr_instr && cpu.eip != exit_eip) {
    aborted = true;
  }

  jit_end_tb_exec(executed, aborted);
  return executed;
}
#endif

/* Simulate how the CPU works. */
void cpu_exec(uint64_t n) {
  if (nemu_state == NEMU_END) {
    printf("Program execution has ended. To restart the program, exit NEMU and run again.\n");
    return;
  }
  nemu_state = NEMU_RUNNING;

  // 模拟器最多执行n条指令
  bool print_flag = n < MAX_INSTR_TO_PRINT;

  while (n > 0) {
#ifdef CONFIG_JIT
    TB *tb = jit_lookup_sealed(cpu.eip);
    if (tb != NULL) {
      uint64_t native_executed = exec_native_tb(tb, n, print_flag);
      if (native_executed > 0) {
        n -= native_executed;
        if (nemu_state != NEMU_RUNNING) { return; }
        continue;
      }

      uint64_t executed = exec_cached_tb(tb, n, print_flag);
      if (executed > 0) {
        n -= executed;
        if (nemu_state != NEMU_RUNNING) { return; }
        continue;
      }
    }
#endif

    bool running = exec_one_instr(print_flag);
    n --;

    if (nemu_state != NEMU_RUNNING) { return; }
    if (!running) { return; }
  }

  if (nemu_state == NEMU_RUNNING) { nemu_state = NEMU_STOP; }
}
