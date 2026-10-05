#include <stdio.h>
#include <time.h>
int main(int argc, char **argv) {
    time_t t = time(NULL);
    char buf[128];
    strftime(buf, sizeof buf, argc > 1 && argv[1][0] == '+' ? argv[1] + 1 : "%A %d %B %Y, %H:%M:%S", localtime(&t));
    puts(buf);
    return 0;
}
