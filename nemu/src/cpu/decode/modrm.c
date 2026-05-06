#include "cpu/exec.h"
#include "cpu/rtl.h"

// 从内存中读取 modR/M 字节，并根据 modR/M 字节的内容解析出操作数的信息
void load_addr(vaddr_t *eip, ModR_M *m, Operand *rm) {
  assert(m->mod != 3);

  int32_t disp = 0;   // 位移值，默认为 0
  int disp_size = 4;  // 位移的大小，默认为 4 字节，可能会被修改为 0 或 1
  int base_reg = -1, index_reg = -1, scale = 0; 
  rtl_li(&rm->addr, 0); // 初始化 rm->addr 为 0

  if (m->R_M == R_ESP) {
    // 如果是 R_ESP 寄存器，说明存在 SIB 字节，需要进一步解析 SIB 字节来计算内存地址
    SIB s;
    // 从内存中读取 SIB 字节的值
    s.val = instr_fetch(eip, 1);
    base_reg = s.base;
    scale = s.ss;

    // 如果 index 字段编码的寄存器编号不能是 R_ESP，否则无效
    if (s.index != R_ESP) { index_reg = s.index; } 
  }
  else {
    /* no SIB */
    // 不是 R_ESP 寄存器，说明没有 SIB 字节，直接根据 r/m 字段编码的寄存器编号来计算内存地址
    base_reg = m->R_M;
  }

  if (m->mod == 0) {
    // 00 ：直接寻址，r/m 字段编码的寄存器编号直接对应一个内存地址
    if (base_reg == R_EBP) { 
      // 如果 r/m 字段编码的寄存器编号是 R_EBP
      base_reg = -1;  // 表示没有基址寄存器
      // 位移的大小为 4 字节，表示有一个 32 位的位移值
    }
    else { 
      // 否则，位移的大小为 0，表示没有位移值
      disp_size = 0; 
    }
  }
  else if (m->mod == 1) { 
    //01: 8 位位移，r/m 字段编码的寄存器编号加上一个 8 位的位移值来计算内存地址
    disp_size = 1; 
  }

  if (disp_size != 0) {
    /* has disp */
    // 有 位移值，从内存中读取位移值，并根据位移的大小进行符号扩展
    disp = instr_fetch(eip, disp_size);
    if (disp_size == 1) { disp = (int8_t)disp; }

    // 读取立即数后，将位移值添加到 rm->addr 中
    rtl_addi(&rm->addr, &rm->addr, disp);
  }

  if (base_reg != -1) {
    // 如果有基址寄存器，将基址寄存器的值添加到 rm->addr 中
    rtl_add(&rm->addr, &rm->addr, &reg_l(base_reg));
  }

  if (index_reg != -1) {
    // 如果有变址寄存器，将变址寄存器的值乘以比例因子后添加到 rm->addr 中
    rtl_shli(&t0, &reg_l(index_reg), scale);
    rtl_add(&rm->addr, &rm->addr, &t0);
  }

#ifdef DEBUG
  char disp_buf[16];
  char base_buf[8];
  char index_buf[8];

  if (disp_size != 0) {
    /* has disp */
    sprintf(disp_buf, "%s%#x", (disp < 0 ? "-" : ""), (disp < 0 ? -disp : disp));
  }
  else { disp_buf[0] = '\0'; }

  if (base_reg == -1) { base_buf[0] = '\0'; }
  else { 
    sprintf(base_buf, "%%%s", reg_name(base_reg, 4));
  }

  if (index_reg == -1) { index_buf[0] = '\0'; }
  else { 
    sprintf(index_buf, ",%%%s,%d", reg_name(index_reg, 4), 1 << scale);
  }

  if (base_reg == -1 && index_reg == -1) {
    sprintf(rm->str, "%s", disp_buf);
  }
  else {
    sprintf(rm->str, "%s(%s%s)", disp_buf, base_buf, index_buf);
  }
#endif

// 设置 rm 的类型为内存操作数
  rm->type = OP_TYPE_MEM;
}

void read_ModR_M(vaddr_t *eip, Operand *rm, bool load_rm_val, Operand *reg, bool load_reg_val) {
  ModR_M m;// modR/M 字节的格式
  m.val = instr_fetch(eip, 1); // 从内存中读取 modR/M 字节的值
  decoding.ext_opcode = m.opcode; // 将 modR/M 字节中的 opcode 字段保存到全局编码信息 decoding 中的 ext_opcode 字段，供指令解码阶段使用
  if (reg != NULL) {
    // 如果 reg 不为 NULL，说明指令中包含 reg 字段编码的寄存器操作数
    reg->type = OP_TYPE_REG;
    reg->reg = m.reg;
    if (load_reg_val) {
      rtl_lr(&reg->val, reg->reg, reg->width);
    }

#ifdef DEBUG
    snprintf(reg->str, OP_STR_SIZE, "%%%s", reg_name(reg->reg, reg->width));
#endif
  }

  // 对于 11 模式，表示寄存器寻址，r/m 字段编码的寄存器编号直接对应一个寄存器操作数
  if (m.mod == 3) {
    rm->type = OP_TYPE_REG;
    rm->reg = m.R_M;
    if (load_rm_val) {
      rtl_lr(&rm->val, m.R_M, rm->width);
    }

#ifdef DEBUG
    sprintf(rm->str, "%%%s", reg_name(m.R_M, rm->width));
#endif
  }
  else {
    // 如果是 别的 则对应 内存寻址
    load_addr(eip, &m, rm);
    if (load_rm_val) {
      rtl_lm(&rm->val, &rm->addr, rm->width);
    }
  }
}
