#pragma once
#include "kernel.h"
struct regs;
void syscall_dispatch(struct regs *r);
bool user_ok(const void *p, size_t n);
int user_str(char *dst, const char *src, size_t max);
