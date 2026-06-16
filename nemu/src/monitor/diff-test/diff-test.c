#include "nemu.h"
#include "monitor/monitor.h"
#include <unistd.h>
#include <sys/prctl.h>
#include <signal.h>

#include "protocol.h"
#include <stdlib.h>

bool gdb_connect_qemu(void);
bool gdb_memcpy_to_qemu(uint32_t, void *, int);
bool gdb_getregs(union gdb_regs *);
bool gdb_setregs(union gdb_regs *);
bool gdb_si(void);
void gdb_exit(void);

static bool is_skip_qemu;
static bool is_skip_nemu;

void diff_test_skip_qemu() { is_skip_qemu = true; }
void diff_test_skip_nemu() { is_skip_nemu = true; }

#define regcpy_from_nemu(regs) \
  do { \
    regs.eax = cpu.eax; \
    regs.ecx = cpu.ecx; \
    regs.edx = cpu.edx; \
    regs.ebx = cpu.ebx; \
    regs.esp = cpu.esp; \
    regs.ebp = cpu.ebp; \
    regs.esi = cpu.esi; \
    regs.edi = cpu.edi; \
    regs.eip = cpu.eip; \
    regs.eflags = cpu.eflags; \
    regs.cs = cpu.cs; \
  } while (0)

static uint8_t mbr[] = {
  // start16:
  0xfa,                           // cli
  0x31, 0xc0,                     // xorw   %ax,%ax
  0x8e, 0xd8,                     // movw   %ax,%ds
  0x8e, 0xc0,                     // movw   %ax,%es
  0x8e, 0xd0,                     // movw   %ax,%ss
  0x0f, 0x01, 0x16, 0x44, 0x7c,   // lgdt   gdtdesc
  0x0f, 0x20, 0xc0,               // movl   %cr0,%eax
  0x66, 0x83, 0xc8, 0x01,         // orl    $CR0_PE,%eax
  0x0f, 0x22, 0xc0,               // movl   %eax,%cr0
  0xea, 0x1d, 0x7c, 0x08, 0x00,   // ljmp   $GDT_ENTRY(1),$start32

  // start32:
  0x66, 0xb8, 0x10, 0x00,         // movw   $0x10,%ax
  0x8e, 0xd8,                     // movw   %ax, %ds
  0x8e, 0xc0,                     // movw   %ax, %es
  0x8e, 0xd0,                     // movw   %ax, %ss
  0xeb, 0xfe,                     // jmp    7c27
  0x8d, 0x76, 0x00,               // lea    0x0(%esi),%esi

  // GDT
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xff, 0xff, 0x00, 0x00, 0x00, 0x9a, 0xcf, 0x00,
  0xff, 0xff, 0x00, 0x00, 0x00, 0x92, 0xcf, 0x00,

  // GDT descriptor
  0x17, 0x00, 0x2c, 0x7c, 0x00, 0x00
};

