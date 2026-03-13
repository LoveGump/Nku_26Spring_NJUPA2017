#include "nemu.h"

/* We use the POSIX regex functions to process regular expressions.
 * Type 'man regex' for more information about POSIX regex functions.
 */
#include <sys/types.h>
#include <regex.h>

enum {
  TK_NOTYPE = 256, TK_EQ,

  /* TODO: Add more token types */
  TK_NUM, // 十进制数字
  TK_HEX, // 十六进制数字
  TK_REG, // 寄存器
  TK_NEQ, // 不等于
  TK_AND, // 逻辑与
  TK_DEREF, // 解引用
  TK_NEG // 负号

};

static struct rule {
  char *regex;
  int token_type;
} rules[] = {

    /* TODO: Add more rules.
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

// 计算表达式e的值，成功返回结果，失败返回0
uint32_t expr(char *e, bool *success) {
  if (!make_token(e)) {
    *success = false;
    return 0;
  }

  /* TODO: Insert codes to evaluate the expression. */
  TODO();

  return 0;
}
