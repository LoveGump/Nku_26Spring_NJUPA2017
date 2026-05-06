.section entry, "ax"   #定义代码段 段名 entru
.globl _start           # 将 _start 符号设为全局可见。
.type _start, @function # 将 _start 符号的类型设置为函数。

_start:
  mov $0, %ebp                  # 将基指针寄存器 %ebp 清零，表示当前没有有效的栈帧。
  mov $_stack_pointer, %esp     # 将栈指针寄存器 %esp 设置为 _stack_pointer 的地址，准备好使用栈。
  call _trm_init                 # never return
