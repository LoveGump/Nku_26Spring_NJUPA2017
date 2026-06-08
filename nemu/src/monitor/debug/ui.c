#include "monitor/monitor.h"
#include "monitor/expr.h"
#include "monitor/watchpoint.h"
#include "cpu/jit.h"
#include "nemu.h"

#include <inttypes.h>
#include <stdlib.h>
#include <readline/readline.h>
#include <readline/history.h>

void cpu_exec(uint64_t);

/* We use the `readline' library to provide more flexibility to read from stdin. */
char* rl_gets() {
  // 清空文本输入缓冲区，准备接受新的输入
  static char *line_read = NULL;
  if (line_read) {
    free(line_read);
    line_read = NULL;
  }

  // 获取用户输入的字符串指针
  line_read = readline("(nemu) ");

  if (line_read && *line_read) {
    add_history(line_read);
  }

  return line_read;
}

static int cmd_c(char *args) {
  cpu_exec(-1);
  return 0;
}

static int cmd_q(char *args) {
  return -1;
}

static int cmd_si(char *args) {
  // si [N]：单步执行N条指令，默认为1
  int n = 1;
  if (args != NULL) {
    // 将args转换为整数
    n = atoi(args);
    if (n <= 0) {
      n = 1;
    }
  }

  cpu_exec(n);
  return 0;
}

static void display_jit_stats(void) {
#ifdef CONFIG_JIT
  const JITStats *stats = jit_get_stats();
  uint64_t dynamic_instr =
    stats->direct_instr + stats->executed_instr + stats->native_executed_instr;
  double native_ratio = dynamic_instr == 0 ? 0.0 :
    100.0 * stats->native_executed_instr / dynamic_instr;
  double compile_ratio = stats->compile_attempts == 0 ? 0.0 :
    100.0 * stats->native_tbs / stats->compile_attempts;

  printf("TB lookup: hits=%" PRIu64 ", misses=%" PRIu64 ", translations=%" PRIu64 "\n",
      stats->hits, stats->misses, stats->translations);
  printf("TB compile: attempts=%" PRIu64 ", native=%" PRIu64
         ", unsupported=%" PRIu64 ", success=%.2f%%\n",
      stats->compile_attempts, stats->native_tbs,
      stats->compile_unsupported, compile_ratio);
  printf("Dynamic instructions: direct=%" PRIu64 ", cached=%" PRIu64
         ", native=%" PRIu64 ", native coverage=%.2f%%\n",
      stats->direct_instr, stats->executed_instr,
      stats->native_executed_instr, native_ratio);
  printf("Code cache: bytes=%" PRIu64 ", flushes=%" PRIu64
         ", invalidations=%" PRIu64 "\n",
      stats->code_bytes, stats->code_flushes, stats->invalidations);

  bool printed_header = false;
  bool used[256] = {};
  for (int rank = 0; rank < 5; rank ++) {
    int best = -1;
    for (int opcode = 0; opcode < 256; opcode ++) {
      if (!used[opcode] && stats->unsupported_opcode[opcode] != 0 &&
          (best < 0 || stats->unsupported_opcode[opcode] > stats->unsupported_opcode[best])) {
        best = opcode;
      }
    }
    if (best < 0) {
      break;
    }

    if (!printed_header) {
      printf("Unsupported opcodes:");
      printed_header = true;
    }
    printf(" 0x%02x=%" PRIu64, best, stats->unsupported_opcode[best]);
    used[best] = true;
  }
  if (printed_header) {
    printf("\n");
  }

  printed_header = false;
  memset(used, 0, sizeof(used));
  for (int rank = 0; rank < 5; rank ++) {
    int best = -1;
    for (int opcode = 0; opcode < 256; opcode ++) {
      if (!used[opcode] && stats->unsupported_0f_opcode[opcode] != 0 &&
          (best < 0 || stats->unsupported_0f_opcode[opcode] > stats->unsupported_0f_opcode[best])) {
        best = opcode;
      }
    }
    if (best < 0) {
      break;
    }

    if (!printed_header) {
      printf("Unsupported 0f opcodes:");
      printed_header = true;
    }
    printf(" 0f %02x=%" PRIu64, best, stats->unsupported_0f_opcode[best]);
    used[best] = true;
  }
  if (printed_header) {
    printf("\n");
  }

  printed_header = false;
  for (int nr_instr = 1; nr_instr <= JIT_MAX_TB_INSTR; nr_instr ++) {
    if (stats->unsupported_instr_count[nr_instr] == 0) {
      continue;
    }
    if (!printed_header) {
      printf("Unsupported TB length:");
      printed_header = true;
    }
    printf(" %d=%" PRIu64, nr_instr, stats->unsupported_instr_count[nr_instr]);
  }
  if (printed_header) {
    printf("\n");
  }
#else
  printf("JIT is disabled in this build.\n");
#endif
}

