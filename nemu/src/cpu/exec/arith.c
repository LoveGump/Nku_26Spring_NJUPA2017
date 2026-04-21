#include "cpu/exec.h"

make_EHelper(add) {
  // 先获取mask
  uint32_t mask = rtl_width_mask(id_dest->width); 
  uint32_t dest = id_dest->val & mask;  // dest
  uint32_t src = id_src->val & mask;      // src
  uint64_t sum = (uint64_t)dest + (uint64_t)src; // 计算结果，使用64位来避免溢出

  t2 = sum & mask; // 截断结果到操作数的宽度
  // 将结果写回目的操作数
  operand_write(id_dest, &t2); 
  // 更新零标志位和符号标志位
  rtl_update_ZFSF(&t2, id_dest->width); 

  // 计算进位标志位，只有当结果超过了操作数的最大值时才会产生进位
  t0 = (sum >> (id_dest->width * 8)) & 0x1;
  rtl_set_CF(&t0);

  // 计算溢出标志位，只有当两个操作数符号相同但结果符号不同的时候才会产生溢出
  t0 = ((~(dest ^ src)) & (dest ^ t2) & rtl_sign_mask(id_dest->width)) ? 1 : 0;
  rtl_set_OF(&t0);

  print_asm_template2(add);
}

// sub 的实现和 add 类似，只不过是计算 dest - src 而不是 dest + src
make_EHelper(sub) {
  uint32_t mask = rtl_width_mask(id_dest->width);
  uint32_t dest = id_dest->val & mask;
  uint32_t src = id_src->val & mask;

  t2 = (dest - src) & mask;
  operand_write(id_dest, &t2);
  rtl_update_ZFSF(&t2, id_dest->width);

  t0 = dest < src;
  rtl_set_CF(&t0);

  t0 = (((dest ^ src) & (dest ^ t2) & rtl_sign_mask(id_dest->width)) != 0);
  rtl_set_OF(&t0);

  print_asm_template2(sub);
}

// cmp 的实现和 sub 类似，只不过不需要写回结果到目的操作数
make_EHelper(cmp) {
  uint32_t mask = rtl_width_mask(id_dest->width);
  uint32_t dest = id_dest->val & mask;
  uint32_t src = id_src->val & mask;

  t2 = (dest - src) & mask;
  rtl_update_ZFSF(&t2, id_dest->width);

  t0 = dest < src;
  rtl_set_CF(&t0);

  t0 = (((dest ^ src) & (dest ^ t2) & rtl_sign_mask(id_dest->width)) != 0);
  rtl_set_OF(&t0);

  print_asm_template2(cmp);
}

// inc 是 自增1
make_EHelper(inc) {
  uint32_t mask = rtl_width_mask(id_dest->width);
  uint32_t dest = id_dest->val & mask;

  rtl_get_CF(&t3);

  t2 = (dest + 1) & mask;
  operand_write(id_dest, &t2);
  rtl_update_ZFSF(&t2, id_dest->width);

  t0 = (((~dest) & t2 & rtl_sign_mask(id_dest->width)) != 0);
  rtl_set_OF(&t0);
  rtl_set_CF(&t3);

  print_asm_template1(inc);
}

make_EHelper(dec) {
  uint32_t mask = rtl_width_mask(id_dest->width);
  uint32_t dest = id_dest->val & mask;

  rtl_get_CF(&t3);

  t2 = (dest - 1) & mask;
  operand_write(id_dest, &t2);
  rtl_update_ZFSF(&t2, id_dest->width);

  t0 = ((dest & (~t2) & rtl_sign_mask(id_dest->width)) != 0);
  rtl_set_OF(&t0);
  rtl_set_CF(&t3);

  print_asm_template1(dec);
}

// neg 是取反加一，即 -dest = ~dest + 1
make_EHelper(neg) {
  uint32_t mask = rtl_width_mask(id_dest->width);
  uint32_t dest = id_dest->val & mask;

  t2 = ((~dest) + 1) & mask;
  operand_write(id_dest, &t2);
  rtl_update_ZFSF(&t2, id_dest->width);

  t0 = (dest != 0);
  rtl_set_CF(&t0);

  t0 = (dest == rtl_sign_mask(id_dest->width));
  rtl_set_OF(&t0);

  print_asm_template1(neg);
}

