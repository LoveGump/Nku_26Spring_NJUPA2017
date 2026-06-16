#include "common.h"

#ifdef HAS_IOE

#include <sys/time.h>
#include <signal.h>
#include <SDL2/SDL.h>

#define TIMER_HZ 100
#define VGA_HZ 50

// jiffy 记录虚拟定时器已经触发的次数，可用于按固定频率驱动设备。
static uint64_t jiffy = 0;
// 保存 setitimer() 使用的定时器参数，信号处理函数中会重复装载它。
static struct itimerval it;
// 信号处理函数中只设置标志位，真正的设备更新放到主循环里完成。
static int device_update_flag = false;
static int update_screen_flag = false;

void init_serial();
void init_timer();
void init_vga();
void init_i8042();

extern void timer_intr();
extern void send_key(uint8_t, bool);
extern void update_screen();


// SIGVTALRM 的处理函数：每次虚拟定时器到期时触发一次。
// 这里模拟硬件时钟中断：递增 jiffy，向 CPU 发起时钟中断，并设置设备更新标志。
static void timer_sig_handler(int signum) {
  jiffy ++;
  timer_intr();

  device_update_flag = true;
  // VGA 刷新频率低于时钟频率，因此每隔 TIMER_HZ / VGA_HZ 个 tick 刷新一次屏幕。
  if (jiffy % (TIMER_HZ / VGA_HZ) == 0) {
    update_screen_flag = true;
  }

  // 重新启动单次定时器，使下一次 SIGVTALRM 能继续到来。
  int ret = setitimer(ITIMER_VIRTUAL, &it, NULL);
  Assert(ret == 0, "Can not set timer");
}

// 在 CPU 执行主循环中被周期性调用，用来把信号处理函数设置的更新请求落到具体设备上。
void device_update() {
  if (!device_update_flag) {
    return;
  }
  device_update_flag = false;

  // 屏幕更新比较耗时，只有达到 VGA 刷新周期时才真正重绘。
  if (update_screen_flag) {
    update_screen();
    update_screen_flag = false;
  }

  // 处理 SDL 事件队列，把宿主机窗口事件转换成 NEMU 内部设备事件。
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    switch (event.type) {
      case SDL_QUIT: exit(0);

                     // 键盘按下/释放事件会被送到 i8042 键盘控制器。
      case SDL_KEYDOWN:
      case SDL_KEYUP: {
                        // 忽略长按产生的重复按键，避免一次按下被当成多次输入。
                        if (event.key.repeat == 0) {
                          uint8_t k = event.key.keysym.scancode;
                          bool is_keydown = (event.key.type == SDL_KEYDOWN);
                          send_key(k, is_keydown);
                          break;
                        }
                      }
      default: break;
    }
  }
}

// 清空 SDL 中已经积累的事件，常用于避免旧输入影响后续运行。
void sdl_clear_event_queue() {
  SDL_Event event;
  while (SDL_PollEvent(&event));
}

// 初始化串口、时钟、VGA 和键盘控制器，并启动用于驱动设备的虚拟定时器。
void init_device() {
  init_serial();
  init_timer();
  init_vga();
  init_i8042();

  // 安装 SIGVTALRM 处理函数。ITIMER_VIRTUAL 只在进程实际执行时计时，
  // 这样 NEMU 暂停或阻塞时不会继续产生虚拟时钟中断。
  struct sigaction s;
  memset(&s, 0, sizeof(s));
  s.sa_handler = timer_sig_handler;
  int ret = sigaction(SIGVTALRM, &s, NULL);
  Assert(ret == 0, "Can not set signal handler");

  // 设置第一次触发时间。这里使用单次定时器，后续在 timer_sig_handler() 中重新装载。
  it.it_value.tv_sec = 0;
  it.it_value.tv_usec = 1000000 / TIMER_HZ;
  ret = setitimer(ITIMER_VIRTUAL, &it, NULL);
  Assert(ret == 0, "Can not set timer");
}
#else

void init_device() {
}

#endif	/* HAS_IOE */
