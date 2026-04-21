#include "cpu/rtl.h"

/* Condition Code */

void rtl_setcc(rtlreg_t* dest, uint8_t subcode) {
  bool invert = subcode & 0x1;
  enum {
    CC_O, CC_NO, CC_B,  CC_NB,
    CC_E, CC_NE, CC_BE, CC_NBE,
    CC_S, CC_NS, CC_P,  CC_NP,
    CC_L, CC_NL, CC_LE, CC_NLE
  };

  // TODO(finish): Query EFLAGS to determine whether the condition code is satisfied.
  // 检查 subcode 的低 3 位，确定要检查哪个条件码，并根据相应的条件码来查询 EFLAGS 寄存器中的标志位，判断条件是否满足
  // dest <- ( cc is satisfied ? 1 : 0)
  switch (subcode & 0xe) {
    case CC_O:
      // 溢出标志位 OF
      rtl_get_OF(dest);
      break;
    case CC_B:
      // 进位标志位 CF
      rtl_get_CF(dest);
      break;
    case CC_E:
      // 零标志位 ZF
      rtl_get_ZF(dest);
      break;
    case CC_BE:
      // 进位标志位 CF 或者 零标志位 ZF
      rtl_get_CF(&t0);
      rtl_get_ZF(&t1);
      rtl_or(dest, &t0, &t1);
      break;
    case CC_S:
      // 符号标志位 SF
      rtl_get_SF(dest);
      break;
    case CC_L:
      // 符号标志位 SF 和 溢出标志位 OF 不相等
      rtl_get_SF(&t0);
      rtl_get_OF(&t1);
      rtl_xor(dest, &t0, &t1);
      break;
    case CC_LE:
      // 符号标志位 SF 和 溢出标志位 OF 不相等，或者 零标志位 ZF
      rtl_get_SF(&t0);
      rtl_get_OF(&t1);
      rtl_xor(&t0, &t0, &t1);
      rtl_get_ZF(&t1);
      rtl_or(dest, &t0, &t1);
      break;
    default: panic("should not reach here");
    case CC_P: panic("n86 does not have PF");
  }

  if (invert) {
    rtl_xori(dest, dest, 0x1);
  }
}
