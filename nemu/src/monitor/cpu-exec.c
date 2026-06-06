#include "nemu.h"
#include "cpu/jit.h"
#include "monitor/monitor.h"
#include "monitor/watchpoint.h"

/* The assembly code of instructions executed is only output to the screen
 * when the number of instructions executed is less than this value.
 * This is useful when you use the `si' command.
 * You can modify this value as you want.
 */
#define MAX_INSTR_TO_PRINT 10

int nemu_state = NEMU_STOP;

void exec_wrapper(bool);

static bool exec_one_instr(bool print_flag) {
  /* Execute one instruction, including instruction fetch,
   * instruction decode, and the actual execution. */
  exec_wrapper(print_flag);

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

#ifdef CONFIG_JIT
static uint64_t exec_cached_tb(TB *tb, uint64_t n, bool print_flag) {
  assert(tb != NULL && tb->valid && tb->sealed);

  uint64_t limit = tb->nr_instr < n ? tb->nr_instr : n;
  uint64_t executed = 0;
  bool aborted = false;

  jit_begin_tb_exec(tb);

  for (; executed < limit; executed ++) {
    bool running = exec_one_instr(print_flag);
    if (!running) {
      executed ++;
      break;
    }

    bool reached_end = cpu.eip == tb->exit_eip;
    bool outside_tb = cpu.eip < tb->guest_start || cpu.eip > tb->guest_end;

    if (executed + 1 < tb->nr_instr && (outside_tb || reached_end)) {
      aborted = true;
      executed ++;
      break;
    }
  }

  if (executed == tb->nr_instr && cpu.eip != tb->exit_eip) {
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