// 启动一个QEMU进程，并连接到它的GDB服务器
void init_difftest(void) {
  // 父进程 pid
  int ppid_before_fork = getpid();

  // 进行 fork，创建一个子进程来执行 QEMU
  int pid = fork();
  if (pid == -1) {
    perror("fork");
    panic("fork error");
  }
  else if (pid == 0) {
    // pid == 0，说明当前是子进程，执行 QEMU

    // install a parent death signal in the chlid
    // 当父进程死亡时，子进程会收到 SIGTERM 信号，从而可以及时退出，避免成为孤儿进程
    int r = prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (r == -1) {
      perror("prctl error");
      panic("prctl");
    }

    if (getppid() != ppid_before_fork) {
      // 如果父进程在 fork 后已经死亡，输出错误信息并终止程序
      panic("parent has died!");
    }

    // 子进程关闭标准输入
    close(STDIN_FILENO);
    // 执行 QEMU，使用 -S 和 -s 选项让 QEMU 在启动后暂停，并等待 GDB 连接
    execlp("qemu-system-i386", "qemu-system-i386", "-S", "-s", "-nographic", NULL);
    // 启动失败
    perror("exec");
    panic("exec error");
  }
  else {
    // father
    // 父进程连接到 QEMU 的 GDB 服务器

    gdb_connect_qemu();
    Log("Connect to QEMU successfully");

    // 注册一个退出函数，在程序结束时调用 gdb_exit 来关闭与 QEMU 的连接
    atexit(gdb_exit);

    // put the MBR code to QEMU to enable protected mode
    // 将 MBR 代码写入 QEMU 的内存中，以便进入保护模式
    bool ok = gdb_memcpy_to_qemu(0x7c00, mbr, sizeof(mbr));
    assert(ok == 1);

    // 获得 QEMU 中的寄存器状态
    union gdb_regs r; 
    gdb_getregs(&r);

    // 设置 cs:eip 到 0000:7c00，即 MBR 代码的起始地址
    // set cs:eip to 0000:7c00
    r.eip = 0x7c00;
    r.cs = 0x0000;
    ok = gdb_setregs(&r);
    assert(ok == 1);

    // 执行足够的指令以进入保护模式
    // execute enough instructions to enter protected mode
    int i;
    for (i = 0; i < 20; i ++) {
      gdb_si();
    }
  }
}

void init_qemu_reg() {
  union gdb_regs r;
  gdb_getregs(&r);
  regcpy_from_nemu(r);
  bool ok = gdb_setregs(&r);
  assert(ok == 1);
}

void difftest_step(uint32_t eip) {
  union gdb_regs r;
  bool diff = false;

  if (is_skip_nemu) {
    is_skip_nemu = false;
    return;
  }

  if (is_skip_qemu) {
    // to skip the checking of an instruction, just copy the reg state to qemu
    gdb_getregs(&r);
    regcpy_from_nemu(r);
    gdb_setregs(&r);
    is_skip_qemu = false;
    return;
  }

  gdb_si();
  gdb_getregs(&r);

  // TODO(finished): Check the registers state with QEMU.
  // Set `diff` as `true` if they are not the same.
  int i;
  // 比较通用寄存器和 EIP 的值，如果不相同则记录差异并打印日志
  for (i = 0; i < 8; i ++) {
    if (reg_l(i) != r.array[i]) {
      Log("Differ at eip = 0x%08x: %s, NEMU = 0x%08x, QEMU = 0x%08x",
          eip, regsl[i], reg_l(i), r.array[i]);
      diff = true;
    }
  }

  if (cpu.eip != r.eip) {
    Log("Differ at eip = 0x%08x: eip, NEMU = 0x%08x, QEMU = 0x%08x",
        eip, cpu.eip, r.eip);
    diff = true;
  }

  // 比较 EFLAGS 的值时，只关注部分标志位，
  // 因为某些标志位可能会因为实现细节而有所不同，例如 IOPL、NT 等
  uint32_t eflags_mask = (1u << 0) | (1u << 6) | (1u << 7) | (1u << 9) | (1u << 11);
  uint32_t nemu_eflags = cpu.eflags & eflags_mask;
  uint32_t qemu_eflags = r.eflags & eflags_mask;
  if (nemu_eflags != qemu_eflags) {
    Log("Differ at eip = 0x%08x: eflags(masked), NEMU = 0x%08x, QEMU = 0x%08x",
        eip, nemu_eflags, qemu_eflags);
    diff = true;
  }

  // 如果有差异，打印 NEMU 和 QEMU 的寄存器状态，方便调试
  if (diff) {
    Log("NEMU register state:");
    isa_reg_display();
    Log("QEMU register state:");
    for (i = 0; i < 8; i ++) {
      Log("%s\t0x%08x", regsl[i], r.array[i]);
    }
    Log("eip\t0x%08x", r.eip);
  }

  if (diff) {
    // 如果有差异，退出程序，进入调试状态
    nemu_state = NEMU_END;
  }
}
