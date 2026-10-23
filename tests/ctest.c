/* ctest: libc behaviour checks (run with tcc -run, and as a tcc -o program). Prints "FAIL ..."
   lines and "ctest: ok". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <setjmp.h>
#include <ctype.h>
#include <time.h>
#include "nocturne.h"

static int failed;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failed++; } } while (0)

static jmp_buf jb;
static void jump(int v) { longjmp(jb, v); }

static int cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

/* Thread-local storage: initialized data then zeros, 12 bytes at alignment 4 (not a multiple of
   the thread control block's 8), a copy of its own in each thread. */
static __thread int tls_counter = 41;
static __thread char tls_zero[8];
static int tls_fresh;
static void tls_thread(void *arg) {
    (void)arg;
    tls_fresh = tls_counter == 41 && !tls_zero[7];
    tls_counter = 7;
    tls_zero[7] = 2;
}

int main(void) {
    char buf[128];
    snprintf(buf, sizeof buf, "%d %5.2f %-4s| %x %lu %e %g", -42, 3.14159, "ab", 255, 1234567890123UL, 12345.678, 0.0001);
    CHECK(!strcmp(buf, "-42  3.14 ab  | ff 1234567890123 1.234568e+04 0.0001"));
    snprintf(buf, sizeof buf, "%.3f %.0f %f", 1.0005, 2.5, -0.0);
    CHECK(!strncmp(buf, "1.000 2 -0.000000", 17) || !strncmp(buf, "1.001 2 -0.000000", 17));
    CHECK(fabs(strtod("1e-5", NULL) - 1e-5) < 1e-20);
    CHECK(strtod("0x1p4", NULL) == 16.0);
    CHECK(atoi("  -17x") == -17 && strtol("ff", NULL, 16) == 255);
    CHECK(fabs(sqrt(2.0) - 1.41421356237) < 1e-10);
    CHECK(fabs(sin(M_PI / 6) - 0.5) < 1e-12 && fabs(cos(0) - 1) < 1e-15);
    CHECK(fabs(pow(2, 10) - 1024) < 1e-9 && fabs(exp(log(10.0)) - 10) < 1e-12);
    CHECK(fabs(atan2(1, 1) - M_PI / 4) < 1e-12);
    CHECK(floor(-1.5) == -2 && ceil(-1.5) == -1 && fmod(7.5, 2) == 1.5);
    CHECK(isnan(NAN) && !isnan(1.0));

    int v = setjmp(jb);
    if (v == 0) jump(7);
    CHECK(v == 7);

    int a[] = {5, 3, 9, 1, 7, 2};
    qsort(a, 6, sizeof *a, cmp);
    CHECK(a[0] == 1 && a[5] == 9);
    int key = 7, *f = bsearch(&key, a, 6, sizeof *a, cmp);
    CHECK(f && *f == 7);

    CHECK(toupper('q') == 'Q' && isdigit('5') && !isalpha('5') && isspace('\t'));
    CHECK(strstr("haystack", "st") && !strstr("haystack", "zz"));
    char s[32] = "a,b,,c";
    int n = 0;
    for (char *t = strtok(s, ","); t; t = strtok(NULL, ",")) n++;
    CHECK(n == 3);
    CHECK(memcmp("abc", "abd", 3) < 0);

    time_t now = time(NULL);
    CHECK(now > 1700000000); /* the CMOS clock is set */
    struct tm *tm = gmtime(&now);
    CHECK(tm && tm->tm_year >= 123);

    char file[4096];
    CHECK(n_path_join(n_temp_folder(), "ctest.txt", file, sizeof file) > 0);
    FILE *fp = fopen(file, "w+");
    CHECK(fp != NULL);
    if (fp) {
        fprintf(fp, "line1\nline2\n");
        rewind(fp);
        char l[16];
        CHECK(fgets(l, sizeof l, fp) && !strcmp(l, "line1\n"));
        fclose(fp);
        CHECK(remove(file) == 0);
    }
    int x = 0;
    CHECK(sscanf("12 abc", "%d %s", &x, buf) == 2 && x == 12 && !strcmp(buf, "abc"));

    CHECK(tls_counter == 41 && !tls_zero[0] && !tls_zero[7]);
    tls_counter++;
    tls_zero[7] = 1;
    n_result tid = thread_create(tls_thread, NULL);
    CHECK(tid >= 0 && !thread_join((int)tid));
    CHECK(tls_fresh && tls_counter == 42 && tls_zero[7] == 1);

    if (!failed) printf("ctest: ok\n");
    return failed != 0;
}
