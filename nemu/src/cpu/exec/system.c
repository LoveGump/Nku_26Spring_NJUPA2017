#include "cpu/exec.h"
#include "device/port-io.h"

void diff_test_skip_qemu();
void diff_test_skip_nemu();

// implementation of `lidt' instruction
void raise_intr(uint8_t NO, vaddr_t ret_addr);

// lidt ： 从内存中加载 IDTR 寄存器
make_EHelper(lidt) {
  // 在 x86 里，LIDT 是 Load IDTR 指令，
  // 用来把内存中的 6 字节描述符加载到 IDTR（中断描述符表寄存器）

  // 6 字节描述符的格式如下：
  // +-----------------+-----------------+
  // |      Limit      |       Base      |
  // |     2 bytes     |     4 bytes     |
  // +-----------------+-----------------+
  rtl_li(&t0, id_dest->addr);
  rtl_lm(&t1, &t0, 2);
  rtl_addi(&t0, &t0, 2);
  rtl_lm(&t2, &t0, 4);

  cpu.idtr.limit = t1;
  cpu.idtr.base = t2;

  print_asm_template1(lidt);
}

// move_r2cr : 将通用寄存器的值移动到控制寄存器中
make_EHelper(mov_r2cr) {
  rtl_mv(&t0, &id_src->val);
  switch (id_dest->reg) {
    case 0: cpu.cr0.val = t0; break;
    case 3: cpu.cr3.val = t0; break;
    default: panic("unsupported control register CR%d", id_dest->reg);
  }

  print_asm("movl %%%s,%%cr%d", reg_name(id_src->reg, 4), id_dest->reg);
}

// move_cr2r : 将控制寄存器的值移动到通用寄存器中
make_EHelper(mov_cr2r) {
  
  switch (id_src->reg) {
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

  rtl_andi(&t0, &id_dest->val, 0xff); // 记录中断号，取低8位
  raise_intr(t0, *eip); // 中断结束 继续执行

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

  rtl_andi(&t1, &id_src->val, 0xffff); // 端口号 低16位
  rtl_li(&t0, pio_read(t1, id_dest->width)); // 从端口读取数据
  operand_write(id_dest, &t0);

  print_asm_template2(in);

#ifdef DIFF_TEST
  diff_test_skip_qemu();
#endif
}

// out ： 从通用寄存器写数据到端口
make_EHelper(out) {

  rtl_andi(&t0, &id_dest->val, 0xffff); // 端口号 低16位
  rtl_andi(&t1, &id_src->val, rtl_width_mask(id_src->width)); // 输出数据 按操作数宽度截断
  pio_write(t0, id_src->width, t1); // 写数据到端口

  print_asm_template2(out);

#ifdef DIFF_TEST
  diff_test_skip_qemu();
#endif
}
