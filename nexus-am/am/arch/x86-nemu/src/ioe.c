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
  int i;
  for (i = 0; i < _screen.width * _screen.height; i++) {
    fb[i] = i;
  if (w <= 0 || h <= 0 || x >= _screen.width || y >= _screen.height) {
    return;
  }

  int copy_w = w;
  if (x + copy_w > _screen.width) {
    copy_w = _screen.width - x;
  }
  int copy_bytes = copy_w * sizeof(uint32_t);

  for (int j = 0; j < h && y + j < _screen.height; j++) {
    memcpy(&fb[(y + j) * _screen.width + x], pixels, copy_bytes);
    pixels += w;
  }
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
