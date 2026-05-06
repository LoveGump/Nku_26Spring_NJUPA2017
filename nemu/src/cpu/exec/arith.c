#include "cpu/exec.h"

make_EHelper(add) {
  uint32_t mask = rtl_width_mask(id_dest->width);

  // 先按操作数宽度截断，再基于截断后的值计算结果和标志位。
  rtl_andi(&t0, &id_dest->val, mask);
  rtl_andi(&t1, &id_src->val, mask);

  rtl_add(&t2, &t0, &t1); // t2 = t0 + t1
  rtl_andi(&t2, &t2, mask);
  operand_write(id_dest, &t2);
  rtl_update_ZFSF(&t2, id_dest->width);

  // 无符号进位：截断后的结果小于任一加数时产生进位。
  rtl_sltu(&t3, &t2, &t0);
  rtl_set_CF(&t3);

  // 有符号溢出：两输入同号，但结果与输入异号。
  rtl_xor(&t3, &t0, &t1); // t3 = t0 ^ t1
  rtl_not(&t3);                      // t3 = ~(t0 ^ t1) t0 和 t1 同号
  rtl_xor(&t1, &t0, &t2); // t1 = t0 ^ t2    t0 和 t2 异号
  rtl_and(&t3, &t3, &t1); 
  rtl_msb(&t3, &t3, id_dest->width); // 获得 最高位 作为 OF 标志
  rtl_set_OF(&t3);

  print_asm_template2(add);
}

// sub 的实现和 add 类似
make_EHelper(sub) {
  uint32_t mask = rtl_width_mask(id_dest->width);

  rtl_andi(&t0, &id_dest->val, mask);
  rtl_andi(&t1, &id_src->val, mask);

  rtl_sub(&t2, &t0, &t1);  // t2 = t0 - t1
  rtl_andi(&t2, &t2, mask);
  operand_write(id_dest, &t2);
  rtl_update_ZFSF(&t2, id_dest->width);

  // 无符号借位：当被减数小于减数时，CF 置 1。
  rtl_sltu(&t3, &t0, &t1); // t3 = (t0 < t1) ? 1 : 0
  rtl_set_CF(&t3);

  // 有符号溢出：两个输入异号，且结果与被减数异号。
  rtl_xor(&t3, &t0, &t1); // t3 = t0 ^ t1  t0 和 t1 异号
  rtl_xor(&t1, &t0, &t2); // t1 = t0 ^ t2  t0 和 t2 异号
  rtl_and(&t3, &t3, &t1); 
  rtl_msb(&t3, &t3, id_dest->width); // 获得 最高位 作为 OF 标志
  rtl_set_OF(&t3);

  print_asm_template2(sub);
}

// cmp 只关心结果的标志位，不写回结果
make_EHelper(cmp) {
  uint32_t mask = rtl_width_mask(id_dest->width);

  rtl_andi(&t0, &id_dest->val, mask);
  rtl_andi(&t1, &id_src->val, mask);

  rtl_sub(&t2, &t0, &t1); // t2 = t0 - t1
  rtl_andi(&t2, &t2, mask);
  rtl_update_ZFSF(&t2, id_dest->width);

  rtl_sltu(&t3, &t0, &t1); // t3 = (t0 < t1) ? 1 : 0
  rtl_set_CF(&t3);

  // update OF 标志：两个输入异号，且结果与被减数异号。
  rtl_xor(&t3, &t0, &t1);
  rtl_xor(&t1, &t0, &t2);
  rtl_and(&t3, &t3, &t1);
  rtl_msb(&t3, &t3, id_dest->width);
  rtl_set_OF(&t3);

  print_asm_template2(cmp);
}

// inc 是 自增1 ，不更改 CF 标志
make_EHelper(inc) {
  uint32_t mask = rtl_width_mask(id_dest->width);

  rtl_get_CF(&t3); // 保存原来的 CF 标志，因为 inc 不修改 CF
  rtl_andi(&t0, &id_dest->val, mask); // t0 = dest 的值，按操作数宽度截断

  rtl_addi(&t2, &t0, 1);
  rtl_andi(&t2, &t2, mask);
  operand_write(id_dest, &t2);
  rtl_update_ZFSF(&t2, id_dest->width);

  rtl_mv(&t1, &t0); // t1 
  rtl_not(&t1);           //  t1 = ~t0
  rtl_and(&t1, &t1, &t2); // t1 = ~t0 & t2  当 t0 是最大正数时，t2 会变成最小负数，t1 的最高位会是1
  rtl_msb(&t1, &t1, id_dest->width);
  rtl_set_OF(&t1); // OF 只有在 inc 导致符号位从0变1时才会置位，即从最大正数变成最小负数
  rtl_set_CF(&t3);

  print_asm_template1(inc);
}

