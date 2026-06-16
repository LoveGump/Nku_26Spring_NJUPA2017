#include "common.h"

// ramdisk_start 和 ramdisk_end 由 initrd.S 提供，标记内嵌 ramdisk 镜像的起止位置。
extern uint8_t ramdisk_start;
extern uint8_t ramdisk_end;
// ramdisk 的总大小等于两个链接符号地址之差。
#define RAMDISK_SIZE ((&ramdisk_end) - (&ramdisk_start))

// Nanos-lite 是单体内核，内核可以直接访问传入的 buf，不需要额外做地址空间转换。

// 从 ramdisk 的 offset 位置读取 len 字节到 buf 中。
void ramdisk_read(void *buf, off_t offset, size_t len) {
  // 确保读取范围不会超过 ramdisk 边界。
  assert(offset + len <= RAMDISK_SIZE);
  memcpy(buf, &ramdisk_start + offset, len);
}

// 将 buf 中的 len 字节写入 ramdisk 的 offset 位置。
void ramdisk_write(const void *buf, off_t offset, size_t len) {
  // 确保写入范围不会超过 ramdisk 边界。
  assert(offset + len <= RAMDISK_SIZE);
  memcpy(&ramdisk_start + offset, buf, len);
}

// 打印 ramdisk 的起止地址和大小，便于启动时检查镜像是否正确加载。
void init_ramdisk() {
  Log("ramdisk info: start = %p, end = %p, size = %d bytes",
      &ramdisk_start, &ramdisk_end, RAMDISK_SIZE);
}

// 返回 ramdisk 的总大小，供文件系统等模块查询。
size_t get_ramdisk_size() {
  return RAMDISK_SIZE;
}
