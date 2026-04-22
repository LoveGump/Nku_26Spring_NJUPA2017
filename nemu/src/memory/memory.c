#include "nemu.h"
#include "device/mmio.h"

// 模拟内存
#define PMEM_SIZE (128 * 1024 * 1024)

uint8_t pmem[PMEM_SIZE];

/* Memory accessing interfaces */

// 模拟内存的读写接口：物理地址读写和虚拟地址读写
uint32_t paddr_read(paddr_t addr, int len) {
  Assert(len == 1 || len == 2 || len == 4, "invalid read len = %d", len);

  int map_NO = is_mmio(addr);
  if (map_NO != -1) {
    return mmio_read(addr, len, map_NO);
  }

  Assert((uint64_t)addr + len - 1 < PMEM_SIZE,
      "physical read out of bound: addr = 0x%08x, len = %d, eip = 0x%08x",
      addr, len, cpu.eip);

  uint32_t data = 0;
  memcpy(&data, guest_to_host(addr), len);
  return data;
}
void paddr_write(paddr_t addr, int len, uint32_t data) {
  Assert(len == 1 || len == 2 || len == 4, "invalid write len = %d", len);

  int map_NO = is_mmio(addr);
  if (map_NO != -1) {
    mmio_write(addr, len, data, map_NO);
    return;
  }

  Assert((uint64_t)addr + len - 1 < PMEM_SIZE,
      "physical write out of bound: addr = 0x%08x, len = %d, eip = 0x%08x",
      addr, len, cpu.eip);

  memcpy(guest_to_host(addr), &data, len);
}

uint32_t vaddr_read(vaddr_t addr, int len) {
  return paddr_read(addr, len);
}

void vaddr_write(vaddr_t addr, int len, uint32_t data) {
  paddr_write(addr, len, data);
}