// dec 
make_EHelper(dec) {
  uint32_t mask = rtl_width_mask(id_dest->width);

  rtl_get_CF(&t3);
  rtl_andi(&t0, &id_dest->val, mask);

  rtl_subi(&t2, &t0, 1);
  rtl_andi(&t2, &t2, mask);
  operand_write(id_dest, &t2);
  rtl_update_ZFSF(&t2, id_dest->width);

  rtl_mv(&t1, &t2);
  rtl_not(&t1);
  rtl_and(&t1, &t0, &t1);
  rtl_msb(&t1, &t1, id_dest->width);
  rtl_set_OF(&t1);
  rtl_set_CF(&t3);

  print_asm_template1(dec);
}

// neg 是取反加一，即 -dest = ~dest + 1
make_EHelper(neg) {
  uint32_t mask = rtl_width_mask(id_dest->width);
  rtl_andi(&t0, &id_dest->val, mask);

  rtl_mv(&t2, &t0);
  rtl_not(&t2);
  rtl_addi(&t2, &t2, 1);
  rtl_andi(&t2, &t2, mask);
  operand_write(id_dest, &t2);
  rtl_update_ZFSF(&t2, id_dest->width);

  // 设置 CF 标志：当 dest 不为0时才会产生进位（相当于从0借1来做 neg）
  rtl_neq0(&t1, &t0);
  rtl_set_CF(&t1);

  // 设置 OF 标志：当 dest 是最小的负数时，取反加一会得到相同的数，产生溢出
  rtl_eqi(&t1, &t0, rtl_sign_mask(id_dest->width));
  rtl_set_OF(&t1);

  print_asm_template1(neg);
}

// adc 是带进位的加法，即 dest + src + CF
make_EHelper(adc) {
  rtl_add(&t2, &id_dest->val, &id_src->val); // t2 = dest + src
  rtl_sltu(&t3, &t2, &id_dest->val);        // t3 = (t2 < dest) ? 1 : 0，保存 dest + src 是否产生了无符号进位
  rtl_get_CF(&t1);                           // t1 = CF       
  rtl_add(&t2, &t2, &t1);         // t2 = t2 + t1
  operand_write(id_dest, &t2);                    // 写回结果

  rtl_update_ZFSF(&t2, id_dest->width); // 更新 ZF 和 SF 标志

  rtl_sltu(&t0, &t2, &id_dest->val); // t0 = (t2 < dest) ? 1 : 0，保存 dest + src + CF 是否产生了无符号进位
  rtl_or(&t0, &t3, &t0);              // t0 = t3 | t0 
  rtl_set_CF(&t0);                

  // 设置 OF 标志：当 dest 和 src 同号，且结果与 dest 异号时，才会产生溢出
  rtl_xor(&t0, &id_dest->val, &id_src->val); // t0 = dest ^ src
  rtl_not(&t0);                                         // t0 = ~(dest ^ src) 当 dest 和 src 同号时，t0 的最高位会是1
  rtl_xor(&t1, &id_dest->val, &t2);
  rtl_and(&t0, &t0, &t1);
  rtl_msb(&t0, &t0, id_dest->width);
  rtl_set_OF(&t0);
  print_asm_template2(adc);
}

// sbb 是带借位的减法，即 dest - src - CF
make_EHelper(sbb) {
  rtl_sub(&t2, &id_dest->val, &id_src->val);
  rtl_sltu(&t3, &id_dest->val, &t2);
  rtl_get_CF(&t1);
  rtl_sub(&t2, &t2, &t1);
  operand_write(id_dest, &t2);

  rtl_update_ZFSF(&t2, id_dest->width);

  rtl_sltu(&t0, &id_dest->val, &t2);
  rtl_or(&t0, &t3, &t0);
  rtl_set_CF(&t0);

  rtl_xor(&t0, &id_dest->val, &id_src->val);
  rtl_xor(&t1, &id_dest->val, &t2);
  rtl_and(&t0, &t0, &t1);
  rtl_msb(&t0, &t0, id_dest->width);
  rtl_set_OF(&t0);
  print_asm_template2(sbb);
}

