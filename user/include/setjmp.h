#pragma once
/* rbx, rbp, r12, r13, r14, r15, rsp, rip */
typedef long jmp_buf[8];
int setjmp(jmp_buf env);
_Noreturn void longjmp(jmp_buf env, int val);
#define _setjmp setjmp
#define _longjmp longjmp
