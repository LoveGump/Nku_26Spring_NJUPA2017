#include "common.h"

/* Uncomment these macros to enable corresponding functionality. */
#define HAS_ASYE
//#define HAS_PTE

// 各子系统的初始化函数，由 main() 按依赖顺序调用。
void init_mm(void);
void init_ramdisk(void);
void init_device(void);
void init_irq(void);
void init_fs(void);
uint32_t loader(_Protect *, const char *);

int main() {
#ifdef HAS_PTE
  // 若启用分页机制，先初始化内存管理，为后续进程地址空间做准备。
  init_mm();
#endif

  Log("'Hello World!' from Nanos-lite");
  Log("Build time: %s, %s", __TIME__, __DATE__);

  // 初始化 ramdisk，后续文件系统会从 ramdisk 中读取程序和资源。
  init_ramdisk();

  // 初始化抽象机器提供的设备接口，如串口、时钟、键盘、VGA 等。
  init_device();

#ifdef HAS_ASYE
  Log("Initializing interrupt/exception handler...");
  // 初始化中断/异常处理，使 Nanos-lite 能响应系统调用和设备中断。
  init_irq();
#endif

  // 初始化简单文件系统，为 loader 打开用户程序提供支持。
  init_fs();

  // 从文件系统加载用户程序，entry 是用户程序的入口地址。
  uint32_t entry = loader(NULL, "/");
  // 跳转到用户程序开始执行；正常情况下不会再返回到这里。
  ((void (*)(void))entry)();

  panic("Should not reach here");
}
