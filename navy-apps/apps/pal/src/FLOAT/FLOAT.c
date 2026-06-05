#include "FLOAT.h"
#include <stdint.h>
#include <assert.h>

FLOAT F_mul_F(FLOAT a, FLOAT b) {
  return ((int64_t)a * b) >> 16;
}

FLOAT F_div_F(FLOAT a, FLOAT b) {
  assert(b != 0);
  return ((int64_t)a << 16) / b;
}

FLOAT f2F(float a) {
  /* You should figure out how to convert `a' into FLOAT without
   * introducing x87 floating point instructions. Else you can
   * not run this code in NEMU before implementing x87 floating
   * point instructions, which is contrary to our expectation.
   *
   * Hint: The bit representation of `a' is already on the
   * stack. How do you retrieve it to another variable without
   * performing arithmetic operations on it directly?
   */

  // 使用 union 来访问 float 的位表示
  union {
    float f;
    uint32_t u;
  } bits;

  bits.f = a;

  uint32_t sign = bits.u >> 31;         // 获取符号位
  uint32_t exp = (bits.u >> 23) & 0xff; // 获取指数部分
  uint32_t frac = bits.u & 0x7fffff;    // 获取尾数部分

  if (exp == 0) {
    // 0 表示非规格化数或零，直接返回 0
    // 非规格化数的值非常小，转换为 FLOAT 后会被截断为 0
    return 0;
  }

  // 指数位全为 1 ：
  // 尾数为 0 表示无穷大，尾数不为 0 表示 NaN，这两种情况都无法转换为 FLOAT，直接断言失败
  assert(exp != 0xff);

  // 正常数的值为 (-1)^sign * 1.frac * 2^(exp-127)，其中 1.frac 是隐含的整数部分
  uint32_t mant = frac | (1 << 23); // 恢复隐含的整数部分，得到完整的尾数
  int shift = (int)exp - 127 - 7;   // 127 是单精度浮点数的偏移量，7 是因为 FLOAT 的尾数部分只有 16 位，而单精度浮点数的尾数部分有 23 位，所以需要右移 7 位来适配 FLOAT 的格式
  int32_t ret;

  if (shift >= 0) { // 如果 shift 大于等于 0，说明尾数需要左移来适配 FLOAT 的格式
    ret = mant << shift;
  } else if (shift > -32) { // 如果 shift 小于 0 且大于 -32，说明尾数需要右移来适配 FLOAT 的格式
    ret = mant >> -shift;
  } else {  // 如果 shift 小于等于 -32，说明尾数已经非常小，转换为 FLOAT 后会被截断为 0
    ret = 0;
  }

  // 添加符号位
  return sign ? -ret : ret;
}

// 由于 FLOAT 是一个有符号整数类型，直接判断 a 是否小于 0 来确定是否需要取反
FLOAT Fabs(FLOAT a) {
  return a < 0 ? -a : a;
}

/* Functions below are already implemented */

FLOAT Fsqrt(FLOAT x) {
  FLOAT dt, t = int2F(2);

  do {
    dt = F_div_int((F_div_F(x, t) - t), 2);
    t += dt;
  } while(Fabs(dt) > f2F(1e-4));

  return t;
}

FLOAT Fpow(FLOAT x, FLOAT y) {
  /* we only compute x^0.333 */
  FLOAT t2, dt, t = int2F(2);

  do {
    t2 = F_mul_F(t, t);
    dt = (F_div_F(x, t2) - t) / 3;
    t += dt;
  } while(Fabs(dt) > f2F(1e-4));

  return t;
}
