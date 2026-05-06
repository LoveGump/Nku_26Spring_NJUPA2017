#include "fs.h"

typedef struct {
  char *name;
  size_t size;
  off_t disk_offset;
  off_t open_offset;
} Finfo;

enum {FD_STDIN, FD_STDOUT, FD_STDERR, FD_FB, FD_EVENTS, FD_DISPINFO, FD_NORMAL};

/* This is the information about all files in disk. */
static Finfo file_table[] __attribute__((used)) = {
  {"stdin (note that this is not the actual stdin)", 0, 0},
  {"stdout (note that this is not the actual stdout)", 0, 0},
  {"stderr (note that this is not the actual stderr)", 0, 0},
  [FD_FB] = {"/dev/fb", 0, 0},
  [FD_EVENTS] = {"/dev/events", 0, 0},
  [FD_DISPINFO] = {"/proc/dispinfo", 128, 0},
#include "files.h"
};

#define NR_FILES (sizeof(file_table) / sizeof(file_table[0]))

void ramdisk_read(void *buf, off_t offset, size_t len);

int fs_open(const char *pathname, int flags, int mode) {
  (void)flags;
  (void)mode;

  for (int i = 0; i < NR_FILES; i ++) {
    if (strcmp(pathname, file_table[i].name) == 0) {
      file_table[i].open_offset = 0;
      return i;
    }
  }

  assert(0);
  return -1;
}

size_t fs_read(int fd, void *buf, size_t len) {
  assert(fd >= 0 && fd < NR_FILES);

  Finfo *file = &file_table[fd];
  if (file->open_offset + len > file->size) {
    len = file->size - file->open_offset;
  }

  ramdisk_read(buf, file->disk_offset + file->open_offset, len);
  file->open_offset += len;
  return len;
}

int fs_close(int fd) {
  assert(fd >= 0 && fd < NR_FILES);
  return 0;
}

size_t fs_filesz(int fd) {
  assert(fd >= 0 && fd < NR_FILES);
  return file_table[fd].size;
}

void init_fs() {
  // TODO: initialize the size of /dev/fb
}
