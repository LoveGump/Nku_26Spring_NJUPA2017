#include "proc.h"

// 当前最多维护 4 个进程控制块。
#define MAX_NR_PROC 4
// 游戏进程连续运行若干次调度后，让出一次 CPU 给辅助进程。
#define GAME_SCHEDULE_WEIGHT 256

// 简单的静态 PCB 池。
static PCB pcb[MAX_NR_PROC];
// 已加载进程数量。
static int nr_proc = 0;
// 当前游戏进程已经连续被调度的次数。
static int game_count = 0;
// 当前正在前台运行的游戏进程，可通过按键在两个游戏槽位间切换。
static PCB *current_game = NULL;
// 当前正在运行的进程。
PCB *current = NULL;

uintptr_t loader(_Protect *as, const char *filename);

// 加载一个用户程序，创建地址空间和初始 trap frame。
void load_prog(const char *filename) {
  int i = nr_proc ++;
  // 为该进程初始化独立的用户地址空间。
  _protect(&pcb[i].as);
  if (i == 0) {
    // 第一个进程默认是第一个游戏
    current_game = &pcb[i];
  }

  uintptr_t entry = loader(&pcb[i].as, filename);

  // 使用 PCB 内部的栈作为用户栈和内核栈区域。
  _Area stack;
  stack.start = pcb[i].stack;
  stack.end = stack.start + sizeof(pcb[i].stack);

  // 构造进程第一次被调度时要恢复的寄存器现场，eip 指向程序入口。
  pcb[i].tf = _umake(&pcb[i].as, stack, stack, (void *)entry, NULL, NULL);
}

// 切换前台游戏。当前加载顺序下，pcb[0] 和 pcb[2] 是两个可切换的游戏/图形程序。
void switch_game(void) {
  // 在两个游戏之间切换，切换时重置调度权重计数器
  current_game = (current_game == &pcb[0] ? &pcb[2] : &pcb[0]);
  game_count = 0;
}

// 时钟中断或 trap 到来时调用的调度函数，返回下一个要恢复的 trap frame。
_RegSet* schedule(_RegSet *prev) {
  if (current != NULL) {
    // 保存当前进程被中断时的现场，供下次继续运行。
    current->tf = prev;
  }

  // 如果还没有选定前台游戏，则默认选择第一个进程。
  if (current_game == NULL) {
    current_game = &pcb[0];
  }

  // 游戏进程运行到一定权重后，切到 pcb[1] 运行一次，避免它长期得不到 CPU。
  if (current == current_game && game_count >= GAME_SCHEDULE_WEIGHT) {
    current = &pcb[1];
    game_count = 0;
  }
  else {
    // 默认调度当前前台游戏，并累加其连续运行次数。
    current = current_game;
    game_count ++;
  }

  // 切换页目录，使之后恢复现场时处在目标进程的地址空间。
  _switch(&current->as);
  // 返回目标进程的 trap frame，底层中断返回流程会恢复这个现场。
  return current->tf;
}