// adc 是带进位的加法，即 dest + src + CF
make_EHelper(adc) {
  rtl_add(&t2, &id_dest->val, &id_src->val);
  rtl_sltu(&t3, &t2, &id_dest->val); // 检查第一次加法是否溢出 (dest + src)

  rtl_get_CF(&t1);
  rtl_add(&t0, &t2, &t1);           // t0 = dest + src + CF_old
  
  // 检查第二次加法是否溢出 ( (dest + src) + CF_old )
  // 只有当 t2 为全 1 且 t1 为 1 时，这一步才会产生进位
  rtl_sltu(&t1, &t0, &t2); 
  
  rtl_or(&t1, &t3, &t1);            // 合并两次进位情况
  rtl_set_CF(&t1);

  // 更新结果
  operand_write(id_dest, &t0);
  rtl_update_ZFSF(&t0, id_dest->width);

  // OF 标志计算 (保持原样基本正确，但建议使用最终结果 t0 比较)
  rtl_xor(&t1, &id_dest->val, &id_src->val);
  rtl_not(&t1);
  rtl_xor(&t3, &id_dest->val, &t0);
  rtl_and(&t1, &t1, &t3);
  rtl_msb(&t1, &t1, id_dest->width);
  rtl_set_OF(&t1);

  print_asm_template2(adc);
}

// sbb 是带借位的减法，即 dest - src - CF
make_EHelper(sbb) {
  // 1. 获取旧的 CF
  rtl_get_CF(&t1); // t1 = CF_old

  // 2. 第一步减法: dest - src
  rtl_sub(&t2, &id_dest->val, &id_src->val);
  // 判定第一步是否借位: dest < src
  rtl_sltu(&t3, &id_dest->val, &id_src->val);

  // 3. 第二步减法: (dest - src) - CF_old
  rtl_sub(&t0, &t2, &t1);
  // 判定第二步是否借位: (dest - src) < CF_old
  rtl_sltu(&t1, &t2, &t1);

  // 4. 合并并设置 CF
  rtl_or(&t1, &t3, &t1);
  rtl_set_CF(&t1);

  // 5. 更新 ZF, SF 并写回结果
  operand_write(id_dest, &t0);
  rtl_update_ZFSF(&t0, id_dest->width);

  // 6. 计算 OF (减法溢出)
  // 符号不同 (dest ^ src) 且 结果符号改变 (dest ^ res)
  rtl_xor(&t1, &id_dest->val, &id_src->val);
  rtl_xor(&t3, &id_dest->val, &t0);
  rtl_and(&t1, &t1, &t3);
  rtl_msb(&t1, &t1, id_dest->width);
  rtl_set_OF(&t1);

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
      rtl_sr_l(R_EAX, &t1); // EAX 存低位
      rtl_sr_l(R_EDX, &t0); // EDX 存高位
// 1. 获取低位结果的符号扩展（全 0 或全 1）
      rtl_sari(&t2, &t1, 31); 
      
      // 2. 比较高位 (t0) 和符号扩展 (t2) 是否相等
      rtl_xor(&t3, &t0, &t2); // 如果相等，t3 为 0；如果不等，t3 非 0
      
      // 3. 利用 rtl_neq0 判定是否溢出
      rtl_neq0(&t3, &t3);     // 如果 t3 != 0，则 t3 = 1
      
      // 4. 设置溢出标志位
      rtl_set_CF(&t3);
      rtl_set_OF(&t3);

      // 5. 更新 ZF/SF 满足 DiffTest
      rtl_update_ZFSF(&t1, 4);
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

  rtl_update_ZFSF(&t1, id_dest->width);
  // 【fixed】手动清除被污染的 CF 和 OF
  // rtl_imul 内部的运算可能污染了标志位
  t0 = 0;
  rtl_set_CF(&t0);
  rtl_set_OF(&t0);

  print_asm_template2(imul);
}

// imul with three operands
make_EHelper(imul3) {
  rtl_sext(&id_src->val, &id_src->val, id_src->width);
  rtl_sext(&id_src2->val, &id_src2->val, id_src->width);
  rtl_sext(&id_dest->val, &id_dest->val, id_dest->width);

  rtl_imul(&t0, &t1, &id_src2->val, &id_src->val);
  operand_write(id_dest, &t1);

  rtl_update_ZFSF(&t1, id_dest->width);
  t0 = 0;
  rtl_set_CF(&t0);
  rtl_set_OF(&t0);
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
