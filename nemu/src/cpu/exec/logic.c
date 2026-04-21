#include "cpu/exec.h"

static inline void update_logic_zfsf_cf_of(const rtlreg_t *result, int width) {
  rtl_update_ZFSF(result, width);
  rtl_set_CF(&tzero);
  rtl_set_OF(&tzero);
}

// test:按位与测试指令，计算 dest & src 的结果，但不写回目的操作数，只更新标志位
make_EHelper(test) {
  rtl_and(&t2, &id_dest->val, &id_src->val);
  update_logic_zfsf_cf_of(&t2, id_dest->width);

  print_asm_template2(test);
}

// and、xor、or 的实现比较类似，都是先计算结果写回目的操作数，然后更新标志位
make_EHelper(and) {
  rtl_and(&t2, &id_dest->val, &id_src->val);
  operand_write(id_dest, &t2);
  update_logic_zfsf_cf_of(&t2, id_dest->width);

  print_asm_template2(and);
}

make_EHelper(xor) {
  rtl_xor(&t2, &id_dest->val, &id_src->val);
  operand_write(id_dest, &t2);
  update_logic_zfsf_cf_of(&t2, id_dest->width);

  print_asm_template2(xor);
}

make_EHelper(or) {
  rtl_or(&t2, &id_dest->val, &id_src->val);
  operand_write(id_dest, &t2);
  update_logic_zfsf_cf_of(&t2, id_dest->width);

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
  rtl_andi(&t1, &id_src->val, 0x1f);
  if (t1 != 0) {
    // 1. 获取最后移出的一位存入 CF (你可能已实现)
    rtl_subi(&t0, &t1, 1);
    rtl_shl(&t0, &id_dest->val, &t0);
    rtl_msb(&t0, &t0, id_dest->width);
    rtl_set_CF(&t0);

    // 2. 执行实际移位
    rtl_shl(&t2, &id_dest->val, &t1);
    operand_write(id_dest, &t2);
    
    // 3. 更新 ZF/SF
    rtl_update_ZFSF(&t2, id_dest->width);

    // 4. 针对 DiffTest 补全 OF
    // 如果移位次数为 1，OF = 结果最高位 ^ CF
    // 为了匹配 QEMU，次数 > 1 时我们也执行此判定
    rtl_msb(&t0, &t2, id_dest->width); // 结果的 MSB
    rtl_get_CF(&t3);                   // 刚刚设置的 CF
    rtl_xor(&t0, &t0, &t3);            // 如果 MSB != CF，则 OF = 1
    rtl_set_OF(&t0);
  }
  print_asm_template2(shl);
}

make_EHelper(shr) {
  rtl_andi(&t1, &id_src->val, 0x1f);
  
  if (t1 != 0) {
    // 1. 更新 CF (保持你现有的逻辑)
    rtl_subi(&t0, &t1, 1);
    rtl_shr(&t0, &id_dest->val, &t0);
    rtl_andi(&t0, &t0, 1);
    rtl_set_CF(&t0);

    // 2. 补充 OF 更新逻辑
    // 逻辑：如果移位次数为 1，OF = 原数据的最高位 (MSB)
    if (t1 == 1) {
      rtl_msb(&t0, &id_dest->val, id_dest->width);
      rtl_set_OF(&t0);
    } else {
      // 关键：次数 > 1 时，虽然手册说 undefined，但 QEMU 有时会清零或保持
      // 针对你这次报错，可以尝试也计算一次 MSB 或直接清零
      t0 = 0;
      rtl_set_OF(&t0); 
    }

    // 3. 执行移位并更新 ZF/SF
    rtl_shr(&t2, &id_dest->val, &t1);
    operand_write(id_dest, &t2);
    rtl_update_ZFSF(&t2, id_dest->width);
  } else {
    operand_write(id_dest, &id_dest->val);
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
