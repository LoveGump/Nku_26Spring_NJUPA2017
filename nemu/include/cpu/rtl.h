#ifndef __RTL_H__
#define __RTL_H__

#include "nemu.h"

// RTL 寄存器
extern rtlreg_t t0, t1, t2, t3;
extern const rtlreg_t tzero;

/* RTL basic instructions */
// RTL基本指令：只使用一条机器指令来实现相应的功能

// 将立即数 imm 加载到寄存器 dest 中
static inline void rtl_li(rtlreg_t* dest, uint32_t imm) {
  *dest = imm;
}

#define c_add(a, b) ((a) + (b))
#define c_sub(a, b) ((a) - (b))
#define c_and(a, b) ((a) & (b))
#define c_or(a, b)  ((a) | (b))
#define c_xor(a, b) ((a) ^ (b))
#define c_shl(a, b) ((a) << (b))
#define c_shr(a, b) ((a) >> (b))
#define c_sar(a, b) ((int32_t)(a) >> (b))
#define c_slt(a, b) ((int32_t)(a) < (int32_t)(b))
#define c_sltu(a, b) ((a) < (b))

#define make_rtl_arith_logic(name) \
  static inline void concat(rtl_, name) (rtlreg_t* dest, const rtlreg_t* src1, const rtlreg_t* src2) { \
    *dest = concat(c_, name) (*src1, *src2); \
  } \
  static inline void concat3(rtl_, name, i) (rtlreg_t* dest, const rtlreg_t* src1, int imm) { \
    *dest = concat(c_, name) (*src1, imm); \
  }

// 算术逻辑运算 包括寄存器类型和立即数类型
make_rtl_arith_logic(add)
make_rtl_arith_logic(sub)
make_rtl_arith_logic(and)
make_rtl_arith_logic(or)
make_rtl_arith_logic(xor)
make_rtl_arith_logic(shl)
make_rtl_arith_logic(shr)
make_rtl_arith_logic(sar)
make_rtl_arith_logic(slt)
make_rtl_arith_logic(sltu)

static inline void rtl_mul(rtlreg_t* dest_hi, rtlreg_t* dest_lo, const rtlreg_t* src1, const rtlreg_t* src2) {
  asm volatile("mul %3" : "=d"(*dest_hi), "=a"(*dest_lo) : "a"(*src1), "r"(*src2));
}

static inline void rtl_imul(rtlreg_t* dest_hi, rtlreg_t* dest_lo, const rtlreg_t* src1, const rtlreg_t* src2) {
  asm volatile("imul %3" : "=d"(*dest_hi), "=a"(*dest_lo) : "a"(*src1), "r"(*src2));
}

static inline void rtl_div(rtlreg_t* q, rtlreg_t* r, const rtlreg_t* src1_hi, const rtlreg_t* src1_lo, const rtlreg_t* src2) {
  asm volatile("div %4" : "=a"(*q), "=d"(*r) : "d"(*src1_hi), "a"(*src1_lo), "r"(*src2));
}

static inline void rtl_idiv(rtlreg_t* q, rtlreg_t* r, const rtlreg_t* src1_hi, const rtlreg_t* src1_lo, const rtlreg_t* src2) {
  asm volatile("idiv %4" : "=a"(*q), "=d"(*r) : "d"(*src1_hi), "a"(*src1_lo), "r"(*src2));
}

// 内存访问指令
static inline void rtl_lm(rtlreg_t *dest, const rtlreg_t* addr, int len) {
  *dest = vaddr_read(*addr, len);
}

// 将 src1 的值存储到 addr 指向的内存位置，长度为 len 字节
static inline void rtl_sm(rtlreg_t* addr, int len, const rtlreg_t* src1) {
  vaddr_write(*addr, len, *src1);
}

// 通用寄存器访问指令
static inline void rtl_lr_b(rtlreg_t* dest, int r) {
  *dest = reg_b(r);
}

static inline void rtl_lr_w(rtlreg_t* dest, int r) {
  *dest = reg_w(r);
}

static inline void rtl_lr_l(rtlreg_t* dest, int r) {
  *dest = reg_l(r);
}

static inline void rtl_sr_b(int r, const rtlreg_t* src1) {
  reg_b(r) = *src1;
}

static inline void rtl_sr_w(int r, const rtlreg_t* src1) {
  reg_w(r) = *src1;
}

static inline void rtl_sr_l(int r, const rtlreg_t* src1) {
  reg_l(r) = *src1;
}

/* RTL psuedo instructions */
// RTL 伪指令：是通过 RTL 基本指令或者已经实现的 RTL 伪指令来实现的

// 带宽度的寄存器访问指令
static inline void rtl_lr(rtlreg_t* dest, int r, int width) {
  switch (width) {
    case 4: rtl_lr_l(dest, r); return;
    case 1: rtl_lr_b(dest, r); return;
    case 2: rtl_lr_w(dest, r); return;
    default: assert(0);
  }
}

