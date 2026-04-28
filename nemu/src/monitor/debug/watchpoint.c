#include "monitor/watchpoint.h"
#include "monitor/expr.h"
#include <stdio.h>
#include <string.h>

#define NR_WP 32

static WP wp_pool[NR_WP];
static WP *head, *free_;

void init_wp_pool() {
  // 初始化watchpoint池，将所有watchpoint连接成一个链表，head指向空闲链表的头部，free_指向空闲链表的头部
  int i;
  for (i = 0; i < NR_WP; i ++) {
    wp_pool[i].NO = i;
    wp_pool[i].next = &wp_pool[i + 1];
  }
  wp_pool[NR_WP - 1].next = NULL;

  head = NULL;
  free_ = wp_pool;
}

/* TODO(finished): Implement the functionality of watchpoint */
bool new_watchpoint(char *args) {
  if (args == NULL) {
    printf("Usage: w EXPR\n");
    return false;
  }

  // 空闲watchpoint不足
  if (free_ == NULL) {
    printf("No free watchpoints.\n");
    return false;
  }

  // 从嗯free_取下一个
  WP *wp = free_;
  free_ = free_->next;

  // 设置watchpoint的表达式和初始值
  strncpy(wp->expr, args, sizeof(wp->expr) - 1);
  wp->expr[sizeof(wp->expr) - 1] = '\0';

  bool success = true;
  // 计算初始值并保存
  wp->old_val = expr(wp->expr, &success);
  if (!success) {
    printf("Invalid expression: %s\n", wp->expr);
    wp->next = free_;
    free_ = wp;
    return false;
  }

  // 将watchpoint插入head链表
  wp->next = head;
  head = wp;

  printf("Watchpoint %d: %s\n", wp->NO, wp->expr);
  return true;
}

// 删除watchpoint
bool free_watchpoint(int no) {
  WP *prev = NULL;
  WP *cur = head;

  while (cur != NULL) {
    if (cur->NO == no) {
      // 找到位置
      if (prev == NULL) {
        head = cur->next;
      }
      else {
        prev->next = cur->next;
      }

      // 将cur加入free_链表
      cur->next = free_;
      free_ = cur;
      printf("Watchpoint %d deleted\n", no);
      return true;
    }

    prev = cur;
    cur = cur->next;
  }

  printf("Watchpoint %d not found\n", no);
  return false;
}

void wp_display(void) {
  WP *cur = head;
  
  // 遍历所有的watchpoint，打印编号和表达式

  if (cur == NULL) {
    printf("No watchpoints.\n");
    return;
  }

  printf("Num\tWhat\n");
  while (cur != NULL) {
    printf("%d\t%s\n", cur->NO, cur->expr);
    cur = cur->next;
  }
}

bool check_watchpoints(void) {
  WP *cur = head;
  bool triggered = false;

  while (cur != NULL) {
    bool success = true;
    // 计算当前值
    uint32_t new_val = expr(cur->expr, &success);

    if (!success) {
      printf("Failed to evaluate watchpoint %d: %s\n", cur->NO, cur->expr);
      cur = cur->next;
      continue;
    }

    // 如果值发生变化，打印watchpoint编号、表达式、旧值和新值，并更新旧值
    if (new_val != cur->old_val) {
      printf("Watchpoint %d triggered: %s\n", cur->NO, cur->expr);
      printf("Old value = %u (0x%x)\n", cur->old_val, cur->old_val);
      printf("New value = %u (0x%x)\n", new_val, new_val);
      cur->old_val = new_val;
      triggered = true;
    }

    cur = cur->next;
  }

  return triggered;
}

