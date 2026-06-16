#include "device/port-io.h"
#include "monitor/monitor.h"
#include <sys/time.h>

// NEMU 自定义的 RTC 端口，AM 通过读取它获得当前毫秒时间。
#define RTC_PORT 0x48   // Note that this is not the standard

// 由 device.c 中的虚拟定时器周期性调用，用来模拟硬件时钟中断。
void timer_intr() {
  if (nemu_state == NEMU_RUNNING) {
    extern void dev_raise_intr(void);
    // 只有 CPU 正在运行时才向 CPU 触发外部中断，避免暂停状态下继续推进中断。
    dev_raise_intr();
  }
}

// 指向 RTC 端口在 NEMU 端口 I/O 空间中的映射位置。
static uint32_t *rtc_port_base;

// RTC 端口的读写回调。guest 每次读取 RTC 时，NEMU 都把宿主机当前时间写入端口映射区。
void rtc_io_handler(ioaddr_t addr, int len, bool is_write) {
  if (!is_write) {
    struct timeval now;
    gettimeofday(&now, NULL);
    uint32_t seconds = now.tv_sec;
    uint32_t useconds = now.tv_usec;
    // 转换成毫秒，并对微秒部分做四舍五入，供 AM 的 _uptime() 使用。
    rtc_port_base[0] = seconds * 1000 + (useconds + 500) / 1000;
  }
}

// 注册 RTC 端口映射。读取 RTC_PORT 时会触发 rtc_io_handler() 更新数据。
void init_timer() {
  rtc_port_base = add_pio_map(RTC_PORT, 4, rtc_io_handler);
}
