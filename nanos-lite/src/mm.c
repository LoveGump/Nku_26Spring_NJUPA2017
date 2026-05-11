#include "proc.h"
#include "memory.h"

static void *pf = NULL;

void* new_page(void) {
  assert(pf < (void *)_heap.end);
  void *p = pf;
  pf += PGSIZE;
  return p;
}

void free_page(void *p) {
  panic("not implement yet");
}

/* The brk() system call handler. */
// 把新申请的内存映射到当前进程的地址空间中
int mm_brk(uint32_t new_brk) {
  if (current->cur_brk == 0) {
    current->cur_brk = current->max_brk = new_brk;
  }
  else {
    if (new_brk > current->max_brk) {
      // 如果新的 brk 超过了当前的最大 brk，
      // 则需要将新的内存映射到当前进程的地址空间中
      uintptr_t map_start = PGROUNDUP(current->max_brk);
      uintptr_t map_end = PGROUNDUP(new_brk);
      for (uintptr_t va = map_start; va < map_end; va += PGSIZE) {
        _map(&current->as, (void *)va, new_page());
      }

      current->max_brk = new_brk;
    }

    current->cur_brk = new_brk;
  }

  return 0;
}

void init_mm() {
  pf = (void *)PGROUNDUP((uintptr_t)_heap.start);
  Log("free physical pages starting from %p", pf);

  _pte_init(new_page, free_page);
}
