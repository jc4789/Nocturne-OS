#pragma once
#include "kernel.h"
struct regs;
void syscall_dispatch(struct regs *r);
bool user_ok(const void *p, size_t n);   /* the process may read it */
bool user_ok_w(void *p, size_t n);       /* the process may write it */
int user_str(char *dst, const char *src, size_t max);
