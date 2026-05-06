#include "common.h"

#define NAME(key) \
  [_KEY_##key] = #key,

static const char *keyname[256] __attribute__((used)) = {
  [_KEY_NONE] = "NONE",
  _KEYS(NAME)
};

size_t events_read(void *buf, size_t len) {
  return 0;
}

static char dispinfo[128] __attribute__((used));

void dispinfo_read(void *buf, off_t offset, size_t len) {
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

  // TODO: print the string to array `dispinfo` with the format
  // described in the Navy-apps convention
}
