#include "cpu/exec.h"

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

// sar、shl、shr 的实现也比较类似，都是先计算结果写回目的操作数，然后更新标志位
make_EHelper(sar) {
  uint32_t mask = rtl_width_mask(id_dest->width);
  rtl_andi(&t1, &id_src->val, 0x1f); // t1 = shift count
  rtl_andi(&t2, &id_dest->val, mask);
  
  // 1. 只有 count > 0 时才更新标志位
  if (t1 != 0) {
    // 2. 更新 CF：获取被移出的最后一位
    // 先将原数据右移 (count - 1) 位，然后取最低位
    rtl_subi(&t0, &t1, 1);
    rtl_shr(&t0, &t2, &t0);
    rtl_andi(&t0, &t0, 1);
    rtl_set_CF(&t0);

    // 3. 先按当前操作数宽度符号扩展，再执行真正的算术右移
    rtl_sext(&t2, &t2, id_dest->width);
    rtl_sar(&t2, &t2, &t1);
    rtl_andi(&t2, &t2, mask);
    operand_write(id_dest, &t2);

    // 4. 更新 ZF 和 SF
    rtl_update_ZFSF(&t2, id_dest->width);
    
    // 5. 根据手册，SAR 超过 0 位时，OF 清零
    t0 = 0;
    rtl_set_OF(&t0);
  }

  print_asm_template2(sar);
}

make_EHelper(shl) {
  uint32_t mask = rtl_width_mask(id_dest->width);
  rtl_andi(&t1, &id_src->val, 0x1f);
  rtl_andi(&t2, &id_dest->val, mask);
  if (t1 != 0) {
    // 1. 获取最后移出的一位存入 CF (你可能已实现)
    rtl_subi(&t0, &t1, 1);
    rtl_shl(&t0, &t2, &t0);
    rtl_msb(&t0, &t0, id_dest->width);
    rtl_set_CF(&t0);

    // 2. 执行实际移位
    rtl_shl(&t2, &t2, &t1);
    rtl_andi(&t2, &t2, mask);
    operand_write(id_dest, &t2);
    
    // 3. 更新 ZF/SF
    rtl_update_ZFSF(&t2, id_dest->width);

    // count == 1 时 OF = 结果最高位 ^ CF；count > 1 时 OF 未定义，这里统一清零。
    if (t1 == 1) {
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

make_EHelper(shr) {
  uint32_t mask = rtl_width_mask(id_dest->width);
  rtl_andi(&t1, &id_src->val, 0x1f);
  rtl_andi(&t2, &id_dest->val, mask);
  
  if (t1 != 0) {
    // 1. 更新 CF (保持你现有的逻辑)
    rtl_subi(&t0, &t1, 1);
    rtl_shr(&t0, &t2, &t0);
    rtl_andi(&t0, &t0, 1);
    rtl_set_CF(&t0);

    // count == 1 时 OF = 原操作数最高位；count > 1 时 OF 未定义，这里统一清零。
    if (t1 == 1) {
      rtl_msb(&t0, &t2, id_dest->width);
      rtl_set_OF(&t0);
    } else {
      rtl_set_OF(&tzero);
    }

    // 3. 执行移位并更新 ZF/SF
    rtl_shr(&t2, &t2, &t1);
    rtl_andi(&t2, &t2, mask);
    operand_write(id_dest, &t2);
    rtl_update_ZFSF(&t2, id_dest->width);
  }
  print_asm_template2(shr);
}

// rol：循环左移，类似于 shl，但移出的位会被重新移入到另一端
make_EHelper(rol) {
  uint32_t bits = id_dest->width * 8;
  uint32_t count = (id_src->val & 0x1f) % bits;
  uint32_t mask = rtl_width_mask(id_dest->width);

  if (count != 0) {
    rtl_andi(&t0, &id_dest->val, mask);
    rtl_li(&t1, count);
    rtl_shl(&t2, &t0, &t1);
    rtl_li(&t1, bits - count);
    rtl_shr(&t3, &t0, &t1);
    rtl_or(&t2, &t2, &t3);
    rtl_andi(&t2, &t2, mask);
    operand_write(id_dest, &t2);

    rtl_andi(&t0, &t2, 1);
    rtl_set_CF(&t0);

    if (count == 1) {
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

make_EHelper(setcc) {
  uint8_t subcode = decoding.opcode & 0xf;
  rtl_setcc(&t2, subcode);
  operand_write(id_dest, &t2);

  print_asm("set%s %s", get_cc_name(subcode), id_dest->str);
}

// not 指令是按位取反，即 dest = ~dest
make_EHelper(not) {
  rtl_andi(&t2, &id_dest->val, rtl_width_mask(id_dest->width));
  rtl_not(&t2);
  rtl_andi(&t2, &t2, rtl_width_mask(id_dest->width));
  operand_write(id_dest, &t2);

  print_asm_template1(not);
}
