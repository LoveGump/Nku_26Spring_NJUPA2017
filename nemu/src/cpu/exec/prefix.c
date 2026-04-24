#include "cpu/exec.h"

make_EHelper(real);

// exec_operand_size 将操作数修改为 16 位，并调用 exec_real 来执行指令
make_EHelper(operand_size) {
  decoding.is_operand_size_16 = true;
  exec_real(eip);
  decoding.is_operand_size_16 = false;
}
