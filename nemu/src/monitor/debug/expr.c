#include "nemu.h"

/* We use the POSIX regex functions to process regular expressions.
 * Type 'man regex' for more information about POSIX regex functions.
 */
#include <stdlib.h>
#include <sys/types.h>
#include <regex.h>

enum {
  TK_NOTYPE = 256, TK_EQ,

  /* TODO(finished): Add more token types */
  TK_NUM, // 十进制数字
  TK_HEX, // 十六进制数字
  TK_REG, // 寄存器
  TK_NEQ, // 不等于
  TK_AND, // 逻辑与
  TK_OR, // 逻辑或
  TK_NOT, // 逻辑非
  TK_DEREF, // 解引用
  TK_NEG // 负号

};

static struct rule {
  char *regex;
  int token_type;
} rules[] = {

    /* TODO(finished): Add more rules.
   * Pay attention to the precedence level of different rules.
   */
  {" +", TK_NOTYPE},              // spaces
  {"\\+", '+'},                   // plus
  {"-", '-'},                     // minus
  {"\\*", '*'},                   
  {"/", '/'},                     
  {"\\(", '('},                  
  {"\\)", ')'},                   
  {"==", TK_EQ},                 
  {"!=", TK_NEQ},                
  {"&&", TK_AND},                
  {"\\|\\|", TK_OR},
  {"!", TK_NOT},
  {"0[xX][0-9a-fA-F]+", TK_HEX},  // hexadecimal number
  {"[0-9]+", TK_NUM},             // decimal number
  {"\\$[a-zA-Z][a-zA-Z0-9]*", TK_REG}  // register
};

#define NR_REGEX (sizeof(rules) / sizeof(rules[0]) )

static regex_t re[NR_REGEX];

/* Rules are used for many times.
 * Therefore we compile them only once before any usage.
 */
void init_regex() {
  int i;
  char error_msg[128];
  int ret;

  for (i = 0; i < NR_REGEX; i ++) {
    ret = regcomp(&re[i], rules[i].regex, REG_EXTENDED);
    if (ret != 0) {
      regerror(ret, &re[i], error_msg, 128);
      panic("regex compilation failed: %s\n%s", error_msg, rules[i].regex);
    }
  }
}

typedef struct token {
  int type;
  char str[32];
} Token;

Token tokens[32];
int nr_token;

static bool make_token(char *e) {
  int position = 0;
  int i;
  regmatch_t pmatch;

  nr_token = 0;

  while (e[position] != '\0') {
    /* Try all rules one by one. */
    for (i = 0; i < NR_REGEX; i ++) {
      if (regexec(&re[i], e + position, 1, &pmatch, 0) == 0 && pmatch.rm_so == 0) {
        char *substr_start = e + position;
        int substr_len = pmatch.rm_eo;

        Log("match rules[%d] = \"%s\" at position %d with len %d: %.*s",
            i, rules[i].regex, position, substr_len, substr_len, substr_start);
        position += substr_len;

        /* TODO: Now a new token is recognized with rules[i]. Add codes
         * to record the token in the array `tokens'. For certain types
         * of tokens, some extra actions should be performed.
         */

        switch (rules[i].token_type) {
          case TK_NOTYPE:
            // 忽略空格，不记录到tokens数组中
            break;

          case TK_NUM:
          case TK_HEX:
          case TK_REG:
            // token 数量不能超过32
            Assert(nr_token < 32, "too many tokens");
            // 记录token类型和字符串内容
            tokens[nr_token].type = rules[i].token_type;
            Assert(substr_len < (int)sizeof(tokens[nr_token].str), "token is too long");
            strncpy(tokens[nr_token].str, substr_start, substr_len);
            // 添加字符串结束符
            tokens[nr_token].str[substr_len] = '\0';
            nr_token ++;
            break;

          default:
            // 剩下的符号类token，直接记录类型，不需要字符串内容
            Assert(nr_token < 32, "too many tokens");
            // 记录token类型，字符串内容置空
            tokens[nr_token].type = rules[i].token_type;
            tokens[nr_token].str[0] = '\0';
            nr_token ++;
            break;
        }

        break;
      }
    }

    if (i == NR_REGEX) {
      printf("no match at position %d\n%s\n%*.s^\n", position, e, position, "");
      return false;
    }
  }

  // 处理一元运算符：解引用和负号
  int j;
  for (j = 0; j < nr_token; j ++) {
    // 如果当前token是*，并且它是第一个token，或者前一个token不是数字、寄存器或右括号，那么它就是解引用符
    if (tokens[j].type == '*' &&
        (j == 0 ||
         (tokens[j - 1].type != TK_NUM &&
          tokens[j - 1].type != TK_HEX &&
          tokens[j - 1].type != TK_REG &&
          tokens[j - 1].type != ')'))) {
      tokens[j].type = TK_DEREF;
    }

    // 如果当前token是-，并且它是第一个token，或者前一个token不是数字、寄存器或右括号，那么它就是负号
    if (tokens[j].type == '-' &&
        (j == 0 ||
         (tokens[j - 1].type != TK_NUM &&
          tokens[j - 1].type != TK_HEX &&
          tokens[j - 1].type != TK_REG &&
          tokens[j - 1].type != ')'))) {
      tokens[j].type = TK_NEG;
    }
  }

  return true;
}
/**
 * 获取寄存器的值
 * @param s The string representing the register.
 * @param success A pointer to a boolean indicating whether the operation was successful.
 * @return The value of the register.
 */