// info r：显示寄存器；info w：显示watchpoint；info j：显示JIT统计
static int cmd_info(char *args) {
  if (args == NULL) {
    printf("Usage: info r|w|j\n");
    return 0;
  }

  if (strcmp(args, "r") == 0) {
    isa_reg_display();
  }
  else if (strcmp(args, "w") == 0) {
    wp_display();
  }
  else if (strcmp(args, "j") == 0) {
    display_jit_stats();
  }
  else {
    printf("Unknown info command '%s'\n", args);
  }

  return 0;
}

// p EXPR：计算表达式EXPR的值并输出结果
static int cmd_p(char *args) {
  if (args == NULL) {
    printf("Usage: p EXPR\n");
    return 0;
  }

  bool success = true;
  uint32_t result = expr(args, &success);
  if (!success) {
    printf("Bad expression: %s\n", args);
    return 0;
  }

  printf("%u (0x%x)\n", result, result);
  return 0;
}

// x N EXPR：从表达式 EXPR 求出的地址开始，输出 N 个 4 字节内存单元
static int cmd_x(char *args) {
  if (args == NULL) {
    printf("Usage: x N EXPR\n");
    return 0;
  }

  char *n_str = strtok(args, " ");
  char *expr_str = strtok(NULL, "");

  if (n_str == NULL || expr_str == NULL) {
    printf("Usage: x N EXPR\n");
    return 0;
  }

  int n = atoi(n_str);
  if (n <= 0) {
    printf("N should be a positive integer\n");
    return 0;
  }

  bool success = true;
  uint32_t addr = expr(expr_str, &success);
  if (!success) {
    printf("Bad expression: %s\n", expr_str);
    return 0;
  }

  int i;
  for (i = 0; i < n; i ++) {
    uint32_t cur_addr = addr + i * 4;
    printf("0x%08x: 0x%08x\n", cur_addr, vaddr_read(cur_addr, 4));
  }

  return 0;
}

// w EXPR：添加一个新的watchpoint，监视表达式EXPR的值
// 当表达式的值发生变化时，程序会暂停执行，并显示相关信息
static int cmd_w(char *args) {
  new_watchpoint(args);
  return 0;
}

// d N：删除编号为N的watchpoint
static int cmd_d(char *args) {
  if (args == NULL) {
    printf("Usage: d N\n");
    return 0;
  }

  free_watchpoint(atoi(args));
  return 0;
}


static int cmd_help(char *args);

static struct {
  char *name;
  char *description;
  int (*handler) (char *);
} cmd_table [] = {
  { "help", "Display informations about all supported commands", cmd_help },
  { "c", "Continue the execution of the program", cmd_c },
  { "q", "Exit NEMU", cmd_q },
  { "si", "Step one or more instructions", cmd_si },
  { "info", "Print register or watchpoint information", cmd_info },
  { "p", "Evaluate an expression", cmd_p },
  { "x", "Examine memory", cmd_x },
  { "w", "Set a watchpoint", cmd_w },
  { "d", "Delete a watchpoint", cmd_d },

};

#define NR_CMD (sizeof(cmd_table) / sizeof(cmd_table[0]))

// help ：打印所有命令的帮助信息，
// help <command> ：打印特定命令的帮助信息
static int cmd_help(char *args) {
  char *arg = args;
  int i;

  if (arg == NULL) {
    /* no argument given */
    for (i = 0; i < NR_CMD; i ++) {
      printf("%s - %s\n", cmd_table[i].name, cmd_table[i].description);
    }
  }
  else {
    for (i = 0; i < NR_CMD; i ++) {
      if (strcmp(arg, cmd_table[i].name) == 0) {
        // 匹配
        printf("%s - %s\n", cmd_table[i].name, cmd_table[i].description);
        return 0;
      }
    }
    // 没有匹配
    printf("Unknown command '%s'\n", arg);
  }
  return 0;
}

void ui_mainloop(int is_batch_mode) {
  if (is_batch_mode) {
    // 批处理模式
    cmd_c(NULL);
    return;
  }

  while (1) {
    char *str = rl_gets();
    char *str_end = str + strlen(str);

    /* extract the first token as the command */
    char *cmd = strtok(str, " ");
    if (cmd == NULL) { continue; }

    /* treat the remaining string as the arguments,
     * which may need further parsing
     */
    char *args = cmd + strlen(cmd) + 1;
    if (args >= str_end) {
      args = NULL;
    }

#ifdef HAS_IOE
    extern void sdl_clear_event_queue(void);
    sdl_clear_event_queue();
#endif

    int i;
    for (i = 0; i < NR_CMD; i ++) {
      if (strcmp(cmd, cmd_table[i].name) == 0) {
        if (cmd_table[i].handler(args) < 0) { return; }
        break;
      }
    }

    if (i == NR_CMD) { printf("Unknown command '%s'\n", cmd); }
  }
}
