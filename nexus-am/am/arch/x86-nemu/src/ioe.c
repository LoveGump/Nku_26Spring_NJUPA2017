#include <am.h>
#include <x86.h>
#include "../../../../../nemu/include/device/i8042.h"

#define RTC_PORT 0x48   // Note that this is not standard
static unsigned long boot_time;

void _ioe_init() {
  boot_time = inl(RTC_PORT);
}

unsigned long _uptime() {
  return inl(RTC_PORT) - boot_time;
}

uint32_t* const fb = (uint32_t *)0x40000;

_Screen _screen = {
  .width  = 400,
  .height = 300,
}; // 屏幕的宽度和高度，供 _draw_rect() 使用

extern void* memcpy(void *, const void *, int);

// 实现 _draw_rect() 函数，将 pixels 中的像素数据绘制到屏幕上指定的位置
void _draw_rect(const uint32_t *pixels, int x, int y, int w, int h) {
  int src_stride = w;
  int src_x = 0, src_y = 0;

  if (w <= 0 || h <= 0) {
    return;
  }

  if (x < 0) {
    src_x = -x;
    w += x;
    x = 0;
  }
  if (y < 0) {
    src_y = -y;
    h += y;
    y = 0;
  }
  if (w <= 0 || h <= 0 || x >= _screen.width || y >= _screen.height) {
    return;
  }

  if (x + w > _screen.width) {
    w = _screen.width - x;
  }
  if (y + h > _screen.height) {
    h = _screen.height - y;
  }

  int copy_bytes = w * sizeof(uint32_t);
  const uint32_t *src = pixels + src_y * src_stride + src_x;
  for (int j = 0; j < h; j++) {
    memcpy(&fb[(y + j) * _screen.width + x], src, copy_bytes);
    src += src_stride;
  }
}

// 用于将之前的绘制内容同步到屏幕上 (在NEMU中绘制内容总是会同步到屏幕上, 因而无需实现此API)
void _draw_sync() {
}

int _read_key() {
  // 读取键盘输入，如果有键可读则返回键值，否则返回 _KEY_NONE
  if (inb(I8042_STATUS_PORT) & I8042_STATUS_HASKEY_MASK) {
    return inl(I8042_DATA_PORT);
  }
  return _KEY_NONE;
}
