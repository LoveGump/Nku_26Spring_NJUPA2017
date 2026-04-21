#ifndef __CPU_DECODE_H__
#define __CPU_DECODE_H__

#include "common.h"

#include "rtl.h"

// 操作数类型：寄存器、内存、立即数
enum { OP_TYPE_REG, OP_TYPE_MEM, OP_TYPE_IMM };

// str 的 长度
#define OP_STR_SIZE 40

typedef struct {
  uint32_t type;  // 操作数类型：寄存器、内存、立即数
  int width;      // 操作数宽度：1、2、4字节
  union {
    uint32_t reg;   // 寄存器编号
    rtlreg_t addr;  // 内存地址
    uint32_t imm;   // 立即数值
    int32_t simm;   // 有符号立即数
  };         
  rtlreg_t val;   // 操作数的实际值
  char str[OP_STR_SIZE];    // 操作数的字符串名字，用于调试输出
} Operand;

typedef struct {
  uint32_t opcode;            // 当前指令的操作码 
  vaddr_t seq_eip;            // sequential eip
  bool is_operand_size_16;    // 是否使用16位操作数
  uint8_t ext_opcode;         // 扩展操作码：当opcode为0x0f时，表示指令的真正操作码在后续字节中
  bool is_jmp;                // 是否是跳转指令
  vaddr_t jmp_eip;            // 跳转指令的目标地址：对于跳转指令，在解码阶段就可以计算出目标地址，存储在jmp_eip中，供执行阶段使用
  Operand src, dest, src2;
#ifdef DEBUG
  char assembly[80];          // 当前指令的汇编字符串，用于调试输出
  char asm_buf[128];          // 指令的完整汇编字符串
  char *p;                    // asm_buf的当前写入位置
#endif
} DecodeInfo;

typedef union {
  struct {
    uint8_t R_M		:3; // r/m 字段编码的寄存器编号或内存寻址方式
    uint8_t reg		:3; // reg 字段编码的寄存器编号
    uint8_t mod		:2; // mod 字段编码的寻址方式
  };
  struct {
    uint8_t dont_care	:3;// 低 3 位不关心
    uint8_t opcode		:3;// 扩展操作码：当 opcode 字段为 0 时，表示指令的真正操作码在后续字节中
  };
  uint8_t val; // 对应的8位值
} ModR_M; // modR/M 字节的格式

typedef union {
  struct {
    uint8_t base	:3; // base 字段编码的基址寄存器编号
    uint8_t index	:3; // index 字段编码的变址寄存器编号
    uint8_t ss		:2; // ss 字段编码的比例因子，00 01 10 分别表示 1、2、4
  };
  uint8_t val;
} SIB;// SIB(Scale - Index - Base) 字节的格式 有效地址为 base + index * (1 << ss) + disp

void load_addr(vaddr_t *, ModR_M *, Operand *);
void read_ModR_M(vaddr_t *, Operand *, bool, Operand *, bool);

void operand_write(Operand *, rtlreg_t *);

/* shared by all helper functions */
extern DecodeInfo decoding;

#define id_src (&decoding.src)
#define id_src2 (&decoding.src2)
#define id_dest (&decoding.dest)

// 构建 decode 辅助函数

#define make_DHelper(name) void concat(decode_, name) (vaddr_t *eip)
typedef void (*DHelper) (vaddr_t *);

make_DHelper(I2E);
make_DHelper(I2a);
make_DHelper(I2r);
make_DHelper(SI2E);
make_DHelper(SI_E2G);
make_DHelper(I_E2G);
make_DHelper(I_G2E);
make_DHelper(I);
make_DHelper(r);
make_DHelper(E);
make_DHelper(gp7_E);
make_DHelper(test_I);
make_DHelper(SI);
make_DHelper(G2E);
make_DHelper(E2G);

make_DHelper(mov_I2r);
make_DHelper(mov_I2E);
make_DHelper(mov_G2E);
make_DHelper(mov_E2G);
make_DHelper(lea_M2G);

make_DHelper(gp2_1_E);
make_DHelper(gp2_cl2E);
make_DHelper(gp2_Ib2E);

make_DHelper(O2a);
make_DHelper(a2O);

make_DHelper(J);

make_DHelper(push_SI);

make_DHelper(in_I2a);
make_DHelper(in_dx2a);
make_DHelper(out_a2I);
make_DHelper(out_a2dx);

#endif
