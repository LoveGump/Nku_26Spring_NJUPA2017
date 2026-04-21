#include "cpu/exec.h"
#include "cpu/rtl.h"

/* shared by all helper functions */
DecodeInfo decoding;

// 临时寄寄存器
rtlreg_t t0, t1, t2, t3; 

// 0 寄存器 只能读 不能写
const rtlreg_t tzero = 0; 

// 参数分别为 eip 操作数 是否加载操作数的值
#define make_DopHelper(name) void concat(decode_op_, name) (vaddr_t *eip, Operand *op, bool load_val)

/* Refer to Appendix A in i386 manual for the explanations of these abbreviations */

/* Ib, Iv */
// 对应 指令为  
// Ib：8 bit 立即数
// Iv：16/32 bit 立即数，按照当前的宽度
// 这里的 eip 指向立即数的地址
static inline make_DopHelper(I) { 
  /* eip here is pointing to the immediate */
  op->type = OP_TYPE_IMM; // 设置 立即数类型
  // 按照宽度 读取对应的字节
  op->imm = instr_fetch(eip, op->width);
  // 将立即数的指 加载到 op->val 寄存器中
  rtl_li(&op->val, op->imm);

#ifdef DEBUG
  snprintf(op->str, OP_STR_SIZE, "$0x%x", op->imm);
#endif
}

/* I386 manual does not contain this abbreviation, but it is different from
 * the one above from the view of implementation. So we use another helper
 * function to decode it.
 */
/* sign immediate */
// si 表示有符号立即数
static inline make_DopHelper(SI) {
  assert(op->width == 1 || op->width == 4);

  op->type = OP_TYPE_IMM;

  /* TODO: Use instr_fetch() to read `op->width' bytes of memory
   * pointed by `eip'. Interpret the result as a signed immediate,
   * and assign it to op->simm.
   *
   op->simm = ???
   */

  // 使用 instr_fetch() 从内存中读取 op->width 字节的立即数
  // 
  if (op->width == 1) {
    // 这里我们使用 int8_t 来读取 1 字节的立即数，并将其符号扩展为 32 位
    // 隐式类型转换
    op->simm = (int8_t)instr_fetch(eip, 1);
  } else {
    op->simm = (int32_t)instr_fetch(eip, 4);
  }

  // 赋值给 op->simm 后，将其加载到 op->val 寄存器中
  rtl_li(&op->val, op->simm);

#ifdef DEBUG
  snprintf(op->str, OP_STR_SIZE, "$0x%x", op->simm);
#endif
}

/* I386 manual does not contain this abbreviation.
 * It is convenient to merge them into a single helper function.
 */
/* AL/eAX */
// a 对应的是 AL 或 eAX 寄存器，取决于当前的操作数宽度
static inline make_DopHelper(a) {
  op->type = OP_TYPE_REG; // 设置寄存器类型
  op->reg = R_EAX;        // 寄存器编号为 R_EAX
  if (load_val) {
    // 根据操作数宽度，从 R_EAX 寄存器中加载对应的值到 op->val 中
    rtl_lr(&op->val, R_EAX, op->width);
  }

#ifdef DEBUG
  snprintf(op->str, OP_STR_SIZE, "%%%s", reg_name(R_EAX, op->width));
#endif
}

/* This helper function is use to decode register encoded in the opcode. */
/* XX: AL, AH, BL, BH, CL, CH, DL, DH
 * eXX: eAX, eCX, eDX, eBX, eSP, eBP, eSI, eDI
 */
// 解码 寄存器编号编码在操作码中的指令
static inline make_DopHelper(r) {
  op->type = OP_TYPE_REG;
  op->reg = decoding.opcode & 0x7;  // 寄存器编号在操作码的低 3 位
  if (load_val) {
    // 加载 对应的 寄存器值到 op->val 中
    rtl_lr(&op->val, op->reg, op->width);
  }

#ifdef DEBUG
  snprintf(op->str, OP_STR_SIZE, "%%%s", reg_name(op->reg, op->width));
#endif
}

/* I386 manual does not contain this abbreviation.
 * We decode everything of modR/M byte by one time.
 */
