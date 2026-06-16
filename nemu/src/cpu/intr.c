#include "cpu/exec.h"
#include "memory/mmu.h"

void raise_intr(uint8_t NO, vaddr_t ret_addr) {
  // 每个 IDT 表项占 8 字节，用中断号 NO 找到对应的门描述符。
  vaddr_t gate_addr = cpu.idtr.base + NO * 8;
  uint32_t low = vaddr_read(gate_addr, 4);
  uint32_t high = vaddr_read(gate_addr + 4, 4);

  // 检查门描述符的 Present 位，确保该中断处理入口有效。
  assert(high & 0x8000);

  // 中断门的目标地址被拆成低 16 位和高 16 位，这里将它们拼成完整入口地址。
  uint32_t target = (low & 0x0000ffff) | (high & 0xffff0000);

  // 模拟 x86 硬件进入中断时的压栈顺序：EFLAGS、CS、返回地址 EIP。
  // 这些内容会在中断处理结束执行 iret 时被恢复。
  rtl_li(&t0, cpu.eflags);
  rtl_push(&t0);
  rtl_li(&t0, cpu.cs);
  rtl_push(&t0);
  rtl_li(&t0, ret_addr);
  rtl_push(&t0);

  // 进入中断处理后关闭可屏蔽中断，避免嵌套中断打断当前处理过程。
  cpu.IF = 0;

  // 通知执行框架下一条指令跳转到中断处理函数入口。
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
