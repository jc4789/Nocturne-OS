/* Host endpoint adapter for native Worker harnesses. Product record layouts
 * come from common/abi.h; Windows CRT supplies its own ssize_t. No OS runs. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <stdio.h>
#undef SYS_OPEN
#include "../../../common/abi.h"
#define MIN(a,b) ((a)<(b)?(a):(b))
#define WNOHANG 1
#define O_WRONLY 1
#define O_NONBLOCK 2048
ssize_t read(int,void *,size_t);
ssize_t write(int,const void *,size_t);
int open(const char *,int,...);
int close(int);
int fcntl(int,int,...);
int pipe(int[2]);
int spawn(const char *,char *const[],const int[3],int);
int kill(int);
int waitpid(int,int *,int);
int poll(struct n_pollfd *,int,int);
int n_channel_listen(const char *);
int n_channel_connect(const char *);
int n_channel_accept(int);
uint64_t uptime_ms(void);
int cpu_info(struct n_cpuinfo *);
void yield(void);
size_t strlcpy(char *,const char *,size_t);
