#pragma once
#include <stddef.h>
#include <sys/types.h>
ssize_t read(int,void *,size_t);
ssize_t write(int,const void *,size_t);
int close(int);
long long lseek(int,long long,int);
int unlink(const char *);
