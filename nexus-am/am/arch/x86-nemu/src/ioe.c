#include <am.h>
#include <x86.h>
#include "../../../../../nemu/include/device/i8042.h"

// NEMU 自定义的 RTC 端口，读取该端口可以得到当前时间的毫秒数。
#define RTC_PORT 0x48   // Note that this is not standard
// 记录 IOE 初始化时的时间，之后 _uptime() 返回相对于这个时刻的运行时间。
static unsigned long boot_time;

// 初始化 AM 的输入输出扩展模块，目前主要记录启动时间。
void _ioe_init() {
  boot_time = inl(RTC_PORT);
}

// 返回程序从 _ioe_init() 到当前时刻经过的毫秒数。
unsigned long _uptime() {
  return inl(RTC_PORT) - boot_time;
}

// NEMU 中 VGA 显存映射的起始地址，AM 程序直接向这里写像素。
uint32_t* const fb = (uint32_t *)0x40000;

_Screen _screen = {
  .width  = 400,
  .height = 300,
}; // 屏幕的宽度和高度，供 _draw_rect() 使用

extern void* memcpy(void *, const void *, int);

// 实现 _draw_rect() 函数，将 pixels 中的像素数据绘制到屏幕上指定的位置。
// pixels 按行连续存放，颜色格式为 32 位 ARGB，与 NEMU 的 VGA 显存格式一致。
void _draw_rect(const uint32_t *pixels, int x, int y, int w, int h) {
  // src_stride 保存原始矩形的宽度。后面即使裁剪 w，也仍要按原始宽度跳到下一行。
  int src_stride = w;
  int src_x = 0, src_y = 0;

  // 宽高非法时没有任何内容需要绘制。
  if (w <= 0 || h <= 0) {
    return;
  }

  // 如果矩形左边或上边超出屏幕，就裁掉不可见部分，并调整源像素起点。
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
  // 裁剪后如果矩形为空，或者起点已经在屏幕外，直接返回。
  if (w <= 0 || h <= 0 || x >= _screen.width || y >= _screen.height) {
    return;
  }

  // 如果矩形右边或下边超出屏幕，就把绘制范围截到屏幕边界内。
  if (x + w > _screen.width) {
    w = _screen.width - x;
  }
  if (y + h > _screen.height) {
    h = _screen.height - y;
  }

  // 按行复制可见区域到帧缓冲。目标地址需要用屏幕宽度定位，源地址用原始矩形宽度定位。
  int copy_bytes = w * sizeof(uint32_t);
  const uint32_t *src = pixels + src_y * src_stride + src_x;
  for (int j = 0; j < h; j++) {
    memcpy(&fb[(y + j) * _screen.width + x], src, copy_bytes);
    src += src_stride;
  }
}

// 用于将之前的绘制内容同步到屏幕上 
// (在NEMU中绘制内容总是会同步到屏幕上, 因而无需实现此API)
void _draw_sync() {
}

// 从 NEMU 的 i8042 键盘控制器读取一个 AM 键盘事件。
// 若无按键, 则返回_KEY_NONE
int _read_key() {
  // 先读状态端口判断是否有键可读；有则从数据端口取出键值，否则返回 _KEY_NONE。
  if (inb(I8042_STATUS_PORT) & I8042_STATUS_HASKEY_MASK) {
    return inl(I8042_DATA_PORT);
  }
  return _KEY_NONE;
}
