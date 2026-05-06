#include "cpu/exec.h"

// 更新逻辑运算指令的 ZF、SF、CF、OF 标志位
static inline void update_logic_zfsf_cf_of(const rtlreg_t *result, int width) {
  rtl_update_ZFSF(result, width);
  rtl_set_CF(&tzero);
  rtl_set_OF(&tzero);
}

// test:按位与测试指令，计算 dest & src 的结果，但不写回目的操作数，只更新标志位
make_EHelper(test) {

  uint32_t mask = rtl_width_mask(id_dest->width);
  rtl_andi(&t0, &id_dest->val, mask);
  rtl_andi(&t1, &id_src->val, mask);

  rtl_and(&t2, &t0, &t1);
  rtl_andi(&t2, &t2, mask);
  update_logic_zfsf_cf_of(&t2, id_dest->width);

  print_asm_template2(test);
}

// and、xor、or 的实现比较类似，都是先计算结果写回目的操作数，然后更新标志位
make_EHelper(and) {

  uint32_t mask = rtl_width_mask(id_dest->width);
  rtl_andi(&t0, &id_dest->val, mask);
  rtl_andi(&t1, &id_src->val, mask);

  rtl_and(&t2, &t0, &t1);
  rtl_andi(&t2, &t2, mask);
  operand_write(id_dest, &t2);

  update_logic_zfsf_cf_of(&t2, id_dest->width);

  print_asm_template2(and);
}

make_EHelper(xor) {

  uint32_t mask = rtl_width_mask(id_dest->width);
  rtl_andi(&t0, &id_dest->val, mask);
  rtl_andi(&t1, &id_src->val, mask);

  rtl_xor(&t2, &t0, &t1);
  rtl_andi(&t2, &t2, mask);
  operand_write(id_dest, &t2);

  update_logic_zfsf_cf_of(&t2, id_dest->width);

  print_asm_template2(xor);
}

make_EHelper(or) {

  uint32_t mask = rtl_width_mask(id_dest->width);
  rtl_andi(&t0, &id_dest->val, mask);
  rtl_andi(&t1, &id_src->val, mask);

  rtl_or(&t2, &t0, &t1);
  rtl_andi(&t2, &t2, mask);
  operand_write(id_dest, &t2);

  update_logic_zfsf_cf_of(&t2, id_dest->width);

  print_asm_template2(or);
}

// sar：算术右移，保持符号位不变
make_EHelper(sar) {
  uint32_t mask = rtl_width_mask(id_dest->width);
  rtl_andi(&t1, &id_src->val, 0x1f); // t1 = shift count 最高 5 位有效
  rtl_andi(&t2, &id_dest->val, mask);
  
  //  count > 0 
  if (t1 != 0) {
    // 更新 CF：获取被移出的最后一位
    // 先将原数据右移 (count - 1) 位，然后取最低位
    rtl_subi(&t0, &t1, 1);    // t0 = count - 1
    rtl_shr(&t0, &t2, &t0);
    rtl_andi(&t0, &t0, 1);    // t0 = 被移出的最后一位
    rtl_set_CF(&t0);

    // 先符号扩展
    rtl_sext(&t2, &t2, id_dest->width); // t2 = sign-extended dest
    rtl_sar(&t2, &t2, &t1);
    rtl_andi(&t2, &t2, mask);
    operand_write(id_dest, &t2);

    // 更新 ZF 和 SF
    rtl_update_ZFSF(&t2, id_dest->width);
    
    // 根据手册，SAR 超过 0 位时，OF 清零
    t0 = 0;
    rtl_set_OF(&t0);
  }

  print_asm_template2(sar);
}

