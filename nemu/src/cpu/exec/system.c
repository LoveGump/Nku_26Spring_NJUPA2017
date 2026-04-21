#include "cpu/exec.h"
#include "device/port-io.h"

void diff_test_skip_qemu();
void diff_test_skip_nemu();

// implementation of `lidt' instruction
void raise_intr(uint8_t NO, vaddr_t ret_addr);

make_EHelper(lidt) {
  // 在 x86 里，LIDT 是 Load IDTR 指令，
  // 用来把内存中的 6 字节描述符加载到 IDTR（中断描述符表寄存器）

  // 6 字节描述符的格式如下：
  // +-----------------+-----------------+
  // |      Limit      |       Base      |
  // |     2 bytes     |     4 bytes     |
  // +-----------------+-----------------+
  cpu.idtr.limit = vaddr_read(id_dest->addr, 2);
  cpu.idtr.base = vaddr_read(id_dest->addr + 2, 4);

  print_asm_template1(lidt);
}

// move_r2cr : 将通用寄存器的值移动到控制寄存器中
make_EHelper(mov_r2cr) {
  switch (id_dest->reg) {
    // 在 x86 架构中，常见的控制寄存器有 CR0、CR3 等
    case 0: cpu.cr0.val = id_src->val; break;
    case 3: cpu.cr3.val = id_src->val; break;
    default: panic("unsupported control register CR%d", id_dest->reg);
  }

  print_asm("movl %%%s,%%cr%d", reg_name(id_src->reg, 4), id_dest->reg);
}

// move_cr2r : 将控制寄存器的值移动到通用寄存器中
make_EHelper(mov_cr2r) {
  switch (id_src->reg) {
    // 加载到通用寄存器时，根据控制寄存器的编号，获取对应的值
    case 0: rtl_li(&t0, cpu.cr0.val); break;
    case 3: rtl_li(&t0, cpu.cr3.val); break;
    default: panic("unsupported control register CR%d", id_src->reg);
  }
  operand_write(id_dest, &t0);

  print_asm("movl %%cr%d,%%%s", id_src->reg, reg_name(id_dest->reg, 4));

#ifdef DIFF_TEST
  diff_test_skip_qemu();
#endif
}

// exec_int ： 执行一个软件中断，参数 NO 是中断号，ret_addr 是返回地址
make_EHelper(int) {

  raise_intr(id_dest->val, *eip);

  print_asm("int %s", id_dest->str);

#ifdef DIFF_TEST
  diff_test_skip_nemu();
#endif
}

// iret ： 执行一个中断返回指令，恢复之前压栈的 EIP、CS 和 EFLAGS
make_EHelper(iret) {
  rtl_pop(&t0); // pop EIP
  rtl_pop(&t1); // pop CS, ignored in NEMU
  rtl_pop(&t2); // pop EFLAGS
  cpu.eflags = t2;

  // 设置 jmp_eip 和 is_jmp 标志，告诉 CPU 需要跳转到新的 EIP
  decoding.jmp_eip = t0;
  decoding.is_jmp = 1;

  print_asm("iret");
}

uint32_t pio_read(ioaddr_t, int);
void pio_write(ioaddr_t, int, uint32_t);

// in ： 从端口读取数据到通用寄存器
make_EHelper(in) {
  // 端口输入操作，使用 pio_read 从指定的端口读取数据，并将结果存储到通用寄存器中
  rtl_li(&t0, pio_read(id_src->val, id_dest->width));
  operand_write(id_dest, &t0);

  print_asm_template2(in);

#ifdef DIFF_TEST
  diff_test_skip_qemu();
#endif
}

// out ： 从通用寄存器写数据到端口
make_EHelper(out) {
  pio_write(id_dest->val, id_src->width, id_src->val);

  print_asm_template2(out);

#ifdef DIFF_TEST
  diff_test_skip_qemu();
#endif
}
