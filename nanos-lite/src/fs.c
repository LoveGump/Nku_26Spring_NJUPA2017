#include "fs.h"

// 文件表项：普通文件记录 ramdisk 中的位置，设备文件则通过特殊 fd 分流处理。
typedef struct {
  char *name;        // 文件名或设备名
  size_t size;       // 文件大小，设备文件在 init_fs() 中补充
  off_t disk_offset; // 文件在 ramdisk.img 中的起始偏移
  off_t open_offset; // 当前打开后的读写位置
} Finfo;

// 前几个 fd 保留给标准输入输出和设备文件，普通 ramdisk 文件从 FD_NORMAL 之后开始。
enum {FD_STDIN, FD_STDOUT, FD_STDERR, FD_FB, FD_EVENTS, FD_DISPINFO, FD_NORMAL};

// 文件系统的全局文件表。files.h 由 Makefile 根据 navy-apps/fsimg 自动生成。
static Finfo file_table[] __attribute__((used)) = {
  {"stdin (note that this is not the actual stdin)", 0, 0},
  {"stdout (note that this is not the actual stdout)", 0, 0},
  {"stderr (note that this is not the actual stderr)", 0, 0},
  [FD_FB] = {"/dev/fb", 0, 0},
  [FD_EVENTS] = {"/dev/events", 0, 0},
  [FD_DISPINFO] = {"/proc/dispinfo", 0, 0},
#include "files.h"
};

#define NR_FILES (sizeof(file_table) / sizeof(file_table[0]))

// ramdisk 普通文件读写接口。
void ramdisk_read(void *buf, off_t offset, size_t len);
void ramdisk_write(const void *buf, off_t offset, size_t len);
// 设备文件接口：事件、显示信息和帧缓冲分别在 device.c 中实现。
size_t events_read(void *buf, size_t len);
void dispinfo_read(void *buf, off_t offset, size_t len);
size_t dispinfo_size(void);
void fb_write(const void *buf, off_t offset, size_t len);

// 根据路径查找文件表，返回表项下标作为 fd。
int fs_open(const char *pathname, int flags, int mode) {
  (void)flags;
  (void)mode;

  for (int i = 0; i < NR_FILES; i ++) {
    if (strcmp(pathname, file_table[i].name) == 0) {
      // 每次打开文件时，把读写位置重置到文件开头。
      file_table[i].open_offset = 0;
      return i;
    }
  }

  return -1;
}

// 从 fd 对应的文件或设备读取数据。
size_t fs_read(int fd, void *buf, size_t len) {
  assert(fd >= 0 && fd < NR_FILES);

  Finfo *file = &file_table[fd];
  // /dev/events 每次读取都动态生成键盘或时钟事件，不使用 open_offset。
  if (fd == FD_EVENTS) {
    return events_read(buf, len);
  }

  // 普通文件和 /proc/dispinfo 不能读过文件末尾。
  if (file->open_offset + len > file->size) {
    len = file->size - file->open_offset;
  }

  switch (fd) {
    case FD_DISPINFO:
      // /proc/dispinfo 的内容在内存字符串中，不在 ramdisk 中。
      dispinfo_read(buf, file->open_offset, len);
      break;
    default:
      // 普通文件从 ramdisk 的 disk_offset + open_offset 处读取。
      ramdisk_read(buf, file->disk_offset + file->open_offset, len);
      break;
  }

  // 成功读取后推进文件当前位置。
  file->open_offset += len;
  return len;
}

// 向 fd 对应的文件或设备写入数据。
size_t fs_write(int fd, const void *buf, size_t len) {
  assert(fd >= 0 && fd < NR_FILES);

  Finfo *file = &file_table[fd];
  switch (fd) {
    case FD_STDOUT:
    case FD_STDERR:
      // 标准输出和标准错误直接通过 AM 串口输出到 NEMU 终端。
      for (size_t i = 0; i < len; i ++) {
        _putc(((const char *)buf)[i]);
      }
      // 返回实际写入的字节数。
      return len;
  }

  // 限制写入范围，避免越过当前文件或设备大小。
  if (file->open_offset + len > file->size) {
    len = file->size - file->open_offset;
  }

  // 对于帧缓冲设备，写操作会调用 fb_write 函数将数据绘制到屏幕上；
  // 对于其他文件，则直接写入 ramdisk。
  switch (fd) {
    case FD_FB:
      fb_write(buf, file->open_offset, len);
      break;
    default:
      ramdisk_write(buf, file->disk_offset + file->open_offset, len);
      break;
  }

  // 成功写入后推进文件当前位置。
  file->open_offset += len;
  return len;
}

// 调整 fd 的当前读写位置，语义类似 POSIX lseek。
off_t fs_lseek(int fd, off_t offset, int whence) {
  assert(fd >= 0 && fd < NR_FILES);

  Finfo *file = &file_table[fd];
  off_t new_offset = 0;
  switch (whence) {
    case SEEK_SET: new_offset = offset; break;
    case SEEK_CUR: new_offset = file->open_offset + offset; break;
    case SEEK_END: new_offset = file->size + offset; break;
    default: assert(0);
  }

  // 简化实现中不允许定位到文件范围之外。
  assert(new_offset >= 0 && new_offset <= file->size);
  file->open_offset = new_offset;
  return new_offset;
}

// 当前文件系统没有分配额外打开资源，close 只检查 fd 合法性。
int fs_close(int fd) {
  assert(fd >= 0 && fd < NR_FILES);
  return 0;
}

// 返回 fd 对应文件或设备的大小。
size_t fs_filesz(int fd) {
  assert(fd >= 0 && fd < NR_FILES);
  return file_table[fd].size;
}

// 初始化设备文件大小。普通文件大小已由 files.h 静态生成。
void init_fs() {
  file_table[FD_FB].size = _screen.width * _screen.height * sizeof(uint32_t);
  file_table[FD_DISPINFO].size = dispinfo_size();
}
