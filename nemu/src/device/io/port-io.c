#include "common.h"
#include "device/port-io.h"

#define PORT_IO_SPACE_MAX 65536
#define NR_MAP 8

/* "+ 3" is for hacking, see pio_read() below */
// + 3 是为了在 pio_read() 中方便地读取 1/2/4 字节的数据，避免越界访问
static uint8_t pio_space[PORT_IO_SPACE_MAX + 3];

typedef struct {
  ioaddr_t low;           // 端口地址范围的下界
  ioaddr_t high;          // 端口地址范围的上界
  pio_callback_t callback;// 读写该范围内端口时的回调函数
} PIO_t; // 

static PIO_t maps[NR_MAP];
static int nr_map = 0;

static void pio_callback(ioaddr_t addr, int len, bool is_write) {
  int i;
  for (i = 0; i < nr_map; i ++) {
    if (addr >= maps[i].low && addr + len - 1 <= maps[i].high) {
      maps[i].callback(addr, len, is_write);
      return;
    }
  }
}

/* device interface */
// 初始化的时候调用 
// 返回映射的起始地址
void* add_pio_map(ioaddr_t addr, int len, pio_callback_t callback) {
  assert(nr_map < NR_MAP);
  assert(addr + len <= PORT_IO_SPACE_MAX);
  // 将新的映射信息记录到 maps 数组中
  maps[nr_map].low = addr;
  maps[nr_map].high = addr + len - 1;
  maps[nr_map].callback = callback;
  nr_map ++;
  return pio_space + addr;
}


/* CPU interface */
// 面向 CPU 的接口，提供读写端口的函数
uint32_t pio_read(ioaddr_t addr, int len) {
  assert(len == 1 || len == 2 || len == 4);
  assert(addr + len - 1 < PORT_IO_SPACE_MAX);
  // 只有进行读写的时候彩绘调用回调函数 更新设备状态
  pio_callback(addr, len, false);		// prepare data to read
  uint32_t data = *(uint32_t *)(pio_space + addr) & (~0u >> ((4 - len) << 3));
  return data;
}

void pio_write(ioaddr_t addr, int len, uint32_t data) {
  assert(len == 1 || len == 2 || len == 4);
  assert(addr + len - 1 < PORT_IO_SPACE_MAX);
  memcpy(pio_space + addr, &data, len);
  pio_callback(addr, len, true);
}

