#include "common.h"
#include <stdlib.h>
#include <unistd.h>
#include "protocol.h"

static struct gdb_conn *conn;

bool gdb_connect_qemu(void) {
  // connect to gdbserver on localhost port 1234
  // 连接到 QEMU 的 GDB 服务器，服务器默认监听在 localhost 的 1234 端口
  // 循环尝试连接，直到成功为止
  while ((conn = gdb_begin_inet("127.0.0.1", 1234)) == NULL) {
    usleep(1);
  }

  return true;
}

static bool gdb_memcpy_to_qemu_small(uint32_t dest, void *src, int len) {
  char *buf = malloc(len * 2 + 128);
  assert(buf != NULL);
  int p = sprintf(buf, "M0x%x,%x:", dest, len);
  int i;
  for (i = 0; i < len; i ++) {
    p += sprintf(buf + p, "%c%c", hex_encode(((uint8_t *)src)[i] >> 4), hex_encode(((uint8_t *)src)[i] & 0xf));
  }

  gdb_send(conn, (const uint8_t *)buf, strlen(buf));
  free(buf);

  size_t size;
  uint8_t *reply = gdb_recv(conn, &size);
  bool ok = !strcmp((const char*)reply, "OK");
  free(reply);

  return ok;
}

/*
 * 复制一块数据到 QEMU 的内存中，由于 GDB 协议中一次只能传输有限的数据，因此需要分多次传输才能完成大块数据的复制
 * @param dest 目标地址，即 QEMU 内存中的地址
 * @param src 源地址，即 NEMU 内存中的地址
 * @param len 需要复制的数据长度，单位为字节
 * @return 返回 true 表示复制成功，返回 false 表示复制失败
 * @note 该函数会调用 gdb_memcpy_to_qemu_small 来完成实际复制操作
 */
bool gdb_memcpy_to_qemu(uint32_t dest, void *src, int len) {
  // 由于 GDB 协议中一次只能传输有限的数据，因此需要分多次传输才能完成大块数据的复制
  const int mtu = 1500;
  bool ok = true;
  while (len > mtu) {
    ok &= gdb_memcpy_to_qemu_small(dest, src, mtu);
    dest += mtu;
    src += mtu;
    len -= mtu;
  }
  ok &= gdb_memcpy_to_qemu_small(dest, src, len);
  return ok;
}

// 将 regs 中的寄存器值复制到 QEMU 中
bool gdb_getregs(union gdb_regs *r) {
  // 发送 "g" 命令给 QEMU，表示请求获取寄存器的值
  gdb_send(conn, (const uint8_t *)"g", 1);
  size_t size;
  // 接收 QEMU 的回复，回复中包含了寄存器的值，按照 GDB 协议的格式进行解析，并将解析得到的寄存器值存储到 r 中
  uint8_t *reply = gdb_recv(conn, &size);

  int i;
  uint8_t *p = reply;
  uint8_t c;
  // 解析回复中的寄存器值，按照 GDB 协议的格式进行解析，并将解析得到的寄存器值存储到 r 中
  for (i = 0; i < sizeof(union gdb_regs) / sizeof(uint32_t); i ++) {
    c = p[8];
    p[8] = '\0';
    r->array[i] = gdb_decode_hex_str(p);
    p[8] = c;
    p += 8;
  }

  // 释放回复的内存，并返回 true 表示获取寄存器值成功
  free(reply);

  return true;
}

bool gdb_setregs(union gdb_regs *r) {
  int len = sizeof(union gdb_regs);
  char *buf = malloc(len * 2 + 128);
  assert(buf != NULL);
  buf[0] = 'G';

  void *src = r;
  int p = 1;
  int i;
  for (i = 0; i < len; i ++) {
    p += sprintf(buf + p, "%c%c", hex_encode(((uint8_t *)src)[i] >> 4), hex_encode(((uint8_t *)src)[i] & 0xf));
  }

  gdb_send(conn, (const uint8_t *)buf, strlen(buf));
  free(buf);

  size_t size;
  uint8_t *reply = gdb_recv(conn, &size);
  bool ok = !strcmp((const char*)reply, "OK");
  free(reply);

  return ok;
}

bool gdb_si(void) {
  char buf[] = "vCont;s:1";
  gdb_send(conn, (const uint8_t *)buf, strlen(buf));
  size_t size;
  uint8_t *reply = gdb_recv(conn, &size);
  free(reply);
  return true;
}

void gdb_exit(void) {
  gdb_end(conn);
}
