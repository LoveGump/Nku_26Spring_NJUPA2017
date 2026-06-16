#include "nemu.h"
#include "cpu/jit.h"
#include "device/mmio.h"

// NEMU 为 guest 模拟的物理内存大小：128MB。
#define PMEM_SIZE (128 * 1024 * 1024)

// 根据 guest 物理地址获得 host 指针，并在访问前检查是否越界。
// type 用来决定按 1/2/4 字节中的哪种宿主类型解释内存。
#define pmem_rw(addr, type) *(type *)({\
    Assert(addr < PMEM_SIZE, "physical address(0x%08x) is out of bound", addr); \
    guest_to_host(addr); \
    })

// 真实存放 guest 物理内存内容的数组。
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

  // 按 4 字节取出后用掩码保留 len 字节，适配 1/2/4 字节读。
  return pmem_rw(addr, uint32_t) & (~0u >> ((4 - len) << 3));
}

// 物理地址写入接口：MMIO 地址交给设备处理，普通内存则直接拷贝到 pmem。
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

// 二级页表地址翻译：将 32 位虚拟地址转换成物理地址。
static paddr_t page_translate(vaddr_t addr) {
  // x86 32 位分页：高 10 位为页目录索引，中间 10 位为页表索引，低 12 位为页内偏移。
  uint32_t dir = (addr >> 22) & 0x3ff;
  uint32_t page = (addr >> 12) & 0x3ff;
  uint32_t offset = addr & PAGE_MASK;

  // CR3 保存页目录物理基址的高 20 位。
  paddr_t pdir_base = cpu.cr3.page_directory_base << 12;
  PDE pde;  // 读取页目录项
  pde.val = page_entry_read(pdir_base + dir * sizeof(PDE));
  assert(pde.present);

  // PDE 中的 page_frame 指向二级页表的物理页框。
  paddr_t ptab_base = pde.page_frame << 12;
  // 根据页表索引读取 PTE，present 为 1 表示目标物理页有效。
  PTE pte;
  pte.val = page_entry_read(ptab_base + page * sizeof(PTE));
  assert(pte.present);

  // PTE 给出物理页框，和页内偏移拼接得到最终物理地址。
  return (pte.page_frame << 12) | offset;
}

// 虚拟地址读取接口。分页关闭时虚拟地址等同于物理地址；分页开启时先翻译地址。
uint32_t vaddr_read(vaddr_t addr, int len) {
  if (!cpu.cr0.paging) {
    return paddr_read(addr, len);
  }

  uint32_t offset = addr & PAGE_MASK;
  // 如果一次访问跨越页边界，需要拆成左右两段分别翻译，再按小端序拼回结果。
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

// 虚拟地址写入接口，处理逻辑与 vaddr_read() 对称。
void vaddr_write(vaddr_t addr, int len, uint32_t data) {
  if (!cpu.cr0.paging) {
    paddr_write(addr, len, data);
    /* 普通数据写不必清空全部 TB；只有覆盖已翻译指令时才失效对应 TB。 */
    jit_invalidate_range(addr, len);
    return;
  }

  uint32_t offset = addr & PAGE_MASK;
  // 跨页写入也要拆成两段。低地址部分写 data 的低 left_len 字节。
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
