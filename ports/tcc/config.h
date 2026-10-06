/* TinyCC configuration for building tcc as a native Nocturne program. */
#define TCC_VERSION "0.9.28rc"
#define TCC_TARGET_X86_64 1
#define CONFIG_TCC_STATIC 1      /* no dlopen: Nocturne has no shared libraries */
#define CONFIG_TCC_BACKTRACE 0
#define CONFIG_TCC_BCHECK 0
#define CONFIG_TCC_SEMLOCK 0
#define CONFIG_TCC_PREDEFS 1     /* tccdefs.h compiled in */
#define CONFIG_TCCDIR "/usr/lib/tcc"
#define CONFIG_TCC_SYSINCLUDEPATHS "{B}/include:/usr/include"
#define CONFIG_TCC_LIBPATHS "{B}:/usr/lib:/data/lib"
#define CONFIG_TCC_CRTPREFIX "/usr/lib"
#define CONFIG_TCC_ELFINTERP "-"
#define CONFIG_TCC_SWITCHES "-static"
#define CONFIG_RUNMEM_RO 1       /* tcc -run: code read+execute, data read+write (W^X) */
