#include "cpu/exec.h"

make_EHelper(jmp) {
  // the target address is calculated at the decode stage
  decoding.is_jmp = 1;

  print_asm("jmp %x", decoding.jmp_eip);
}

make_EHelper(jcc) {
  // the target address is calculated at the decode stage
  uint8_t subcode = decoding.opcode & 0xf;
  rtl_setcc(&t2, subcode);
  decoding.is_jmp = t2;

  print_asm("j%s %x", get_cc_name(subcode), decoding.jmp_eip);
}

make_EHelper(jmp_rm) {
  decoding.jmp_eip = id_dest->val;
  decoding.is_jmp = 1;

  print_asm("jmp *%s", id_dest->str);
}

make_EHelper(call) {
  // the target address is calculated at the decode stage
  // 目标地址在 decode 阶段计算出来了，直接使用即可
  // 将下一条指令的地址压入栈中，供 ret 指令使用
  rtl_push(eip);
  decoding.is_jmp = 1; 

  print_asm("call %x", decoding.jmp_eip);
}

make_EHelper(ret) {
  // 从栈顶弹出返回地址，更新 eip
  rtl_pop(&decoding.jmp_eip);
  decoding.is_jmp = 1; 

  print_asm("ret");
}

make_EHelper(call_rm) {
  // 将下一条指令的地址压入栈中，供 ret 指令使用
  rtl_push(eip);
  decoding.jmp_eip = id_dest->val; // 目标地址在 id_dest 中，直接使用即可
  decoding.is_jmp = 1;

  print_asm("call *%s", id_dest->str);
}
