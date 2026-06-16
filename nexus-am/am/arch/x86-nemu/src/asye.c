#include <am.h>
#include <x86.h>

static _RegSet* (*H)(_Event, _RegSet*) = NULL;

void vecsys();
void vecnull();

_RegSet* irq_handle(_RegSet *tf) {
  // 默认情况下直接返回原始 trap frame。
  _RegSet *next = tf;
  if (H) {
    _Event ev;
    // 根据中断号构造事件，目前只将 0x80 识别为系统调用。
    switch (tf->irq) {
      case 0x80: ev.event = _EVENT_SYSCALL; break;
      default: ev.event = _EVENT_ERROR; break;
    }

    // 交给上层事件处理函数处理，并允许其返回新的寄存器现场。
    next = H(ev, tf);
    if (next == NULL) {
      next = tf;
    }
  }

  return next;
}

static GateDesc idt[NR_IRQ];

void _asye_init(_RegSet*(*h)(_Event, _RegSet*)) {
  // 初始化 IDT，默认所有中断都进入空处理入口。
  for (unsigned int i = 0; i < NR_IRQ; i ++) {
    idt[i] = GATE(STS_TG32, KSEL(SEG_KCODE), vecnull, DPL_KERN);
  }

  // -------------------- 系统调用入口 --------------------------
  // 0x80 作为用户态可访问的系统调用中断。
  idt[0x80] = GATE(STS_TG32, KSEL(SEG_KCODE), vecsys, DPL_USER);

  // 加载 IDT。
  set_idt(idt, sizeof(idt));

  // 注册上层事件处理函数。
  H = h;
}

_RegSet *_make(_Area stack, void *entry, void *arg) {
  // x86-nemu 下暂未实现线程/进程上下文创建。
  return NULL;
}

void _trap() {
  // 保留接口，具体 trap 处理由中断入口完成。
}

int _istatus(int enable) {
  // 暂不维护中断开关状态，直接返回占位值。
  return 0;
}
