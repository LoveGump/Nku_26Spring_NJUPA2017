#ifndef __PROC_H__
#define __PROC_H__

#include "common.h"
#include "memory.h"

#define STACK_SIZE (8 * PGSIZE)

typedef union {
  uint8_t stack[STACK_SIZE] PG_ALIGN;
  struct {
    _RegSet *tf; // trap frame
    _Protect as; // address space
    uintptr_t cur_brk; // 当前 brk 的值
    // we do not free memory, so use `max_brk' to determine when to call _map()
    uintptr_t max_brk;  // 当前进程的最大 brk 的值
  };
} PCB;

extern PCB *current;

#endif
