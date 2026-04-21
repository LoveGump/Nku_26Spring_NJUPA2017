#include "cpu/exec.h"

// test:按位与测试指令，计算 dest & src 的结果，但不写回目的操作数，只更新标志位
make_EHelper(test) {
  rtl_and(&t2, &id_dest->val, &id_src->val);
  rtl_update_ZFSF(&t2, id_dest->width);
  rtl_li(&t0, 0);
  rtl_set_CF(&t0);
  rtl_set_OF(&t0);

  print_asm_template2(test);
}

// and、xor、or 的实现比较类似，都是先计算结果写回目的操作数，然后更新标志位
make_EHelper(and) {
  rtl_and(&t2, &id_dest->val, &id_src->val);
  operand_write(id_dest, &t2);
  rtl_update_ZFSF(&t2, id_dest->width);
  rtl_li(&t0, 0);
  rtl_set_CF(&t0);
  rtl_set_OF(&t0);

  print_asm_template2(and);
}

make_EHelper(xor) {
  rtl_xor(&t2, &id_dest->val, &id_src->val);
  operand_write(id_dest, &t2);
  rtl_update_ZFSF(&t2, id_dest->width);
  rtl_li(&t0, 0);
  rtl_set_CF(&t0);
  rtl_set_OF(&t0);

  print_asm_template2(xor);
}

make_EHelper(or) {
  rtl_or(&t2, &id_dest->val, &id_src->val);
  operand_write(id_dest, &t2);
  rtl_update_ZFSF(&t2, id_dest->width);
  rtl_li(&t0, 0);
  rtl_set_CF(&t0);
  rtl_set_OF(&t0);

  print_asm_template2(or);
}

// sar、shl、shr 的实现也比较类似，都是先计算结果写回目的操作数，然后更新标志位
make_EHelper(sar) {
  rtl_andi(&t1, &id_src->val, 0x1f); // t1 = shift count
  
  // 1. 只有 count > 0 时才更新标志位
  if (t1 != 0) {
    // 2. 更新 CF：获取被移出的最后一位
    // 先将原数据右移 (count - 1) 位，然后取最低位
    rtl_subi(&t0, &t1, 1);
    rtl_shr(&t0, &id_dest->val, &t0);
    rtl_andi(&t0, &t0, 1);
    rtl_set_CF(&t0);

    // 3. 执行真正的算术右移
    rtl_sar(&t2, &id_dest->val, &t1);
    operand_write(id_dest, &t2);

    // 4. 更新 ZF 和 SF
    rtl_update_ZFSF(&t2, id_dest->width);
    
    // 5. 根据手册，SAR 超过 0 位时，OF 清零
    t0 = 0;
    rtl_set_OF(&t0);
  } else {
    // count 为 0，只写回原值（虽然没变），不改标志位
    operand_write(id_dest, &id_dest->val);
  }

  print_asm_template2(sar);
}

make_EHelper(shl) {
  // 按照 x86 规范，移位次数需与 0x1f 相与（只取低 5 位）
  rtl_andi(&t1, &id_src->val, 0x1f);
  
  rtl_shl(&t2, &id_dest->val, &t1);
  operand_write(id_dest, &t2);

  // fixed
  // 只有当移位次数不为 0 时，才更新 ZF 和 SF
  if (t1 != 0) {
    rtl_update_ZFSF(&t2, id_dest->width);
  }

  print_asm_template2(shl);
}

make_EHelper(shr) {
  rtl_andi(&t1, &id_src->val, 0x1f);
  rtl_shr(&t2, &id_dest->val, &t1);
  operand_write(id_dest, &t2);

  // fixed
  if (t1 != 0) {
    rtl_update_ZFSF(&t2, id_dest->width);
  }

  print_asm_template2(shr);
}

make_EHelper(setcc) {
  uint8_t subcode = decoding.opcode & 0xf;
  rtl_setcc(&t2, subcode);
  operand_write(id_dest, &t2);

  print_asm("set%s %s", get_cc_name(subcode), id_dest->str);
}

// not 指令是按位取反，即 dest = ~dest
make_EHelper(not) {
  rtl_mv(&t2, &id_dest->val);
  rtl_not(&t2);
  operand_write(id_dest, &t2);

  print_asm_template1(not);
}
