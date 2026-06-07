#include "nemu.h"
#include "cpu/jit.h"
#include "device/mmio.h"

// 模拟内存
#define PMEM_SIZE (128 * 1024 * 1024)

#define pmem_rw(addr, type) *(type *)({\
    Assert(addr < PMEM_SIZE, "physical address(0x%08x) is out of bound", addr); \
    guest_to_host(addr); \
    })

uint8_t pmem[PMEM_SIZE];

/* Memory accessing interfaces */

static inline uint32_t page_entry_read(paddr_t addr) {
  /*
   * 页目录/页表是 CPU 使用的普通内存结构，不应放在 MMIO 区域。
   * page walk 是虚拟访存的热路径，这里直接读 pmem，避免每次读取 PDE/PTE
   * 都进入 paddr_read() 并重复执行 MMIO 判断。
   */
  Assert(addr <= PMEM_SIZE - sizeof(uint32_t),
      "page table physical address(0x%08x) is out of bound", addr);
  return *(uint32_t *)guest_to_host(addr);
}

// 模拟内存的读写接口：物理地址读写和虚拟地址读写
uint32_t paddr_read(paddr_t addr, int len) {
  /*
   * 大多数物理访存落在普通内存。先用 MMIO 整体边界在本函数内快速排除，
   * 避免每次普通读都进入 is_mmio() 函数。
   */
  if (mmio_may_hit(addr)) {
    int map_NO = is_mmio(addr);
    if (map_NO != -1) {
      return mmio_read(addr, len, map_NO);
    }
  }
  return pmem_rw(addr, uint32_t) & (~0u >> ((4 - len) << 3));
}
void paddr_write(paddr_t addr, int len, uint32_t data) {
  /* 写路径同样先做边界快拒绝，保留命中 MMIO 时的原有回调语义。 */
  if (mmio_may_hit(addr)) {
    int map_NO = is_mmio(addr);
    if (map_NO != -1) {
      mmio_write(addr, len, data, map_NO);
      return;
    }
  }
  memcpy(guest_to_host(addr), &data, len);
}

// 虚拟地址 -> 物理地址
static paddr_t page_translate(vaddr_t addr) {
  uint32_t dir = (addr >> 22) & 0x3ff;
  uint32_t page = (addr >> 12) & 0x3ff;
  uint32_t offset = addr & PAGE_MASK;

  // 获得页目录
  paddr_t pdir_base = cpu.cr3.page_directory_base << 12;
  PDE pde;  // 读取页目录项
  pde.val = page_entry_read(pdir_base + dir * sizeof(PDE));
  assert(pde.present);

  // 获得页表
  paddr_t ptab_base = pde.page_frame << 12;
  PTE pte;
  pte.val = page_entry_read(ptab_base + page * sizeof(PTE));
  assert(pte.present);

  return (pte.page_frame << 12) | offset;
}

uint32_t vaddr_read(vaddr_t addr, int len) {
  if (!cpu.cr0.paging) {
    // 如果分页未启用，直接使用物理地址读写接口进行访问
    return paddr_read(addr, len);
  }

  uint32_t offset = addr & PAGE_MASK;
  if (offset + len > PAGE_SIZE) {
    int left_len = PAGE_SIZE - offset;
    int right_len = len - left_len;
    uint32_t left = vaddr_read(addr, left_len);
    uint32_t right = vaddr_read(addr + left_len, right_len);
    return left | (right << (left_len << 3));
  }

  paddr_t paddr = page_translate(addr);
  return paddr_read(paddr, len);
}

void vaddr_write(vaddr_t addr, int len, uint32_t data) {
  if (!cpu.cr0.paging) {
    paddr_write(addr, len, data);
    /* 普通数据写不必清空全部 TB；只有覆盖已翻译指令时才失效对应 TB。 */
    jit_invalidate_range(addr, len);
    return;
  }

  uint32_t offset = addr & PAGE_MASK;
  if (offset + len > PAGE_SIZE) {
    int left_len = PAGE_SIZE - offset;
    int right_len = len - left_len;
    uint32_t left_mask = ~0u >> ((4 - left_len) << 3);
    vaddr_write(addr, left_len, data & left_mask);
    vaddr_write(addr + left_len, right_len, data >> (left_len << 3));
    return;
  }

  paddr_t paddr = page_translate(addr);
  paddr_write(paddr, len, data);
  /* 分页状态变化由 cr0/cr3 全量失效处理；普通写入只检查当前虚拟地址范围。 */
  jit_invalidate_range(addr, len);
}
