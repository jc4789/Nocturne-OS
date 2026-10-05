/* cowsay, but a moon */
#include <stdio.h>
#include <string.h>
#include "nocturne.h"

#define WRAP 40

int main(int argc, char **argv) {
    char msg[1024] = "";
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            if (i > 1) strlcat(msg, " ", sizeof msg);
            strlcat(msg, argv[i], sizeof msg);
        }
    } else {
        int n = (int)read(0, msg, sizeof msg - 1);
        msg[n > 0 ? n : 0] = 0;
        for (char *p = msg; *p; p++)
            if (*p == '\n') *p = ' ';
    }
    if (!msg[0]) strcpy(msg, "...");
    /* word wrap */
    char lines[32][WRAP + 1];
    int nl = 0, maxw = 0;
    char *p = msg;
    while (*p && nl < 32) {
        while (*p == ' ') p++;
        if (!*p) break;
        int len = (int)strlen(p);
        int take = len <= WRAP ? len : WRAP;
        if (len > WRAP) {
            int k = take;
            while (k > 0 && p[k] != ' ') k--;
            if (k > 0) take = k;
        }
        memcpy(lines[nl], p, take);
        lines[nl][take] = 0;
        while (take > 0 && lines[nl][take - 1] == ' ') lines[nl][--take] = 0;
        maxw = MAX(maxw, take);
        nl++;
        p += take;
    }
    printf(" ");
    for (int i = 0; i < maxw + 2; i++) putchar('_');
    printf("\n");
    for (int i = 0; i < nl; i++) {
        char l = '|', r = '|';
        if (nl == 1) l = '<', r = '>';
        else if (i == 0) l = '/', r = '\\';
        else if (i == nl - 1) l = '\\', r = '/';
        printf("%c %-*s %c\n", l, maxw, lines[i], r);
    }
    printf(" ");
    for (int i = 0; i < maxw + 2; i++) putchar('-');
    printf("\n");
    printf("   \\\n");
    printf("    \\  \033[1;33m   _..._\n");
    printf("        .' .::::.\n");
    printf("       :  ::::::::\n");
    printf("       :  ::::::::\n");
    printf("       `. '::::::'\n");
    printf("         `-.::''\033[0m\n");
    return 0;
}