static inline void rtl_sr(int r, int width, const rtlreg_t* src1) {
  switch (width) {
    case 4: rtl_sr_l(r, src1); return;
    case 1: rtl_sr_b(r, src1); return;
    case 2: rtl_sr_w(r, src1); return;
    default: assert(0);
  }
}

// 按位生成mask
static inline uint32_t rtl_width_mask(int width) {
  assert(width == 1 || width == 2 || width == 4);
  return width == 4 ? 0xffffffffu : ((1u << (width * 8)) - 1);
}

// 返回最高位的掩码
static inline uint32_t rtl_sign_mask(int width) {
  assert(width == 1 || width == 2 || width == 4);
  return 1u << (width * 8 - 1);
}

// EFLAGS 标志位的读写TODO(finished)
#define make_rtl_setget_eflags(f) \
  static inline void concat(rtl_set_, f) (const rtlreg_t* src) { \
    reg_f(f) = *src; \
  } \
  static inline void concat(rtl_get_, f) (rtlreg_t* dest) { \
    *dest = reg_f(f); \
  }

make_rtl_setget_eflags(CF)
make_rtl_setget_eflags(OF)
make_rtl_setget_eflags(ZF)
make_rtl_setget_eflags(SF)

static inline void rtl_mv(rtlreg_t* dest, const rtlreg_t *src1) {
  // dest <- src1
  // TODO(finfished)
  *dest = *src1;
}

static inline void rtl_not(rtlreg_t* dest) {
  // dest <- ~dest
  // TODO(finfished)
  *dest = ~(*dest);
}

// sect：根据 src1 的符号位扩展 src1 的值到 dest 中，宽度为 width 字节
static inline void rtl_sext(rtlreg_t* dest, const rtlreg_t* src1, int width) {
  // dest <- signext(src1[(width * 8 - 1) .. 0])
  // TODO(finfished)
  switch (width) {
    case 1: *dest = (int8_t)(*src1); return;
    case 2: *dest = (int16_t)(*src1); return;
    case 4: *dest = *src1; return;
    default: assert(0);
  }
}

// push ：将 src1 的值压入栈中，更新 esp 的值
void rtl_push(const rtlreg_t* src1) {
  // esp <- esp - 4
  // M[esp] <- src1
  // TODO(finfished)
  rtlreg_t esp = reg_l(R_ESP) - 4;
  reg_l(R_ESP) = esp; // 更新 esp 的值

  rtl_sm(&esp, 4, src1);// 将 src1 的值写入到 esp 指向的内存位置
}

// pop ：从栈顶弹出一个值到 dest 中，更新 esp 的值
static inline void rtl_pop(rtlreg_t* dest) {
  // dest <- M[esp]
  // esp <- esp + 4
  // TODO(finfished)
  rtlreg_t esp = reg_l(R_ESP);
  rtl_lm(dest, &esp, 4);
  reg_l(R_ESP) = esp + 4;
}

// 判断 src1 是否等于0，结果放在 dest 中
static inline void rtl_eq0(rtlreg_t* dest, const rtlreg_t* src1) {
  // dest <- (src1 == 0 ? 1 : 0)
  // TODO(finfished)
  *dest = (*src1 == 0);
}

// 判断 src1 是否等于立即数 imm
static inline void rtl_eqi(rtlreg_t* dest, const rtlreg_t* src1, int imm) {
  // dest <- (src1 == imm ? 1 : 0)
  // TODO(finfished)
  *dest = (*src1 == (rtlreg_t)imm);
}

// src1 是否不等于0
static inline void rtl_neq0(rtlreg_t* dest, const rtlreg_t* src1) {
  // dest <- (src1 != 0 ? 1 : 0)
  // TODO(finfished)
  *dest = (*src1 != 0);
}

// msb：获取 src1 的最高有效位（符号位），宽度为 width 字节
static inline void rtl_msb(rtlreg_t* dest, const rtlreg_t* src1, int width) {
  // dest <- src1[width * 8 - 1]
  // TODO(finfished)
  assert(width == 1 || width == 2 || width == 4);
  *dest = (*src1 & rtl_sign_mask(width)) ? 1 : 0; // 直接使用符号位掩码来判断最高位是否为1
}

// ZF：零标志位，表示结果是否为0
static inline void rtl_update_ZF(const rtlreg_t* result, int width) {
  // eflags.ZF <- is_zero(result[width * 8 - 1 .. 0])
  assert(width == 1 || width == 2 || width == 4);
  rtlreg_t masked = *result & rtl_width_mask(width);
  cpu.ZF = (masked == 0);
}

// SF：符号标志位，表示结果的符号
static inline void rtl_update_SF(const rtlreg_t* result, int width) {
  // eflags.SF <- is_sign(result[width * 8 - 1 .. 0])
  assert(width == 1 || width == 2 || width == 4);
  cpu.SF = (*result & rtl_sign_mask(width)) ? 1 : 0;
}

static inline void rtl_update_ZFSF(const rtlreg_t* result, int width) {
  rtl_update_ZF(result, width);
  rtl_update_SF(result, width);
}

#endif
