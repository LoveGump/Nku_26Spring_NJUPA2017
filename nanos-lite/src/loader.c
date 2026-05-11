#include "fs.h"
#include "memory.h"

#define DEFAULT_ENTRY ((void *)0x8048000)

uintptr_t loader(_Protect *as, const char *filename) {
  if (filename == NULL) {
    filename = "/bin/hello";
  }

  int fd = fs_open(filename, 0, 0);
  size_t size = fs_filesz(fd);

  if (as == NULL) {
    fs_read(fd, DEFAULT_ENTRY, size);
  }
  else {
    // 将文件内容加载到内存中，并建立虚拟地址和物理地址的映射关系
    uintptr_t va = (uintptr_t)DEFAULT_ENTRY;
    for (size_t offset = 0; offset < size; offset += PGSIZE) {
      void *pa = new_page();
      size_t len = size - offset;
      if (len > PGSIZE) {
        len = PGSIZE;
      }

      _map(as, (void *)(va + offset), pa);
      fs_read(fd, pa, len);
    }
  }

  fs_close(fd);
  return (uintptr_t)DEFAULT_ENTRY;
}
