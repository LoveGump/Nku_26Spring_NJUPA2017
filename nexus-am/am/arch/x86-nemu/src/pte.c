#include <x86.h>

#define PG_ALIGN __attribute((aligned(PGSIZE)))

static PDE kpdirs[NR_PDE] PG_ALIGN;
static PTE kptabs[PMEM_SIZE / PGSIZE] PG_ALIGN;
static void* (*palloc_f)();
static void (*pfree_f)(void*);

_Area segments[] = {      // Kernel memory mappings
  {.start = (void*)0,          .end = (void*)PMEM_SIZE}
};

#define NR_KSEG_MAP (sizeof(segments) / sizeof(segments[0]))

// 传入两个函数指针，分别用于分配和释放页框
void _pte_init(void* (*palloc)(), void (*pfree)(void*)) {
  palloc_f = palloc;
  pfree_f = pfree;

  int i;

  // make all PDEs invalid
  // 清空内核页目录
  for (i = 0; i < NR_PDE; i ++) {
    kpdirs[i] = 0;
  }

  // 重新分配 页表
  PTE *ptab = kptabs;
  for (i = 0; i < NR_KSEG_MAP; i ++) {
    uint32_t pdir_idx = (uintptr_t)segments[i].start / (PGSIZE * NR_PTE);
    uint32_t pdir_idx_end = (uintptr_t)segments[i].end / (PGSIZE * NR_PTE);
    for (; pdir_idx < pdir_idx_end; pdir_idx ++) {
      // fill PDE 只有 P 位
      kpdirs[pdir_idx] = (uintptr_t)ptab | PTE_P;

      // fill PTE
      PTE pte = PGADDR(pdir_idx, 0, 0) | PTE_P;
      PTE pte_end = PGADDR(pdir_idx + 1, 0, 0) | PTE_P;
      for (; pte < pte_end; pte += PGSIZE) {
        // 填充页表项，恒等映射
        *ptab = pte;
        ptab ++;
      }
    }
  }

  set_cr3(kpdirs);
  set_cr0(get_cr0() | CR0_PG);
}

void _protect(_Protect *p) {
  PDE *updir = (PDE*)(palloc_f());
  p->ptr = updir;
  // map kernel space
  for (int i = 0; i < NR_PDE; i ++) {
    updir[i] = kpdirs[i];
  }

  p->area.start = (void*)0x8000000;
  p->area.end = (void*)0xc0000000;
}

void _release(_Protect *p) {
}

void _switch(_Protect *p) {
  set_cr3(p->ptr);
}

// 虚拟地址空间中的地址 va 映射到物理地址 pa
void _map(_Protect *p, void *va, void *pa) {
  PDE *pdir = (PDE *)p->ptr;
  uint32_t pdir_idx = PDX(va);
  uint32_t ptab_idx = PTX(va);

  // 如果页目录项无效，则分配一个新的页表，并将页目录项指向该页表
  if ((pdir[pdir_idx] & PTE_P) == 0) {
    PTE *ptab = (PTE *)palloc_f();
    for (int i = 0; i < NR_PTE; i ++) {
      ptab[i] = 0;
    }
    pdir[pdir_idx] = (uintptr_t)ptab | PTE_P | PTE_W | PTE_U;
  }

  // 将页表项设置为物理地址 pa，并设置 P 位、W 位和 U 位
  PTE *ptab = (PTE *)PTE_ADDR(pdir[pdir_idx]);
  ptab[ptab_idx] = PTE_ADDR(pa) | PTE_P | PTE_W | PTE_U;
}

void _unmap(_Protect *p, void *va) {
}

_RegSet *_umake(_Protect *p, _Area ustack, _Area kstack, void *entry, char *const argv[], char *const envp[]) {
  return NULL;
}
