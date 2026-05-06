#include "cpu/exec.h"
#include "memory/mmu.h"

void raise_intr(uint8_t NO, vaddr_t ret_addr) {
  /* TODO(finished): Trigger an interrupt/exception with ``NO''.
   * That is, use ``NO'' to index the IDT.
   */

  vaddr_t gate_addr = cpu.idtr.base + NO * 8;
  uint32_t low = vaddr_read(gate_addr, 4);
  uint32_t high = vaddr_read(gate_addr + 4, 4);
  assert(high & 0x8000);
  uint32_t target = (low & 0x0000ffff) | (high & 0xffff0000);

  rtl_li(&t0, cpu.eflags);
  rtl_push(&t0);
  rtl_li(&t0, cpu.cs);
  rtl_push(&t0);
  rtl_li(&t0, ret_addr);
  rtl_push(&t0);

  cpu.IF = 0;
  decoding.jmp_eip = target;
  decoding.is_jmp = 1;
}

// dev_raise_intr : 设备触发中断的函数，通常在设备模拟中调用
void dev_raise_intr() {
  // 如果 IF 标志位被设置，说明 CPU 允许响应中断，此时触发一个中断（例如，0x20 是时钟中断）
  if (cpu.IF) {
    raise_intr(0x20, cpu.eip);
  }
}