static uint32_t reg_value(const char *s, bool *success) {
  int i;
  // 去掉$
  const char *name = s[0] == '$' ? s + 1 : s;

  // 处理 eip 寄存器
  if (strcmp(name, "eip") == 0) {
    *success = true;
    return cpu.eip;
  }

  // 处理通用寄存器
  for (i = 0; i < 8; i ++) {
    if (strcmp(name, regsl[i]) == 0) {
      *success = true;
      return reg_l(i);
    }
    if (strcmp(name, regsw[i]) == 0) {
      *success = true;
      return reg_w(i);
    }
    if (strcmp(name, regsb[i]) == 0) {
      *success = true;
      return reg_b(i);
    }
  }

  // 没有找到匹配的寄存器
  *success = false;
  return 0;
}

// 判断tokens[p..q]是否被一对匹配的括号包围
static bool check_parentheses(int p, int q) {
  if (tokens[p].type != '(' || tokens[q].type != ')') {
    return false;
  }

  // 如果被括号包围，检查括号是否匹配
  int level = 0;
  int i;
  for (i = p; i <= q; i ++) {
    if (tokens[i].type == '(') {
      level ++;
    }
    else if (tokens[i].type == ')') {
      level --;
      if (level == 0 && i < q) {
        return false;
      }
      if (level < 0) {
        return false;
      }
    }
  }

  return level == 0;
}

// 返回运算符的优先级，数字越大优先级越高
static int precedence(int type) {
  switch (type) {
    case TK_OR: return 1;
    case TK_AND: return 2;
    case TK_EQ:
    case TK_NEQ: return 3;
    case '+':
    case '-': return 4;
    case '*':
    case '/': return 5;
    case TK_NOT:
    case TK_NEG:
    case TK_DEREF: return 6;
    default: return 0;
  }
}

// 找到tokens[p..q]中优先级最低的运算符的位置
static int dominant_operator(int p, int q) {
  int op = -1;        // 最低优先级位置
  int min_pri = 100; 
  int level = 0;      // 括号层数
  int i;

  // 扫描寻找
  for (i = p; i <= q; i ++) {
    int type = tokens[i].type;

    if (type == '(') {
      level ++;
      continue;
    }
    if (type == ')') {
      level --;
      continue;
    }
    if (level != 0) {
      // 在括号内的运算符不考虑
      continue;
    }

    if (type == TK_NUM || type == TK_HEX || type == TK_REG) {
      // 数字和寄存器不是运算符，跳过
      continue;
    }

    if (precedence(type) > 0 && precedence(type) <= min_pri) {
      // 找到更低优先级的运算符，更新结果
      min_pri = precedence(type);
      op = i;
    }
  }

  return op;
}

// 递归计算tokens[p..q]表达式的值
static uint32_t eval(int p, int q, bool *success) {
  // 表达式无效
  if (p > q) {
    *success = false;
    return 0;
  }

  // 表达式只有一个token
  if (p == q) {
    switch (tokens[p].type) {
      // 数字和寄存器
      case TK_NUM:
        *success = true;
        return strtoul(tokens[p].str, NULL, 10);
      case TK_HEX:
        *success = true;
        return strtoul(tokens[p].str, NULL, 16);
      case TK_REG:
        return reg_value(tokens[p].str, success);
      default:
        // 其他单个token不是有效的表达式
        *success = false;
        return 0;
    }
  }

  if (check_parentheses(p, q)) {
    // 检查到被括号包围，去掉括号继续计算
    return eval(p + 1, q - 1, success);
  }

  // 找到主运算符
  int op = dominant_operator(p, q);
  if (op == -1) {
    // 没有找到主运算符，表达式无效
    *success = false;
    return 0;
  }

  // 处理一元运算符：逻辑非、解引用和负号
  if (tokens[op].type == TK_NOT || tokens[op].type == TK_NEG || tokens[op].type == TK_DEREF) {
    // 计算右侧表达式的值
    uint32_t val = eval(op + 1, q, success);
    if (!*success) {
      return 0;
    }
    // 逻辑非
    if (tokens[op].type == TK_NOT) {
      return !val;
    }
    // 负号
    if (tokens[op].type == TK_NEG) {
      return -val;
    }
    // 解引用
    return vaddr_read(val, 4);
  }

  // 计算主运算符左侧表达式的值
  uint32_t val1 = eval(p, op - 1, success);
  if (!*success) {
    return 0;
  }

  // 计算主运算符右侧表达式的值
  uint32_t val2 = eval(op + 1, q, success);
  if (!*success) {
    return 0;
  }

  // 根据主运算符类型计算结果
  switch (tokens[op].type) {
    case '+': return val1 + val2;
    case '-': return val1 - val2;
    case '*': return val1 * val2;
    case '/':
      if (val2 == 0) {
        *success = false;
        return 0;
      }
      return val1 / val2;
    case TK_EQ: return val1 == val2;
    case TK_NEQ: return val1 != val2;
    case TK_AND: return val1 && val2;
    case TK_OR: return val1 || val2;
    default:
      *success = false;
      return 0;
  }
}

// 计算表达式e的值，成功返回结果，失败返回0
uint32_t expr(char *e, bool *success) {
  if (!make_token(e)) {
    *success = false;
    return 0;
  }
  /* TODO: Insert codes to evaluate the expression. */
  if (nr_token == 0) {
    *success = false;
    return 0;
  }

  return eval(0, nr_token - 1, success);
}
