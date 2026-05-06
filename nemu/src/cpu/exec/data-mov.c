#include "cpu/exec.h"

make_EHelper(mov) {
  operand_write(id_dest, &id_src->val);
  print_asm_template2(mov);
}

// movs{b,w,l}: copy memory from [esi] to [edi], then advance esi/edi.
make_EHelper(movs) {
  rtl_lr_l(&t0, R_ESI);
  rtl_lr_l(&t1, R_EDI);
  rtl_lm(&t2, &t0, id_dest->width);
  rtl_sm(&t1, id_dest->width, &t2);
  rtl_addi(&t0, &t0, id_dest->width);
  rtl_addi(&t1, &t1, id_dest->width);
  rtl_sr_l(R_ESI, &t0);
  rtl_sr_l(R_EDI, &t1);

  print_asm("movs%c %%ds:(%%esi),%%es:(%%edi)", suffix_char(id_dest->width));
}

// push
make_EHelper(push) {
  rtl_push(&id_dest->val);

  print_asm_template1(push);
}

make_EHelper(pop) {
  rtl_pop(&t0);
  // pop 指令需要将栈顶的值写回到目的操作数中，
  //调用 operand_write 来处理不同类型的目的操作数（寄存器或内存）
  operand_write(id_dest, &t0);

  print_asm_template1(pop);
}

// pusha 一次性将所有通用寄存器的值压入栈中，顺序是 eax, ecx, edx, ebx, esp, ebp, esi, edi
make_EHelper(pusha) {
  rtl_lr_l(&t0, R_EAX);
  rtl_push(&t0);
  rtl_lr_l(&t0, R_ECX);
  rtl_push(&t0);
  rtl_lr_l(&t0, R_EDX);
  rtl_push(&t0);
  rtl_lr_l(&t0, R_EBX);
  rtl_push(&t0);

  rtl_lr_l(&t0, R_ESP);
  rtl_addi(&t0, &t0, 16);
  rtl_push(&t0);

  rtl_lr_l(&t0, R_EBP);
  rtl_push(&t0);
  rtl_lr_l(&t0, R_ESI);
  rtl_push(&t0);
  rtl_lr_l(&t0, R_EDI);
  rtl_push(&t0);

  print_asm("pusha");
}

// popa 一次性将栈顶的值弹出到所有通用寄存器中，顺序是 edi, esi, ebp, esp, ebx, edx, ecx, eax
make_EHelper(popa) {
  rtl_pop(&t0);
  rtl_sr_l(R_EDI, &t0);
  rtl_pop(&t0);
  rtl_sr_l(R_ESI, &t0);
  rtl_pop(&t0);
  rtl_sr_l(R_EBP, &t0);
  rtl_pop(&t0);
  rtl_pop(&t0);
  rtl_sr_l(R_EBX, &t0);
  rtl_pop(&t0);
  rtl_sr_l(R_EDX, &t0);
  rtl_pop(&t0);
  rtl_sr_l(R_ECX, &t0);
  rtl_pop(&t0);
  rtl_sr_l(R_EAX, &t0);

  print_asm("popa");
}

// leave 指令用于函数返回，等价于 mov esp, ebp; pop ebp
make_EHelper(leave) {
  rtl_lr_l(&t0, R_EBP);
  rtl_sr_l(R_ESP, &t0);
  rtl_pop(&t0);
  rtl_sr_l(R_EBP, &t0);

  print_asm("leave");
}

// cltd：根据操作数的符号位将 edx 寄存器设置为 0 或 -1
// 
make_EHelper(cltd) {
  if (decoding.is_operand_size_16) {
    rtl_lr_w(&t0, R_AX);
    rtl_msb(&t0, &t0, 2);
    rtl_sub(&t1, &tzero, &t0);
    rtl_sr_w(R_DX, &t1);
  }
  else {
    rtl_lr_l(&t0, R_EAX);
    rtl_msb(&t0, &t0, 4);
    rtl_sub(&t1, &tzero, &t0);
    rtl_sr_l(R_EDX, &t1);
  }

  print_asm(decoding.is_operand_size_16 ? "cwtl" : "cltd");
}

// cwtl：根据操作数的符号位将 dest 寄存器的值扩展到更宽的寄存器中
make_EHelper(cwtl) {
  if (decoding.is_operand_size_16) {
    rtl_lr_b(&t0, R_AL);
    rtl_sext(&t0, &t0, 1);
    rtl_sr_w(R_AX, &t0);
  }
  else {
    rtl_lr_w(&t0, R_AX);
    rtl_sext(&t0, &t0, 2);
    rtl_sr_l(R_EAX, &t0);
  }

  print_asm(decoding.is_operand_size_16 ? "cbtw" : "cwtl");
}

make_EHelper(movsx) {
  id_dest->width = decoding.is_operand_size_16 ? 2 : 4;
  rtl_sext(&t2, &id_src->val, id_src->width);
  operand_write(id_dest, &t2);
  print_asm_template2(movsx);
}

make_EHelper(movzx) {
  id_dest->width = decoding.is_operand_size_16 ? 2 : 4;
  operand_write(id_dest, &id_src->val);
  print_asm_template2(movzx);
}

make_EHelper(lea) {
  rtl_li(&t2, id_src->addr);
  operand_write(id_dest, &t2);
  print_asm_template2(lea);
}
