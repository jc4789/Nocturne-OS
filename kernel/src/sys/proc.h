#pragma once
#include "kernel.h"
struct handle;
struct object;
struct process;

/* Start a program. Handle i of the new app is handles[i] (NULL: none). The app's third entry
   argument is a struct n_start on its stack, holding what is given here. When app is
   set, that handle record reports the app's end. parent: the app that launches
   it, NULL for one the kernel starts. identity: the new app's, decided by the caller
   (sys/authority.h). */
struct proc_launch {
    int argc;
    char **argv;
    int environment_count;
    char **environment; /* "NAME=value" strings for a ported program's C library */
    const char *folder; /* a full path, or NULL */
    const int32_t *terminals;
    uint32_t terminal_count;
    int32_t interrupts;
    struct handle **handles;
    size_t handle_count;
    struct process *parent;
    struct object *app;
    struct n_identity identity;
};
int proc_launch(const char *path, const struct proc_launch *launch);

/* App handles (docs/nocturne-native-abi.md §5). */
/* A new app record and the handle that will report its end (proc_launch's app is its object). */
int proc_app_create(struct handle **out);
int proc_app_open(int pid, struct handle **out);
bool proc_app_is(struct handle *h);
void proc_app_ended(struct object *app, int status);
int proc_app_pid(struct handle *h); /* 0 once it has ended */
int proc_app_id(struct handle *h);  /* its identifier, also after its end */
int proc_app_result(struct handle *h, uint64_t deadline, int *status); /* sys/wait.h; N_ERR_TIMEOUT after it */
