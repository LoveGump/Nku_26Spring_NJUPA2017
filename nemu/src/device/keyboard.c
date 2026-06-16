#include "device/port-io.h"
#include "device/i8042.h"
#include "monitor/monitor.h"
#include <SDL2/SDL.h>

#define KEYBOARD_IRQ 1

// i8042 的数据端口和状态端口在 NEMU 端口 I/O 空间中的映射地址。
static uint32_t *i8042_data_port_base;
static uint8_t *i8042_status_port_base;

// 列出 NEMU/AM 支持的键名，用宏统一生成枚举和映射表。
#define _KEYS(_) \
  _(ESCAPE) _(F1) _(F2) _(F3) _(F4) _(F5) _(F6) _(F7) _(F8) _(F9) _(F10) _(F11) _(F12) \
_(GRAVE) _(1) _(2) _(3) _(4) _(5) _(6) _(7) _(8) _(9) _(0) _(MINUS) _(EQUALS) _(BACKSPACE) \
_(TAB) _(Q) _(W) _(E) _(R) _(T) _(Y) _(U) _(I) _(O) _(P) _(LEFTBRACKET) _(RIGHTBRACKET) _(BACKSLASH) \
_(CAPSLOCK) _(A) _(S) _(D) _(F) _(G) _(H) _(J) _(K) _(L) _(SEMICOLON) _(APOSTROPHE) _(RETURN) \
_(LSHIFT) _(Z) _(X) _(C) _(V) _(B) _(N) _(M) _(COMMA) _(PERIOD) _(SLASH) _(RSHIFT) \
_(LCTRL) _(APPLICATION) _(LALT) _(SPACE) _(RALT) _(RCTRL) \
_(UP) _(DOWN) _(LEFT) _(RIGHT) _(INSERT) _(DELETE) _(HOME) _(END) _(PAGEUP) _(PAGEDOWN)

#define _KEY_NAME(k) _KEY_##k,

enum {
  _KEY_NONE = 0,
  _KEYS(_KEY_NAME)
};

#define XX(k) [concat(SDL_SCANCODE_, k)] = concat(_KEY_, k),
// 将 SDL 的 scancode 转换成 AM 使用的键码。
static uint32_t keymap[256] = {
  _KEYS(XX)
  [SDL_SCANCODE_KP_6] = _KEY_RIGHT,
};

// 简单的环形队列，用来缓存 SDL 事件转换得到的键盘事件。
#define KEY_QUEUE_LEN 1024
static int key_queue[KEY_QUEUE_LEN];
static int key_f = 0, key_r = 0;

// AM 键盘事件中最高位表示按下；没有该位则表示释放。
#define KEYDOWN_MASK 0x8000

// 由 device_update() 调用，把 SDL 键盘事件转换成 AM 键盘事件并放入队列。
void send_key(uint8_t scancode, bool is_keydown) {
  if (nemu_state == NEMU_RUNNING &&
      keymap[scancode] != _KEY_NONE) {
    uint32_t am_scancode = keymap[scancode] | (is_keydown ? KEYDOWN_MASK : 0);
    key_queue[key_r] = am_scancode;
    key_r = (key_r + 1) % KEY_QUEUE_LEN;
  }
}

// i8042 端口回调：guest 读取状态端口时准备数据，读取数据端口后清除 has-key 标志。
void i8042_io_handler(ioaddr_t addr, int len, bool is_write) {
  if (!is_write) {
    if (addr == I8042_DATA_PORT) {
      // 数据已被 guest 读取，清除“有按键可读”的状态位。
      i8042_status_port_base[0] &= ~I8042_STATUS_HASKEY_MASK;
    }
    else if (addr == I8042_STATUS_PORT) {
      if ((i8042_status_port_base[0] & I8042_STATUS_HASKEY_MASK) == 0) {
        if (key_f != key_r) {
          // 若队列中有新事件，把它放到数据端口，并设置状态位通知 guest。
          i8042_data_port_base[0] = key_queue[key_f];
          i8042_status_port_base[0] |= I8042_STATUS_HASKEY_MASK;
          key_f = (key_f + 1) % KEY_QUEUE_LEN;
        }
      }
    }
  }
}

// 注册 i8042 的数据端口和状态端口，并初始化状态为空。
void init_i8042() {
  i8042_data_port_base = add_pio_map(I8042_DATA_PORT, 4, i8042_io_handler);
  i8042_status_port_base = add_pio_map(I8042_STATUS_PORT, 1, i8042_io_handler);
  i8042_status_port_base[0] = 0x0;
}
