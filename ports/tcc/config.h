/* TinyCC configuration for building tcc as a native Nocturne program. */
#define TCC_VERSION "0.9.28rc"
#define TCC_TARGET_X86_64 1
#define CONFIG_TCC_STATIC 1      /* no dlopen: Nocturne has no shared libraries */
#define CONFIG_TCC_BACKTRACE 0
#define CONFIG_TCC_BCHECK 0
#define CONFIG_TCC_NOCTURNE 1
#define CONFIG_TCC_SEMLOCK 1
#define CONFIG_TCC_PREDEFS 1     /* tccdefs.h compiled in */
/* The SDK, C:\Nocturne\SDK (scripts/mksdk.sh): TinyCC's own files in Lib\tcc, the headers in
   Include, crt1.o, libc.a and the other libraries in Lib. Paths are separated by ';'. */
#define CONFIG_TCCDIR "C:\\Nocturne\\SDK\\Lib\\tcc"
#define CONFIG_TCC_SYSINCLUDEPATHS "{B}\\include;C:\\Nocturne\\SDK\\Include"
#define CONFIG_TCC_LIBPATHS "{B};C:\\Nocturne\\SDK\\Lib"
#define CONFIG_TCC_CRTPREFIX "C:\\Nocturne\\SDK\\Lib"
#define CONFIG_TCC_ELFINTERP "-"
#define CONFIG_TCC_SWITCHES "-static"
#define CONFIG_RUNMEM_RO 1       /* tcc -run: code read+execute, data read+write (W^X) */
