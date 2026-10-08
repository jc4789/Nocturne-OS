#ifndef NMEDIA_STDIO_PRIVATE_H
#define NMEDIA_STDIO_PRIVATE_H
#include <stdio.h>

/* FF 現４呼出だけのprivate接点。FILE の内部headerを読まない。
 * fdopen は新しい非負の owned fd を成功・失敗とも引き取る。
 * 同じfdが既存私有streamに属す場合は拒否し、既存所有を保つ。
 * 対応は r/rb と setvbuf(NULL,_IONBF,...)、read/close のみ。
 * token を通常stdioへ渡してはならない。全stdioの互換APIではない。 */
FILE *nmedia_ff_fdopen(int fd, const char *mode);
size_t nmedia_ff_fread(void *buf, size_t size, size_t count, FILE *token);
int nmedia_ff_setvbuf(FILE *token, char *buf, int mode, size_t size);
int nmedia_ff_fclose(FILE *token);
#endif
