#include "proc.h"

#define MAX_NR_PROC 4
#define GAME_SCHEDULE_WEIGHT 256

static PCB pcb[MAX_NR_PROC];
static int nr_proc = 0;
static int game_count = 0;
static PCB *current_game = NULL;
PCB *current = NULL;

uintptr_t loader(_Protect *as, const char *filename);

void load_prog(const char *filename) {
  int i = nr_proc ++;
  _protect(&pcb[i].as);
  if (i == 0) {
    // 第一个进程默认是第一个游戏
    current_game = &pcb[i];
  }

  uintptr_t entry = loader(&pcb[i].as, filename);

  // TODO: remove the following three lines after you have implemented _umake()
  // _switch(&pcb[i].as);
  // current = &pcb[i];
  // ((void (*)(void))entry)();

  _Area stack;
  stack.start = pcb[i].stack;
  stack.end = stack.start + sizeof(pcb[i].stack);

  pcb[i].tf = _umake(&pcb[i].as, stack, stack, (void *)entry, NULL, NULL);
}

void switch_game(void) {
  // 在两个游戏之间切换，切换时重置调度权重计数器
  current_game = (current_game == &pcb[0] ? &pcb[2] : &pcb[0]);
  game_count = 0;
}

_RegSet* schedule(_RegSet *prev) {
  if (current != NULL) {
    current->tf = prev;
  }

  if (current_game == NULL) {
    current_game = &pcb[0];
  }

  if (current == current_game && game_count >= GAME_SCHEDULE_WEIGHT) {
    current = &pcb[1];
    game_count = 0;
  }
  else {
    current = current_game;
    game_count ++;
  }

  _switch(&current->as);
  return current->tf;
}
