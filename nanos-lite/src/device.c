#include "common.h"

#define NAME(key) \
  [_KEY_##key] = #key,

static const char *keyname[256] __attribute__((used)) = {
  [_KEY_NONE] = "NONE",
  _KEYS(NAME)
};

#define KEYDOWN_MASK 0x8000

void switch_game(void);

size_t events_read(void *buf, size_t len) {
  char event[64];
  int key = _read_key();

  if (key != _KEY_NONE) {
    bool down = (key & KEYDOWN_MASK) != 0;
    key &= ~KEYDOWN_MASK;
    assert(key > _KEY_NONE && key < 256 && keyname[key] != NULL);
    if (down && key == _KEY_F12) {
      switch_game();
    }
    snprintf(event, sizeof(event), "%s %s\n",
        down ? "kd" : "ku", keyname[key]);
  } else {
    snprintf(event, sizeof(event), "t %u\n", (unsigned)_uptime());
  }

  size_t event_len = strlen(event);
  if (event_len > len) {
    event_len = len;
  }
  memcpy(buf, event, event_len);
  return event_len;
}

static char dispinfo[128] __attribute__((used));

void dispinfo_read(void *buf, off_t offset, size_t len) {
  assert(offset >= 0);
  assert((size_t)offset + len <= strlen(dispinfo));
  memcpy(buf, dispinfo + offset, len);
}

size_t dispinfo_size(void) {
  return strlen(dispinfo);
}

void fb_write(const void *buf, off_t offset, size_t len) {
  assert(offset % sizeof(uint32_t) == 0);
  assert(len % sizeof(uint32_t) == 0);

  const uint32_t *pixels = (const uint32_t *)buf;
  size_t pixel_offset = offset / sizeof(uint32_t);
  size_t pixels_left = len / sizeof(uint32_t);

  while (pixels_left > 0) {
    int x = pixel_offset % _screen.width;
    int y = pixel_offset / _screen.width;
    size_t row_pixels = _screen.width - x;
    if (row_pixels > pixels_left) {
      row_pixels = pixels_left;
    }

    _draw_rect(pixels, x, y, row_pixels, 1);

    pixels += row_pixels;
    pixel_offset += row_pixels;
    pixels_left -= row_pixels;
  }
}

void init_device() {
  _ioe_init();

  snprintf(dispinfo, sizeof(dispinfo), "WIDTH:%d\nHEIGHT:%d\n",
      _screen.width, _screen.height);
}
