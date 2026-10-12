#pragma once
#include "../../../common/abi.h"
int getpid(void);
int proclist(struct n_procinfo *,int);
int stat(const char *,struct n_stat *);
int fstat(int,struct n_stat *);
int mkdir(const char *,unsigned);
int readdir(int,int,struct n_dirent *);
