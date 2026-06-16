#include "common.h"
#include "device/port-io.h"

/* http://en.wikibooks.org/wiki/Serial_Programming/8250_UART_Programming */

// 0x3F8 是 x86 平台上 COM1 串口的常用基地址。
#define SERIAL_PORT 0x3F8
// 数据寄存器偏移，向该端口写入一个字节表示输出一个字符。
#define CH_OFFSET 0
// 线路状态寄存器偏移，用来表示发送缓冲区等串口状态。
#define LSR_OFFSET 5		/* line status register */

// 指向 NEMU 为串口分配的端口 I/O 映射空间。
static uint8_t *serial_port_base;

// 串口端口读写的回调函数。这里主要处理 guest 向串口写字符的情况。
void serial_io_handler(ioaddr_t addr, int len, bool is_write) {
  if (is_write) {
    assert(len == 1);
    if (addr == SERIAL_PORT + CH_OFFSET) {
      char c = serial_port_base[CH_OFFSET];
      /* We bind the serial port with the host stdout in NEMU. */
      // 在 NEMU 中，串口输出被绑定到宿主机 stdout，方便直接看到程序输出。
      putc(c, stdout);
      if (c == '\n') {
        // 遇到换行时刷新缓冲区，保证输出及时显示。
        fflush(stdout);
      }
    }
  }
}

// 初始化串口设备，并注册 8 个连续端口到端口 I/O 空间。
void init_serial() {
  serial_port_base = add_pio_map(SERIAL_PORT, 8, serial_io_handler);
  // 0x20 表示发送保持寄存器为空，因此 guest 可以认为串口一直可写。
  serial_port_base[LSR_OFFSET] = 0x20; /* the status is always free */
}
