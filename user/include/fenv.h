#pragma once
#include <cdefs.h>
NOCTURNE_BEGIN_DECLS
/* x86-64 floating-point environment: the SSE MXCSR register and the x87
   control/status words are kept in step, so both units round and flag alike. */
#define FE_INVALID 0x01
#define FE_DIVBYZERO 0x04
#define FE_OVERFLOW 0x08
#define FE_UNDERFLOW 0x10
#define FE_INEXACT 0x20
#define FE_ALL_EXCEPT 0x3d
#define FE_TONEAREST 0x000
#define FE_DOWNWARD 0x400
#define FE_UPWARD 0x800
#define FE_TOWARDZERO 0xc00
typedef unsigned short fexcept_t;
typedef struct {
    unsigned short x87_control, x87_status;
    unsigned int mxcsr;
} fenv_t;
#define FE_DFL_ENV ((const fenv_t *)-1)
int feclearexcept(int excepts);
int fegetexceptflag(fexcept_t *flag, int excepts);
int feraiseexcept(int excepts);
int fesetexceptflag(const fexcept_t *flag, int excepts);
int fetestexcept(int excepts);
int fegetround(void);
int fesetround(int round);
int fegetenv(fenv_t *env);
int feholdexcept(fenv_t *env);
int fesetenv(const fenv_t *env);
int feupdateenv(const fenv_t *env);
NOCTURNE_END_DECLS
