/* Isolated host contract for the native Worker broker; no OS execution. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <fcntl.h>
#undef SYS_OPEN
#include "../../common/abi.h"
#define read test_native_read
#define write test_native_write
#define close test_native_close
#define open test_native_open
#define SPAWN_DETACH 1
typedef long long ssize_t;
#define WNOHANG 1
int waitpid(int,int*,int);
int kill(int);
int pipe(int*);
int spawn(const char*,char*const*,const int*,int);
int close(int);
ssize_t read(int,void*,size_t);
ssize_t write(int,const void*,size_t);
int poll(struct n_pollfd*,int,int);
int fcntl(int,int,...);
int open(const char*,int,...);
int n_channel_listen(const char*);
int n_channel_connect(const char*);
int n_channel_accept(int);
uint64_t uptime_ms(void);
int cpu_info(struct n_cpuinfo*);
#ifndef F_SETFL
#define F_SETFL 4
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x800
#endif
