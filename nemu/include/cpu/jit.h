#ifndef __CPU_JIT_H__
#define __CPU_JIT_H__

#include "nemu.h"

#ifdef CONFIG_JIT

void jit_init(void);
void jit_reset(void);
void jit_invalidate_all(void);

#else

static inline void jit_init(void) {}
static inline void jit_reset(void) {}
static inline void jit_invalidate_all(void) {}

#endif

#endif
