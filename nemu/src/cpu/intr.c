#include "cpu/exec.h"
#include "memory/mmu.h"

void raise_intr(uint8_t NO, vaddr_t ret_addr) {
  /* TODO(finished): Trigger an interrupt/exception with ``NO''.
   * That is, use ``NO'' to index the IDT.
   */
  
  // 从 IDT 中获取中断门描述符
  uint32_t gate_addr = cpu.idtr.base + (NO << 3);
  // 获取中断处理程序的地址
  uint32_t low = vaddr_read(gate_addr, 4);
  uint32_t high = vaddr_read(gate_addr + 4, 4);
  uint32_t target = (low & 0x0000ffff) | (high & 0xffff0000);

  // 将当前 EFLAGS、CS 和 EIP 压栈，以便中断处理程序返回时能够恢复现场
  rtl_li(&t0, cpu.eflags);
  rtl_push(&t0);
  rtl_li(&t0, 0x8); // CS selector in protected mode used by AM (0x8 是 AM 中的代码段选择子)
  rtl_push(&t0);
  rtl_li(&t0, ret_addr);
  rtl_push(&t0);

  // 关闭中断标志位，防止在处理中断时再次被中断打断
  cpu.IF = 0;                 // 关闭中断标志位
  decoding.jmp_eip = target;  // 跳转到中断处理程序的地址
  decoding.is_jmp = 1;        // 设置 is_jmp 标志，告诉 CPU 需要跳转到新的 EIP
}

// dev_raise_intr : 设备触发中断的函数，通常在设备模拟中调用
void dev_raise_intr() {
  // 如果 IF 标志位被设置，说明 CPU 允许响应中断，此时触发一个中断（例如，0x20 是时钟中断）
  if (cpu.IF) {
    raise_intr(0x20, cpu.eip);
  }
}
