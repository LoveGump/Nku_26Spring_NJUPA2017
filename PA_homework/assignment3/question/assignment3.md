## 问题一：

- 状态(存储)元件的特点:
  - 具有存储功能，在==时钟控制==下输入被写到电路中，直到下个时钟到达
  - 输入端状态由时钟决定何时被写入，输出端状态随时可以读出
- 定时方式:规定信号何时写入状态元件或何时从状态元件读出
  - 边沿触发(edge-triggered) 方式:
    - 状态单元中的值只在时钟边沿改变。每个时钟周期改变一次
      - 上升沿(rising edge) 触发:在时钟正跳变时进行读/写
      - 下降沿(falling edge)触发:在时钟负跳变时进行读/写。

![1](/Users/gump/course/PA平时作业/assignment3/question/fig/1.png)

Question1:为什么用时钟边缘(下降沿)做控制信号?

## 问题二：

反汇编得到的outputs汇编代码：

```asm
080483e4: push   %ebp
080483e5: mov    %esp,%ebp
080483e7: sub    $0x18,%esp
080483ea: mov    0x8(%ebp),%eax
080483ed: mov    %eax,0x4(%esp)
080483f1: lea    -0x10(%ebp),%eax
080483f4: mov    %eax,(%esp)
080483f7: call   0x8048330  <_gmon start@plt+16>
080483fc: lea    -0x10(%ebp),%eax
080483ff: mov    %eax,0x4(%esp)
08048403: movl   $0x8048500,(%esp)
0804840a: call   0x8048310 <printf@plt>
0804840f: leave
08048410: ret
```

其中，`080483ea` 到 `080483f4`是将两个`strcpy`的参数入栈，`080483fc`到`08048403`是将`printf`的两个参数入栈；

解释：`_gmon_start_`的含义

## 问题三：

### OR Immediate

- 指令格式：

```asm
ori rt, rs, imm16
```

### LOAD and STORE

- Load Word

```asm
lw rt, rs, imm16
```

- Store Word

```asm
sw rt, rs, imm16
```

### I 型指令格式

```text
31          26 25      21 20      16 15                    0
+-------------+----------+----------+-----------------------+
|     op      |    rs    |    rt    |       immediate       |
+-------------+----------+----------+-----------------------+
     6 bits      5 bits     5 bits          16 bits
```

> I 型指令格式（Immediate Format）

---

### 作业（3）：为什么 `ori` 指令是 0 	扩展，而 `load` 是符号扩展？
