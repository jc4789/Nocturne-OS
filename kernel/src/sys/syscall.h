#pragma once
#include "kernel.h"
#include "sys/clock.h"
struct regs;
/* Any CPU: the app's call in r, on the stack of the calling thread. */
void syscall_dispatch(struct regs *r);
/* The calling app's memory. A sibling thread on another CPU may unmap or protect any page of it at
   any time, so the kernel reads and writes it only through these copies, never through the app's
   pointer itself: a page that went meanwhile ends the copy with -N_ERR_BAD_ADDRESS instead of a
   fault. A page not there yet is made present first (a growing stack, a reserved page, a pager's
   page), which may sleep: never with a spinlock held. */
int copy_from_user(void *dst, const void *usrc, size_t n);
int copy_to_user(void *udst, const void *src, size_t n);
/* Whether [p, p+n) is memory the app may read (or, with _w, also write) now, its pages made
   present: a check before long work, which the copies after it still make again. */
bool user_ok(const void *p, size_t n);
bool user_ok_w(void *p, size_t n);
/* The NUL-terminated string at src into dst (at most max bytes with the NUL): its length, or an
   error (-N_ERR_NAME_TOO_LONG when it does not fit). */
int user_str(char *dst, const char *src, size_t max);
/* An app's string of any length (a path, an argument), copied into the kernel; kfree it after use. */
int user_string(const char *u, char **out);
