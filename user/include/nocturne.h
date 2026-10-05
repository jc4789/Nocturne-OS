/* Nocturne OS system interface. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "abi.h"
#include "fcntl.h"
#include "gfx.h"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define ABS(a) ((a) < 0 ? -(a) : (a))

typedef long ssize_t;
typedef int pid_t;

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int open(const char *path, int flags, ...);
int close(int fd);
long lseek(int fd, long off, int whence);
int stat(const char *path, struct n_stat *st);
int fstat(int fd, struct n_stat *st);
int readdir(int fd, int idx, struct n_dirent *d);
int mkdir(const char *path, ...);
int unlink(const char *path);
int rmdir(const char *path);
int rename(const char *from, const char *to);
int chdir(const char *path);
char *getcwd(char *buf, size_t n);
int spawn(const char *path, char *const argv[], const int fdmap[3], int flags);
int waitpid(int pid, int *status, int flags);
#define WNOHANG 1
int getpid(void);
int getppid(void);
int kill(int pid);
int killtree(int pid, int include_self);
void *sbrk(long inc);
int msleep(unsigned ms);
unsigned sleep(unsigned s);
uint64_t uptime_ms(void);
int pipe(int fds[2]);
int dup(int fd);
int dup2(int fd, int newfd);
int poll(struct n_pollfd *fds, int n, int timeout_ms);
int proclist(struct n_procinfo *out, int max);
int sysinfo(struct n_sysinfo *si);
void yield(void);
int poweroff(void);
int reboot(void);
int fcntl(int fd, int cmd, ...);
int dmesg(char *buf, size_t n);
int pcilist(struct n_pciinfo *out, int max);
int isatty(int fd);
extern char **environ;
int execvp(const char *file, char *const argv[]); /* spawn, wait and exit */
int execv(const char *path, char *const argv[]);
long __syscall(long n, long a, long b, long c, long d, long e);

/* run a program (searching /bin) and wait for it; returns exit status or negative error */
int run_wait(const char *path, char *const argv[]);
/* find a program: "ls" -> "/bin/ls"; returns false if not found */
bool find_program(const char *name, char *out, size_t n);

/* line editor for terminal programs (echo, editing keys, history). Returns the line length, or -1
   at end of input (Ctrl+D). RL_SECRET echoes '*' and keeps the line out of the history. */
#define RL_SECRET 1
int readline(const char *prompt, char *buf, size_t size, int flags);

/* ---- networking (net.c) ----
   IPv4 addresses are uint32_t in host order (10.0.2.15 is 0x0A00020F). Functions return -1 and
   set errno on failure. Timeouts are in milliseconds; -1 waits forever. */
int net_info(struct n_netinfo *ni);
int net_ping(uint32_t ip, int seq, int timeout_ms); /* round-trip time in ms */
int net_resolve(const char *name, uint32_t *ip);    /* DNS lookup or dotted quad */
bool ip_parse(const char *s, uint32_t *ip);
char *ip_format(uint32_t ip, char *buf); /* buf: at least 16 bytes */
int udp_socket(uint16_t port);           /* 0 picks an ephemeral port */
int udp_sendto(int s, uint32_t ip, uint16_t port, const void *buf, size_t len);
int udp_recvfrom(int s, void *buf, size_t len, int timeout_ms, struct n_sockaddr *from);
int udp_close(int s);
int tcp_connect(uint32_t ip, uint16_t port, int timeout_ms);
long tcp_send(int s, const void *buf, size_t len);
long tcp_recv(int s, void *buf, size_t len, int timeout_ms); /* 0 = connection closed */
int tcp_close(int s);
bool net_wait_up(int timeout_ms); /* wait for DHCP; false if no network */

/* ---- GUI ---- */
typedef struct window {
    int fd;
    int w, h; /* client size */
    canvas_t c;
    int flags;
    struct gui_event q[32];
    int qn, qi;
    bool closed;
} window_t;

bool gui_available(void);
int screen_size(int *w, int *h);
window_t *win_open(int w, int h, const char *title, int flags);
void win_close(window_t *win);
void win_update(window_t *win);
void win_update_rect(window_t *win, int x, int y, int w, int h);
void win_set_title(window_t *win, const char *title);
void win_move(window_t *win, int x, int y);
/* wait up to timeout_ms (-1 = forever) for an event; returns 1 if *e was filled, 0 on timeout,
   -1 if the window is gone. EV_RESIZE is handled (buffer reallocated) before being returned. */
int win_event(window_t *win, struct gui_event *e, int timeout_ms);
int clipboard_set(const char *s, size_t n);
int clipboard_get(char *buf, size_t n);
int gui_launch(const char *path, const char *arg);

/* ---- tiny UI kit (ui.c) ---- */
#define UI_BG      RGB(30, 28, 52)
#define UI_PANEL   RGB(40, 38, 70)
#define UI_FG      RGB(232, 232, 245)
#define UI_DIM     RGB(150, 150, 185)
#define UI_ACCENT  RGB(130, 110, 240)
#define UI_ACCENT2 RGB(246, 200, 110)
#define UI_BTN     RGB(56, 52, 98)
#define UI_BTN_HI  RGB(84, 74, 150)
void ui_button(canvas_t *c, int x, int y, int w, int h, const char *label, bool hot, bool pressed);
void ui_text_center(canvas_t *c, int x, int y, int w, const char *s, uint32_t col, int font);
bool ui_hit(int px, int py, int x, int y, int w, int h);
/* draws a single-line text field; returns nothing. */
void ui_textfield(canvas_t *c, int x, int y, int w, const char *text, bool focus);
/* edit a line buffer with a key event; returns 1 on Enter, -1 on Escape, 0 otherwise */
int ui_edit_key(char *buf, size_t cap, const struct gui_event *e);
