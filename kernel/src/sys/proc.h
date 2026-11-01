#pragma once
#include "kernel.h"
struct handle;
struct object;
struct process;

/* Start a program. Handle i of the new app is handles[i] (NULL: none). When
   app is set, that handle record reports the app's end. */
struct proc_launch {
    int argc;
    char **argv;
    struct handle **handles;
    size_t handle_count;
    struct object *app;
};
int proc_launch(const char *path, const struct proc_launch *launch);
extern int init_pid; /* the first app: the only one that may claim a PCI function */
int proc_spawn_simple(const char *path, const char *arg1);

/* App handles (docs/nocturne-native-abi.md §5). */
/* A new app record and the handle that will report its end (proc_launch's app is its object). */
int proc_app_create(struct handle **out);
int proc_app_open(int pid, struct handle **out);
bool proc_app_is(struct handle *h);
void proc_app_ended(struct object *app, int status);
int proc_app_pid(struct handle *h); /* 0 once it has ended */
int proc_app_id(struct handle *h);  /* its identifier, also after its end */
int proc_app_result(struct handle *h, uint64_t deadline, int *status); /* sys/wait.h; N_ERR_TIMEOUT after it */
