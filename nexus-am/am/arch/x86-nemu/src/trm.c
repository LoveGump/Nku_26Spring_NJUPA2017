#include <am.h>
#include <x86.h>

// Define this macro after serial has been implemented
#define HAS_SERIAL

#define SERIAL_PORT 0x3f8

extern char _heap_start;
extern char _heap_end;
extern int main();

// 指示堆区的起始地址和结束地址，供内存分配器使用
// 大小为 64MB
_Area _heap = {
  .start = &_heap_start,
  .end = &_heap_end,
};

static void serial_init() {
  // Initialize the serial port for output
#ifdef HAS_SERIAL
  outb(SERIAL_PORT + 1, 0x00);  
  outb(SERIAL_PORT + 3, 0x80);
  outb(SERIAL_PORT + 0, 0x01);
  outb(SERIAL_PORT + 1, 0x00);
  outb(SERIAL_PORT + 3, 0x03);
  outb(SERIAL_PORT + 2, 0xC7);
  outb(SERIAL_PORT + 4, 0x0B);
#endif
}

// 输出一个字符到串口
void _putc(char ch) {
#ifdef HAS_SERIAL
  while ((inb(SERIAL_PORT + 5) & 0x20) == 0);
  outb(SERIAL_PORT, ch);
#endif
}

// 结束从用户程序的执行，并输出退出码
void _halt(int code) {
  // 使用 x86 的 HLT 指令来停止 CPU 的执行
  // HLT 指令会使 CPU 进入低功耗状态，直到下一次外部中断发生
  // code 参数可以通过寄存器传递给外部环境，或者直接作为 HLT 指令的操作数（如果支持的话）
  asm volatile(".byte 0xd6" : :"a"(code));

  // should not reach here
  while (1);
}

void _trm_init() {
  // 在设置好栈帧之后 跳转到这里
  serial_init();
  // 进入 main 函数，开始执行用户程序
  int ret = main();
  _halt(ret);
}
