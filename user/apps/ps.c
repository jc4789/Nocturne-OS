#include <stdio.h>
#include "nocturne.h"
int main(void) {
    struct n_procinfo p[128];
    int n = proclist(p, 128);
    static const char *st[] = {"ready", "run", "sleep", "zombie"};
    printf("\x1b[1m  PID  PPID STATE    CPU(ms)  MEM(K)  NAME\x1b[0m\n");
    for (int i = 0; i < n; i++)
        printf("%5d %5d %-7s %8lu %7lu  %s%s\n", p[i].pid, p[i].ppid, st[p[i].state & 3], (unsigned long)p[i].cpu_ms,
               (unsigned long)p[i].mem_kb, p[i].name, p[i].is_user ? "" : " \x1b[90m[kernel]\x1b[0m");
    return 0;
}