// shl：逻辑左移，移入0
make_EHelper(shl) {
  uint32_t mask = rtl_width_mask(id_dest->width);
  rtl_andi(&t1, &id_src->val, 0x1f);
  rtl_andi(&t2, &id_dest->val, mask);
  if (t1 != 0) {
    // 获取最后移出的一位存入 CF
    rtl_subi(&t0, &t1, 1);  // t0 = count - 1
    rtl_shl(&t0, &t2, &t0);
    rtl_msb(&t0, &t0, id_dest->width); // t0 = 被移出的最后一位（原数据的第 width*8 - count 位）
    rtl_set_CF(&t0);

    // 执行实际移位
    rtl_shl(&t2, &t2, &t1);
    rtl_andi(&t2, &t2, mask);
    operand_write(id_dest, &t2);
    
    // 更新 ZF/SF
    rtl_update_ZFSF(&t2, id_dest->width);

    // count == 1 时 OF = 结果最高位 ^ CF；count > 1 时 OF 未定义，这里统一清零。
    if (t1 == 1) {
      // OF 仅在移位次数为 1 时有效。如果移位后符号位发生了变化（即原 MSB 与现 MSB 不同），则 OF = 1，表示溢出；否则 OF = 0。
      rtl_msb(&t0, &t2, id_dest->width);
      rtl_get_CF(&t3);
      rtl_xor(&t0, &t0, &t3);
      rtl_set_OF(&t0);
    } else {
      rtl_set_OF(&tzero);
    }
  }
  print_asm_template2(shl);
}

// shr：逻辑右移，移入0
make_EHelper(shr) {
  uint32_t mask = rtl_width_mask(id_dest->width);
  rtl_andi(&t1, &id_src->val, 0x1f);
  rtl_andi(&t2, &id_dest->val, mask);
  
  if (t1 != 0) {
    // CF：最后一次最低位移出的那一位
    rtl_subi(&t0, &t1, 1);
    rtl_shr(&t0, &t2, &t0);
    rtl_andi(&t0, &t0, 1);
    rtl_set_CF(&t0);

    // count == 1 时 OF = 原操作数最高位；count > 1 时 OF 未定义，这里统一清零。
    if (t1 == 1) {
      // 如果原操作数的 MSB 是 1，则 SHR 一位后 MSB 必变 0，此时 OF = 1
      rtl_msb(&t0, &t2, id_dest->width);
      rtl_set_OF(&t0);
    } else {
      rtl_set_OF(&tzero);
    }

    // 执行移位并更新 ZF/SF
    rtl_shr(&t2, &t2, &t1);
    rtl_andi(&t2, &t2, mask);
    operand_write(id_dest, &t2);

    rtl_update_ZFSF(&t2, id_dest->width);
  }
  print_asm_template2(shr);
}

// rol：循环左移，移出的位会被重新移入到另一端
make_EHelper(rol) {
  uint32_t bits = id_dest->width * 8;                   // 操作数的位数
  uint32_t count = (id_src->val & 0x1f) % bits;         // 实际移位数，取模以避免超过位数
  uint32_t mask = rtl_width_mask(id_dest->width); 

  if (count != 0) {
    rtl_andi(&t0, &id_dest->val, mask);
    rtl_li(&t1, count);
    
    rtl_shl(&t2, &t0, &t1); // 利用 shl 逻辑左移 count 位，得到高位部分
    rtl_li(&t1, bits - count);     // 计算剩余部分的移位数
    rtl_shr(&t3, &t0, &t1); // 利用 shr 逻辑右移剩余位数，得到低位部分
    rtl_or(&t2, &t2, &t3);  // 将高位和低位部分合并，得到最终结果
    rtl_andi(&t2, &t2, mask);
    operand_write(id_dest, &t2);

    // CF：最后一次移出的那一位，现在的最低位
    rtl_andi(&t0, &t2, 1);
    rtl_set_CF(&t0);

    if (count == 1) {
      // 仅在移位次数为 1 时有效。如果符号位变了，OF 就置 1。
      rtl_msb(&t1, &t2, id_dest->width);
      rtl_xor(&t1, &t1, &t0);
      rtl_set_OF(&t1);
    } else {
      // count > 1 时 OF 未定义，这里统一清零，避免保留旧值。
      rtl_set_OF(&tzero);
    }
  }

  print_asm_template2(rol);
}

// setcc：根据条件码设置 dest 的值为 0 或 1
make_EHelper(setcc) {
  uint8_t subcode = decoding.opcode & 0xf;
  rtl_setcc(&t2, subcode);
  operand_write(id_dest, &t2);

  print_asm("set%s %s", get_cc_name(subcode), id_dest->str);
}

// not 指令是按位取反，即 dest = ~dest
make_EHelper(not) {
  // not 不影响任何标志位
  rtl_andi(&t2, &id_dest->val, rtl_width_mask(id_dest->width));
  rtl_not(&t2);
  rtl_andi(&t2, &t2, rtl_width_mask(id_dest->width));
  operand_write(id_dest, &t2);

  print_asm_template1(not);
}
