#include <unistd.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <assert.h>
#include <time.h>
#include "syscall.h"

#ifndef __ISA_NATIVE__

// Nanos-lite 上的系统调用入口封装：
// eax 传系统调用号，ebx/ecx/edx 传前三个参数，返回值从 eax 取回。
int _syscall_(int type, uintptr_t a0, uintptr_t a1, uintptr_t a2){
  int ret = -1;
  asm volatile("int $0x80": "=a"(ret): "a"(type), "b"(a0), "c"(a1), "d"(a2));
  return ret;
}

// 退出当前程序，最终会由 Nanos-lite 调用 _halt(status) 结束模拟。
void _exit(int status) {
  _syscall_(SYS_exit, status, 0, 0);
}

// 打开文件，path 指针会直接传给内核侧文件系统。
int _open(const char *path, int flags, mode_t mode) {
  return _syscall_(SYS_open, (uintptr_t)path, flags, mode);
}

// 向文件描述符写入 count 字节数据。
int _write(int fd, void *buf, size_t count){
  return _syscall_(SYS_write, fd, (uintptr_t)buf, count);
}

// 调整用户程序的堆顶。这里维护 libc 侧的 program_break，并通过 SYS_brk 通知内核。
void *_sbrk(intptr_t increment){
  extern char _end;
  static char *program_break = NULL;

  // 第一次调用时，堆顶从程序镜像末尾 _end 开始。
  if (program_break == NULL) {
    program_break = &_end;
  }

  // 被调用时, 根据记录的program break位置和参数increment, 计算出新program break
  char *old_break = program_break;
  char *new_break = program_break + increment;

  // 通过SYS_brk系统调用来让操作系统设置新program break
  // 若SYS_brk系统调用成功, 该系统调用会返回0, 此时更新之前记录的program break的位置,
  // 并将旧program break的位置作为_sbrk()的返回值返回
  if (_syscall_(SYS_brk, (uintptr_t)new_break, 0, 0) == 0) {
    program_break = new_break;
    return old_break;
  }

  // 若该系统调用失败, _sbrk()会返回-1
  return (void *)-1;
}

// 从文件描述符读取 count 字节数据到 buf。
int _read(int fd, void *buf, size_t count) {
  return _syscall_(SYS_read, fd, (uintptr_t)buf, count);
}

// 关闭文件描述符。
int _close(int fd) {
  return _syscall_(SYS_close, fd, 0, 0);
}

// 调整文件读写位置。
off_t _lseek(int fd, off_t offset, int whence) {
  return _syscall_(SYS_lseek, fd, offset, whence);
}

// 下面这些接口当前不会被 Nanos-lite 真正使用。
// 为了满足 libc 或应用程序的链接需求，这里先提供占位实现。

// fstat 暂未实现，返回 0 作为占位。
int _fstat(int fd, struct stat *buf) {
  return 0;
}

// execve 暂未实现，若运行到这里说明当前程序调用了未支持的接口。
int execve(const char *fname, char * const argv[], char *const envp[]) {
  assert(0);
  return -1;
}

int _execve(const char *fname, char * const argv[], char *const envp[]) {
  return execve(fname, argv, envp);
}

// kill/getpid 当前未实现，用特殊退出码帮助定位误调用。
int _kill(int pid, int sig) {
  _exit(-SYS_kill);
  return -1;
}

pid_t _getpid() {
  _exit(-SYS_getpid);
  return 1;
}

// 部分 libc 代码会引用 environ，因此这里提供全局变量定义。
char **environ;

// 以下 POSIX 接口暂未实现，若被调用则触发 assert 便于调试。
time_t time(time_t *tloc) {
  assert(0);
  return 0;
}

int signal(int num, void *handler) {
  assert(0);
  return -1;
}

pid_t _fork() {
  assert(0);
  return -1;
}

int _link(const char *d, const char *n) {
  assert(0);
  return -1;
}

int _unlink(const char *n) {
  assert(0);
  return -1;
}

pid_t _wait(int *status) {
  assert(0);
  return -1;
}

clock_t _times(void *buf) {
  assert(0);
  return 0;
}

int _gettimeofday(struct timeval *tv) {
  assert(0);
  tv->tv_sec = 0;
  tv->tv_usec = 0;
  return 0;
}

int _fcntl(int fd, int cmd, ... ) {
  assert(0);
  return 0;
}

int pipe(int pipefd[2]) {
  assert(0);
  return 0;
}

int dup(int oldfd) {
  assert(0);
  return 0;
}

int dup2(int oldfd, int newfd) {
  assert(0);
  return 0;
}

pid_t vfork(void) {
  assert(0);
  return 0;
}

#endif
