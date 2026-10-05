#include <stdio.h>
#include <string.h>
#include <stdbool.h>
static bool icase, invert, number;
static bool has(const char *line, const char *pat) {
    if (!icase) return strstr(line, pat) != NULL;
    size_t pl = strlen(pat);
    for (const char *p = line; *p; p++)
        if (!strncasecmp(p, pat, pl)) return true;
    return false;
}
static int grep(FILE *f, const char *pat, const char *name) {
    char line[2048];
    int found = 0, ln = 0;
    while (fgets(line, sizeof line, f)) {
        ln++;
        if (has(line, pat) != invert) {
            found++;
            if (name) printf("\x1b[35m%s\x1b[0m:", name);
            if (number) printf("\x1b[32m%d\x1b[0m:", ln);
            fputs(line, stdout);
            if (!strchr(line, '\n')) putchar('\n');
        }
    }
    return found;
}
int main(int argc, char **argv) {
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (strchr(argv[i], 'i')) icase = true;
        if (strchr(argv[i], 'v')) invert = true;
        if (strchr(argv[i], 'n')) number = true;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: grep [-ivn] PATTERN [FILE...]\n");
        return 2;
    }
    const char *pat = argv[i++];
    int found = 0;
    if (i >= argc) found = grep(stdin, pat, NULL);
    for (int j = i; j < argc; j++) {
        FILE *f = fopen(argv[j], "r");
        if (!f) {
            perror(argv[j]);
            continue;
        }
        found += grep(f, pat, argc - i > 1 ? argv[j] : NULL);
        fclose(f);
    }
    return found ? 0 : 1;
}