// mul 和 imul 的实现比较复杂，因为它们需要处理乘法结果的高位和低位，以及符号扩展等问题
make_EHelper(mul) {
  rtl_lr(&t0, R_EAX, id_dest->width);
  rtl_mul(&t0, &t1, &id_dest->val, &t0);

  switch (id_dest->width) {
    case 1:
      rtl_sr_w(R_AX, &t1);
      break;
    case 2:
      rtl_sr_w(R_AX, &t1);
      rtl_shri(&t1, &t1, 16);
      rtl_sr_w(R_DX, &t1);
      break;
    case 4:
      rtl_sr_l(R_EDX, &t0);
      rtl_sr_l(R_EAX, &t1);
      break;
    default: assert(0);
  }

  print_asm_template1(mul);
}

// imul with one operand
make_EHelper(imul1) {
  rtl_lr(&t0, R_EAX, id_dest->width);
  rtl_imul(&t0, &t1, &id_dest->val, &t0);

  switch (id_dest->width) {
    case 1:
      rtl_sr_w(R_AX, &t1);
      break;
    case 2:
      rtl_sr_w(R_AX, &t1);
      rtl_shri(&t1, &t1, 16);
      rtl_sr_w(R_DX, &t1);
      break;
    case 4:
      rtl_sr_l(R_EDX, &t0);
      rtl_sr_l(R_EAX, &t1);
      break;
    default: assert(0);
  }
  print_asm_template1(imul);
}

// imul with two operands
make_EHelper(imul2) {
  rtl_sext(&id_src->val, &id_src->val, id_src->width);
  rtl_sext(&id_dest->val, &id_dest->val, id_dest->width);

  rtl_imul(&t0, &t1, &id_dest->val, &id_src->val);
  operand_write(id_dest, &t1);

  print_asm_template2(imul);
}

// imul with three operands
make_EHelper(imul3) {
  rtl_sext(&id_src->val, &id_src->val, id_src->width);
  rtl_sext(&id_src2->val, &id_src2->val, id_src->width);
  rtl_sext(&id_dest->val, &id_dest->val, id_dest->width);

  rtl_imul(&t0, &t1, &id_src2->val, &id_src->val);
  operand_write(id_dest, &t1);

  print_asm_template3(imul);
}

make_EHelper(div) {
  switch (id_dest->width) {
    case 1:
      rtl_li(&t1, 0);
      rtl_lr_w(&t0, R_AX);
      break;
    case 2:
      rtl_lr_w(&t0, R_AX);
      rtl_lr_w(&t1, R_DX);
      rtl_shli(&t1, &t1, 16);
      rtl_or(&t0, &t0, &t1);
      rtl_li(&t1, 0);
      break;
    case 4:
      rtl_lr_l(&t0, R_EAX);
      rtl_lr_l(&t1, R_EDX);
      break;
    default: assert(0);
  }

  rtl_div(&t2, &t3, &t1, &t0, &id_dest->val);

  rtl_sr(R_EAX, id_dest->width, &t2);
  if (id_dest->width == 1) {
    rtl_sr_b(R_AH, &t3);
  }
  else {
    rtl_sr(R_EDX, id_dest->width, &t3);
  }

  print_asm_template1(div);
}

make_EHelper(idiv) {
  rtl_sext(&id_dest->val, &id_dest->val, id_dest->width);

  switch (id_dest->width) {
    case 1:
      rtl_lr_w(&t0, R_AX);
      rtl_sext(&t0, &t0, 2);
      rtl_msb(&t1, &t0, 4);
      rtl_sub(&t1, &tzero, &t1);
      break;
    case 2:
      rtl_lr_w(&t0, R_AX);
      rtl_lr_w(&t1, R_DX);
      rtl_shli(&t1, &t1, 16);
      rtl_or(&t0, &t0, &t1);
      rtl_msb(&t1, &t0, 4);
      rtl_sub(&t1, &tzero, &t1);
      break;
    case 4:
      rtl_lr_l(&t0, R_EAX);
      rtl_lr_l(&t1, R_EDX);
      break;
    default: assert(0);
  }

  rtl_idiv(&t2, &t3, &t1, &t0, &id_dest->val);

  rtl_sr(R_EAX, id_dest->width, &t2);
  if (id_dest->width == 1) {
    rtl_sr_b(R_AH, &t3);
  }
  else {
    rtl_sr(R_EDX, id_dest->width, &t3);
  }

  print_asm_template1(idiv);
}
