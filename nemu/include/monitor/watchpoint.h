#ifndef __WATCHPOINT_H__
#define __WATCHPOINT_H__

#include "common.h"

typedef struct watchpoint {
  int NO;                 // watchpoint编号
  struct watchpoint *next;

  char expr[128];         // watchpoint表达式
  uint32_t old_val;       // 上一次的值

} WP;

// 初始化
void init_wp_pool(void);

// 创建新的watchpoint
bool new_watchpoint(char *args);  
// 删除watchpoint
bool free_watchpoint(int no);
// 显示所有watchpoint
void wp_display(void);
// 检查watchpoint是否被触发
bool check_watchpoints(void);

#endif
