#include <x86.h>

// 页目录和页表都需要按页对齐，便于作为 CR3 或页表基址使用。
#define PG_ALIGN __attribute((aligned(PGSIZE)))

// 内核页目录和内核页表，用来建立内核的恒等映射。
static PDE kpdirs[NR_PDE] PG_ALIGN;
static PTE kptabs[PMEM_SIZE / PGSIZE] PG_ALIGN;
// 页框分配/释放函数由 Nanos-lite 传入，AM 层只保存并调用它们。
static void* (*palloc_f)();
static void (*pfree_f)(void*);

// 需要建立内核映射的地址区间。目前把整个物理内存做恒等映射。
_Area segments[] = {      // Kernel memory mappings
  {.start = (void*)0,          .end = (void*)PMEM_SIZE}
};

#define NR_KSEG_MAP (sizeof(segments) / sizeof(segments[0]))

// 传入两个函数指针，分别用于分配和释放页框
void _pte_init(void* (*palloc)(), void (*pfree)(void*)) {
  // 保存页框分配器，之后创建进程地址空间和新页表时会用到。
  palloc_f = palloc;
  pfree_f = pfree;

  int i;

  // 清空内核页目录，使所有 PDE 初始为无效。
  for (i = 0; i < NR_PDE; i ++) {
    kpdirs[i] = 0;
  }

  // 使用预先分配好的 kptabs 为内核地址区间建立页表。
  PTE *ptab = kptabs;
  for (i = 0; i < NR_KSEG_MAP; i ++) {
    // 每个 PDE 覆盖 4MB 地址空间：PGSIZE * NR_PTE。
    uint32_t pdir_idx = (uintptr_t)segments[i].start / (PGSIZE * NR_PTE);
    uint32_t pdir_idx_end = (uintptr_t)segments[i].end / (PGSIZE * NR_PTE);
    for (; pdir_idx < pdir_idx_end; pdir_idx ++) {
      // 让页目录项指向当前页表，并设置 P 位表示有效。
      kpdirs[pdir_idx] = (uintptr_t)ptab | PTE_P;

      // 填充该页表覆盖的 4MB 区间，建立 va == pa 的恒等映射。
      PTE pte = PGADDR(pdir_idx, 0, 0) | PTE_P;
      PTE pte_end = PGADDR(pdir_idx + 1, 0, 0) | PTE_P;
      for (; pte < pte_end; pte += PGSIZE) {
        // 填充页表项，恒等映射
        *ptab = pte;
        ptab ++;
      }
    }
  }

  // 将 CR3 指向内核页目录，并打开 CR0.PG 启用分页。
  set_cr3(kpdirs);
  set_cr0(get_cr0() | CR0_PG);
}

// 为一个进程创建独立页目录，并继承内核映射。
void _protect(_Protect *p) {
  // 分配一页作为用户进程的页目录。
  PDE *updir = (PDE*)(palloc_f());
  p->ptr = updir;
  // 复制内核页目录，使每个进程都能访问内核恒等映射部分。
  for (int i = 0; i < NR_PDE; i ++) {
    updir[i] = kpdirs[i];
  }

  // 用户可分配的虚拟地址范围，loader 和 brk 会在这个区间内建立映射。
  p->area.start = (void*)0x8000000;
  p->area.end = (void*)0xc0000000;
}

// 当前实现没有回收页表和页框，因此释放接口为空。
void _release(_Protect *p) {
}

// 切换到目标进程地址空间，本质是把 CR3 改成该进程的页目录。
void _switch(_Protect *p) {
  set_cr3(p->ptr);
}

// 虚拟地址空间中的地址 va 映射到物理地址 pa
void _map(_Protect *p, void *va, void *pa) {
  PDE *pdir = (PDE *)p->ptr;
  // 从虚拟地址中拆出页目录索引和页表索引。
  uint32_t pdir_idx = PDX(va);
  uint32_t ptab_idx = PTX(va);

  // 如果页目录项无效，则分配一个新的页表，并将页目录项指向该页表
  if ((pdir[pdir_idx] & PTE_P) == 0) {
    PTE *ptab = (PTE *)palloc_f();
    // 新页表必须清零，否则未映射的虚拟页可能误认为有效。
    for (int i = 0; i < NR_PTE; i ++) {
      ptab[i] = 0;
    }
    // 设置 P/W/U 位，表示页表存在、可写、用户态可访问。
    pdir[pdir_idx] = (uintptr_t)ptab | PTE_P | PTE_W | PTE_U;
  }

  // 将页表项设置为物理地址 pa，并设置 P 位、W 位和 U 位
  PTE *ptab = (PTE *)PTE_ADDR(pdir[pdir_idx]);
  ptab[ptab_idx] = PTE_ADDR(pa) | PTE_P | PTE_W | PTE_U;
}

// 当前实现没有解除映射和回收页框。
void _unmap(_Protect *p, void *va) {
}

// 构造用户进程初始上下文。调度器第一次恢复该 tf 时，会从 entry 开始运行。
_RegSet *_umake(_Protect *p, _Area ustack, _Area kstack, void *entry, char *const argv[], char *const envp[]) {
  (void)p;
  (void)kstack;
  (void)argv;
  (void)envp;

  // 更新栈顶指针，构建用户栈
  uintptr_t *sp = (uintptr_t *)ustack.end;
  *(--sp) = 0;  // envp，暂不传环境变量
  *(--sp) = 0;  // argv，暂不传命令行参数
  *(--sp) = 0;  // argc
  *(--sp) = 0;  // 返回地址占位，用户程序正常不会 ret 到这里

  // 在用户栈上方预留一个 trap frame，作为中断返回时要恢复的寄存器现场。
  _RegSet *tf = (_RegSet *)((uintptr_t)sp - sizeof(_RegSet));
  // 通用寄存器初始清零。
  tf->edi = tf->esi = tf->ebp = 0;
  tf->esp = (uintptr_t)sp;
  tf->ebx = tf->edx = tf->ecx = tf->eax = 0;
  // 这里不是由真实中断进入，只是伪造一个可被 iret 恢复的现场。
  tf->irq = 0;
  tf->error_code = 0;
  // eip 指向用户程序入口。
  tf->eip = (uintptr_t)entry;
  tf->cs = KSEL(SEG_KCODE);
  // eflags bit1 恒为 1，同时打开 IF 允许时钟中断。
  tf->eflags = 0x2 | FL_IF; // IF = 1, enable interrupt

  return tf;
}
