#include "nemu.h"
#include <stdlib.h>
#include <time.h>

CPU_state cpu;

const char *regsl[] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
const char *regsw[] = {"ax", "cx", "dx", "bx", "sp", "bp", "si", "di"};
const char *regsb[] = {"al", "cl", "dl", "bl", "ah", "ch", "dh", "bh"};

// 显示寄存器的值
void isa_reg_display(void) {
  // 打印寄存器的值，包括寄存器名称、16进制值和10进制值
  int i;
  for (i = R_EAX; i <= R_EDI; i ++) {
    printf("%s\t0x%08x\t%u\n", regsl[i], reg_l(i), reg_l(i));
  }
  printf("eip\t0x%08x\t%u\n", cpu.eip, cpu.eip);
}

void reg_test() {
  // 使用随机数生成器来生成寄存器的值，并进行验证
  srand(time(0));
  uint32_t sample[8];
  // 随机生成一个eip值，并将其赋值给cpu.eip
  uint32_t eip_sample = rand();
  cpu.eip = eip_sample;

  // 给每个寄存器随机生成一个值，并将其赋值给对应的寄存器
  int i;
  for (i = R_EAX; i <= R_EDI; i ++) {
    sample[i] = rand();
    reg_l(i) = sample[i];
    assert(reg_w(i) == (sample[i] & 0xffff));
  }

  // 验证8位寄存器的值是否正确
  assert(reg_b(R_AL) == (sample[R_EAX] & 0xff));
  assert(reg_b(R_AH) == ((sample[R_EAX] >> 8) & 0xff));
  assert(reg_b(R_BL) == (sample[R_EBX] & 0xff));
  assert(reg_b(R_BH) == ((sample[R_EBX] >> 8) & 0xff));
  assert(reg_b(R_CL) == (sample[R_ECX] & 0xff));
  assert(reg_b(R_CH) == ((sample[R_ECX] >> 8) & 0xff));
  assert(reg_b(R_DL) == (sample[R_EDX] & 0xff));
  assert(reg_b(R_DH) == ((sample[R_EDX] >> 8) & 0xff));

  assert(sample[R_EAX] == cpu.eax);
  assert(sample[R_ECX] == cpu.ecx);
  assert(sample[R_EDX] == cpu.edx);
  assert(sample[R_EBX] == cpu.ebx);
  assert(sample[R_ESP] == cpu.esp);
  assert(sample[R_EBP] == cpu.ebp);
  assert(sample[R_ESI] == cpu.esi);
  assert(sample[R_EDI] == cpu.edi);

  assert(eip_sample == cpu.eip);
}
