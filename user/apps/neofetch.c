/* System summary with a little moon */
#include <stdio.h>
#include <string.h>
#include "nocturne.h"

static const char *art[] = {
    "\033[1;33m        _..._      ",
    "\033[1;33m      .'   .::'.   ",
    "\033[1;33m     /    :::::  \\ ",
    "\033[1;33m    |     :::::   |",
    "\033[1;33m    |     ':::'   |",
    "\033[1;33m     \\     '::   / ",
    "\033[1;33m      '.       .'  ",
    "\033[1;33m        '-...-'    ",
    "\033[0m                   ",
    "\033[0m                   ",
    "\033[0m                   ",
    "\033[0m                   ",
};

int main(void) {
    struct n_sysinfo si;
    sysinfo(&si);
    char lines[12][128];
    int n = 0;
    unsigned long up = si.uptime_ms / 1000;
    int sw = 0, sh = 0;
    screen_size(&sw, &sh);
    struct n_pciinfo pci[32];
    int npci = pcilist(pci, 32);
    snprintf(lines[n++], 128, "\033[1;35mroot\033[0m@\033[1;35mnocturne\033[0m");
    snprintf(lines[n++], 128, "-------------");
    snprintf(lines[n++], 128, "\033[1;35mOS\033[0m: Nocturne 0.9 x86_64");
    snprintf(lines[n++], 128, "\033[1;35mKernel\033[0m: %s", si.os[0] ? si.os : "nocturne");
    snprintf(lines[n++], 128, "\033[1;35mUptime\033[0m: %luh %lum %lus", up / 3600, up / 60 % 60, up % 60);
    snprintf(lines[n++], 128, "\033[1;35mShell\033[0m: nsh");
    if (si.fb_w) snprintf(lines[n++], 128, "\033[1;35mResolution\033[0m: %ux%u", si.fb_w, si.fb_h);
    snprintf(lines[n++], 128, "\033[1;35mWM\033[0m: Nocturne compositor");
    snprintf(lines[n++], 128, "\033[1;35mCPU\033[0m: %s", si.cpu);
    snprintf(lines[n++], 128, "\033[1;35mMemory\033[0m: %luMiB / %luMiB",
             (unsigned long)((si.total_mem - si.free_mem) >> 20), (unsigned long)(si.total_mem >> 20));
    snprintf(lines[n++], 128, "\033[1;35mPCI devices\033[0m: %d   \033[1;35mTasks\033[0m: %d", npci, si.ntasks);
    lines[n][0] = 0;
    char colors[160] = "";
    for (int i = 0; i < 8; i++) {
        char b[16];
        snprintf(b, sizeof b, "\033[4%dm   ", i);
        strcat(colors, b);
    }
    strcat(colors, "\033[0m");
    strcpy(lines[n++], colors);
    printf("\n");
    for (int i = 0; i < 12; i++) printf("%s\033[0m  %s\n", art[i], i < n ? lines[i] : "");
    printf("\n");
    return 0;
}
