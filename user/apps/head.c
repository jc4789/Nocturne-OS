#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    int lines = 10, i = 1;
    if (argc > 2 && !strcmp(argv[1], "-n")) {
        lines = atoi(argv[2]);
        i = 3;
    }
    FILE *f = i < argc ? fopen(argv[i], "r") : stdin;
    if (!f) {
        perror(argv[i]);
        return 1;
    }
    char buf[1024];
    while (lines-- > 0 && fgets(buf, sizeof buf, f)) fputs(buf, stdout);
    return 0;
}