/* Eb, Ew, Ev E：可能是寄存器，也可能是内存地址，b/w/v 分别表示操作数的宽度为 1/2/4 字节
 * Gb, Gv     G：寄存器，b/v 分别表示操作数的宽度为 1/4 字节
 * Cd,        C：控制寄存器
 * M          M：内存地址
 * Rd         R：调试寄存器
 * Sw         S：段寄存器 
 */
 // ModR/M 是 Mode Register/Memory
 // ModR/M 字节的格式如下：
  // | mod [7,6]  | Reg/Opcode [5,4,3]  | r/m [2,1,0]  |
  // mod 字段表示寻址方式，11 表示寄存器寻址， 00 01 10分别是无位移、8 位位移、32 位位移的内存寻址
  // reg/opcode 字段表示寄存器编号或扩展操作码，对应了 x86 的 8 个通用寄存器
  // r/m 字段表示寄存器编号或内存地址 和mod一起看
 // 解码 modR/M 字节编码的指令
 // eip 指向 modR/M 字节
 // rm 是 modR/M 字节中 r/m 字段编码的操作数
 // reg 是 modR/M 字节中 reg 字段编码的操作数
static inline void decode_op_rm(vaddr_t *eip, Operand *rm, bool load_rm_val, Operand *reg, bool load_reg_val) {
  read_ModR_M(eip, rm, load_rm_val, reg, load_reg_val);
}

/* Ob, Ov */
// O 表示操作数的地址在指令的后续字节中，b/v 分别表示操作数的宽度为 1/4 字节
static inline make_DopHelper(O) {
  op->type = OP_TYPE_MEM;
  op->addr = instr_fetch(eip, 4);
  if (load_val) {
    rtl_lm(&op->val, &op->addr, op->width);
  }

#ifdef DEBUG
  snprintf(op->str, OP_STR_SIZE, "0x%x", op->addr);
#endif
}

/* Eb <- Gb
 * Ev <- Gv
 */
 // G 2 E 表示 G 字段编码的寄存器操作数到 E 字段编码的寄存器或内存操作数
make_DHelper(G2E) {
  decode_op_rm(eip, id_dest, true, id_src, true);
}

// mov_G 2 E 表示将 G 字段编码的寄存器操作数的值加载到 E 字段编码的寄存器或内存操作数中
make_DHelper(mov_G2E) {
  decode_op_rm(eip, id_dest, false, id_src, true);
}

/* Gb <- Eb
 * Gv <- Ev
 */
make_DHelper(E2G) {
  decode_op_rm(eip, id_src, true, id_dest, true);
}

make_DHelper(mov_E2G) {
  decode_op_rm(eip, id_src, true, id_dest, false);
}

make_DHelper(lea_M2G) {
  decode_op_rm(eip, id_src, false, id_dest, false);
}

/* AL <- Ib
 * eAX <- Iv
 */
make_DHelper(I2a) {
  decode_op_a(eip, id_dest, true);
  decode_op_I(eip, id_src, true);
}

/* Gv <- EvIb
 * Gv <- EvIv
 * use for imul */
make_DHelper(I_E2G) {
  decode_op_rm(eip, id_src2, true, id_dest, false);
  decode_op_I(eip, id_src, true);
}

/* Eb <- Ib
 * Ev <- Iv
 */
make_DHelper(I2E) {
  decode_op_rm(eip, id_dest, true, NULL, false);
  decode_op_I(eip, id_src, true);
}

make_DHelper(mov_I2E) {
  decode_op_rm(eip, id_dest, false, NULL, false);
  decode_op_I(eip, id_src, true);
}

/* XX <- Ib
 * eXX <- Iv
 */
make_DHelper(I2r) {
  decode_op_r(eip, id_dest, true);
  decode_op_I(eip, id_src, true);
}

make_DHelper(mov_I2r) {
  decode_op_r(eip, id_dest, false);
  decode_op_I(eip, id_src, true);
}

/* used by unary operations */
make_DHelper(I) {
  decode_op_I(eip, id_dest, true);
}

make_DHelper(r) {
  decode_op_r(eip, id_dest, true);
}

make_DHelper(E) {
  decode_op_rm(eip, id_dest, true, NULL, false);
}

make_DHelper(gp7_E) {
  decode_op_rm(eip, id_dest, false, NULL, false);
}

/* used by test in group3 */
make_DHelper(test_I) {
  decode_op_I(eip, id_src, true);
}

make_DHelper(SI2E) {
  assert(id_dest->width == 2 || id_dest->width == 4);
  decode_op_rm(eip, id_dest, true, NULL, false);
  id_src->width = 1;
  decode_op_SI(eip, id_src, true);
  if (id_dest->width == 2) {
    id_src->val &= 0xffff;
  }
}

make_DHelper(SI_E2G) {
  assert(id_dest->width == 2 || id_dest->width == 4);
  decode_op_rm(eip, id_src2, true, id_dest, false);
  id_src->width = 1;
  decode_op_SI(eip, id_src, true);
  if (id_dest->width == 2) {
    id_src->val &= 0xffff;
  }
}

make_DHelper(gp2_1_E) {
  decode_op_rm(eip, id_dest, true, NULL, false);
  id_src->type = OP_TYPE_IMM;
  id_src->imm = 1;
  rtl_li(&id_src->val, 1);
#ifdef DEBUG
  sprintf(id_src->str, "$1");
#endif
}

make_DHelper(gp2_cl2E) {
  decode_op_rm(eip, id_dest, true, NULL, false);
  id_src->type = OP_TYPE_REG;
  id_src->reg = R_CL;
  rtl_lr_b(&id_src->val, R_CL);
#ifdef DEBUG
  sprintf(id_src->str, "%%cl");
#endif
}

make_DHelper(gp2_Ib2E) {
  decode_op_rm(eip, id_dest, true, NULL, false);
  id_src->width = 1;
  decode_op_I(eip, id_src, true);
}

/* Ev <- GvIb
 * use for shld/shrd */
make_DHelper(Ib_G2E) {
  decode_op_rm(eip, id_dest, true, id_src2, true);
  id_src->width = 1;
  decode_op_I(eip, id_src, true);
}

make_DHelper(O2a) {
  decode_op_O(eip, id_src, true);
  decode_op_a(eip, id_dest, false);
}

make_DHelper(a2O) {
  decode_op_a(eip, id_src, true);
  decode_op_O(eip, id_dest, false);
}

make_DHelper(J) {
  decode_op_SI(eip, id_dest, false);
  // the target address can be computed in the decode stage
  decoding.jmp_eip = id_dest->simm + *eip;
}

make_DHelper(push_SI) {
  decode_op_SI(eip, id_dest, true);
}

make_DHelper(in_I2a) {
  id_src->width = 1;
  decode_op_I(eip, id_src, true);
  decode_op_a(eip, id_dest, false);
}

make_DHelper(in_dx2a) {
  id_src->type = OP_TYPE_REG;
  id_src->reg = R_DX;
  rtl_lr_w(&id_src->val, R_DX);
#ifdef DEBUG
  sprintf(id_src->str, "(%%dx)");
#endif

  decode_op_a(eip, id_dest, false);
}

make_DHelper(out_a2I) {
  decode_op_a(eip, id_src, true);
  id_dest->width = 1;
  decode_op_I(eip, id_dest, true);
}

make_DHelper(out_a2dx) {
  decode_op_a(eip, id_src, true);

  id_dest->type = OP_TYPE_REG;
  id_dest->reg = R_DX;
  rtl_lr_w(&id_dest->val, R_DX);
#ifdef DEBUG
  sprintf(id_dest->str, "(%%dx)");
#endif
}

// 设置操作数的宽度
void operand_write(Operand *op, rtlreg_t* src) {
  if (op->type == OP_TYPE_REG) { 
    // 如果操作数类型是寄存器，则将 src 的值写入到 op->reg 寄存器中，宽度为 op->width
    rtl_sr(op->reg, op->width, src); 
  }
  else if (op->type == OP_TYPE_MEM) { 
    // 如果操作数类型是内存，则将 src 的值写入到 op->addr 指向的内存位置，宽度为 op->width
    rtl_sm(&op->addr, op->width, src); 
  }
  else { assert(0); }
}
