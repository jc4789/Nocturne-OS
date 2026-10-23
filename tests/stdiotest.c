/* ISO C stdio over the native ABI on the real kernel: documents with the
 * stream's own position, append, truncation, exclusive creation, temporary
 * documents, remove/rename, names relative to the app's folder, and the
 * standard streams on documents and message channels. Run in a dedicated test
 * VM; it says nothing about GUI or real-site acceptance. */
#include "nocturne.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static unsigned checks, failed;
#define CHECK(x) do { checks++; if (!(x)) { failed++; printf("stdiotest FAIL line %d\n", __LINE__); } } while (0)

static bool loaded(const char *path, const char *want, size_t n) {
    void *data;
    size_t size;
    if (n_document_load(path, &data, &size) < 0) return false;
    bool same = size == n && (!n || !memcmp(data, want, n));
    if (data) n_memory_release(data, size);
    return same;
}

/* The stream calls on documents inside root, a complete path. */
static void documents(const char *root) {
    char a[512], b[512], buf[64];
    CHECK(n_path_join(root, "stdiotest-a.txt", a, sizeof a) > 0);
    CHECK(n_path_join(root, "stdiotest-b.txt", b, sizeof b) > 0);
    n_entry_remove(a);
    n_entry_remove(b);

    errno = 0;
    CHECK(fopen(a, "r") == NULL && errno == N_ERR_NOT_FOUND);
    FILE *f = fopen(a, "w");
    CHECK(f != NULL);
    if (!f) return;
    CHECK(fputs("hello\n", f) == 0 && fprintf(f, "n=%d\n", 42) == 5);
    CHECK(ftell(f) == 11);
    CHECK(fclose(f) == 0);
    CHECK(loaded(a, "hello\nn=42\n", 11));

    f = fopen(a, "r");
    CHECK(f != NULL);
    if (!f) return;
    CHECK(fgets(buf, sizeof buf, f) && !strcmp(buf, "hello\n"));
    CHECK(ftell(f) == 6);
    CHECK(fseek(f, 2, SEEK_SET) == 0 && fgetc(f) == 'l' && ftell(f) == 3);
    CHECK(ungetc('L', f) == 'L' && ftell(f) == 2 && fgetc(f) == 'L' && fgetc(f) == 'l');
    CHECK(fseek(f, -3, SEEK_END) == 0 && fgetc(f) == '4' && ftell(f) == 9);
    CHECK(fseek(f, -1, SEEK_CUR) == 0 && fgetc(f) == '4');
    CHECK(fseek(f, -100, SEEK_CUR) == -1 && errno == N_ERR_INVALID);
    CHECK(fseek(f, 0, SEEK_END) == 0 && fgetc(f) == EOF && feof(f));
    clearerr(f);
    CHECK(fputc('x', f) == 'x' && fflush(f) == EOF && ferror(f)); /* read-only */
    CHECK(fclose(f) == 0);
    CHECK(loaded(a, "hello\nn=42\n", 11));

    f = fopen(a, "a");
    CHECK(f && fputs("more\n", f) == 0 && fclose(f) == 0);
    CHECK(loaded(a, "hello\nn=42\nmore\n", 16));

    f = fopen(a, "r+");
    CHECK(f != NULL);
    if (!f) return;
    CHECK(fgetc(f) == 'h' && fseek(f, 0, SEEK_CUR) == 0 && fputc('E', f) == 'E');
    CHECK(fseek(f, 0, SEEK_SET) == 0 && fread(buf, 1, 5, f) == 5 && !memcmp(buf, "hEllo", 5));
    CHECK(fclose(f) == 0);
    CHECK(loaded(a, "hEllo\nn=42\nmore\n", 16));

    /* Two appenders write after each other, never over each other. */
    FILE *x = fopen(a, "a"), *y = fopen(a, "a");
    CHECK(x && y);
    if (x && y) {
        CHECK(fputs("x1\n", x) == 0 && fflush(x) == 0 && fputs("y1\n", y) == 0 && fflush(y) == 0);
        CHECK(fputs("x2\n", x) == 0 && fclose(x) == 0 && fclose(y) == 0);
        CHECK(loaded(a, "hEllo\nn=42\nmore\nx1\ny1\nx2\n", 25));
    }

    errno = 0;
    CHECK(fopen(a, "wx") == NULL && errno == N_ERR_EXISTS);
    f = fopen(b, "wx");
    CHECK(f && fputs("bee", f) == 0 && fclose(f) == 0);
    f = fopen(a, "w");
    CHECK(f != NULL);
    struct n_entry e;
    CHECK(n_entry_info(a, &e) == 0 && e.size == 0); /* truncated at once */
    CHECK(f && fclose(f) == 0);
    /* writing, then reading back through the same stream */
    f = fopen(a, "w+");
    CHECK(f && fputs("0123456789", f) == 0 && fseek(f, 3, SEEK_SET) == 0 && fgetc(f) == '3');
    if (f) {
        CHECK(fputs("ab", f) == 0 && ftell(f) == 6);
        rewind(f);
        CHECK(fread(buf, 1, sizeof buf, f) == 10 && !memcmp(buf, "0123ab6789", 10) && feof(f));
        fclose(f);
    }

    /* rename to a free name moves; onto an existing document replaces it */
    CHECK(rename(b, a) == 0 && loaded(a, "bee", 3));
    CHECK(n_entry_info(b, &e) == -N_ERR_NOT_FOUND);
    CHECK(rename(a, b) == 0 && loaded(b, "bee", 3));
    errno = 0;
    CHECK(rename(a, b) == -1 && errno == N_ERR_NOT_FOUND);
    CHECK(remove(b) == 0 && n_entry_info(b, &e) == -N_ERR_NOT_FOUND);
    CHECK(remove(b) == -1 && errno == N_ERR_NOT_FOUND);
}

/* How many entries the temporary folder has (all of them, a page at a time), or a negative error. */
static int tmp_entries(void) {
    struct n_entry list[64];
    int total = 0;
    for (;;) {
        n_result n = n_folder_list(n_temp_folder(), list, 64, (uint64_t)total);
        if (n < 0) return (int)n;
        if (!n) return total;
        total += (int)n;
    }
}

static void temporary(void) {
    int before = tmp_entries();
    FILE *t = tmpfile();
    CHECK(t != NULL);
    if (!t) return;
    CHECK(tmp_entries() == before + 1);
    char buf[16];
    CHECK(fputs("scratch", t) == 0);
    rewind(t);
    CHECK(fread(buf, 1, sizeof buf, t) == 7 && !memcmp(buf, "scratch", 7));
    CHECK(fclose(t) == 0);
    CHECK(tmp_entries() == before);
}

static void relative(void) {
    char was[512], one[512], two[512], gone[640], file[640], renamed[640];
    CHECK(n_path_complete(".", was, sizeof was) > 0);
    CHECK(n_path_join(n_temp_folder(), "stdiotest-one", one, sizeof one) > 0);
    CHECK(n_path_join(n_temp_folder(), "stdiotest-two", two, sizeof two) > 0);
    snprintf(gone, sizeof gone, "%s\\no-such-folder", two);
    snprintf(file, sizeof file, "%s\\stdiotest-rel.txt", two);
    snprintf(renamed, sizeof renamed, "%s\\stdiotest-rel2.txt", two);
    CHECK(n_folder_create(one) == 0 && n_folder_create(two) == 0);
    CHECK(n_folder_set(one) == 0 && n_folder_set("..\\stdiotest-two") == 0);
    CHECK(n_folder_set(gone) == -N_ERR_NOT_FOUND);
    CHECK(n_folder_set("D:\\") == -N_ERR_NOT_FOUND); /* there is no drive D: */
    FILE *f = fopen("stdiotest-rel.txt", "w");
    CHECK(f && fputs("rel", f) == 0 && fclose(f) == 0);
    CHECK(loaded(file, "rel", 3));
    CHECK(n_folder_set(file) == -N_ERR_NOT_FOLDER);
    CHECK(rename("stdiotest-rel.txt", "..\\stdiotest-two\\stdiotest-rel2.txt") == 0 && loaded(renamed, "rel", 3));
    CHECK(remove("stdiotest-rel2.txt") == 0);
    n_folder_set(was);
    n_entry_remove(one);
    n_entry_remove(two);
}

/* Child modes. "print": output and log on whatever the launcher gave.
   "count": read all of the input and report its length and sum. */
static int child(const char *mode) {
    if (!strcmp(mode, "print")) {
        for (int i = 0; i < 3; i++) printf("child line %d\n", i);
        fflush(stdout);
        fputs("child log\n", stderr);
        return 0;
    }
    if (!strcmp(mode, "count")) {
        unsigned long n = 0, sum = 0;
        int c;
        char block[5000];
        size_t got = fread(block, 1, sizeof block, stdin); /* larger than the input buffer */
        for (size_t i = 0; i < got; i++) sum += (unsigned char)block[i];
        n += got;
        while ((c = getchar()) != EOF) {
            n++;
            sum += (unsigned char)c;
        }
        printf("count %lu sum %lu\n", n, sum);
        return ferror(stdin) ? 3 : 0;
    }
    return 1;
}

static void standard_streams(const char *self) {
    char out[512];
    CHECK(n_path_join(n_temp_folder(), "stdiotest-out.txt", out, sizeof out) > 0);
    CHECK(n_document_save(out, "parent:", 7) == 0);
    n_handle doc = n_document_open(out, N_DOCUMENT_EDIT);
    CHECK(doc >= 0);
    const char *print_args[] = {self, "child", "print"};
    int32_t handles[] = {-1, doc, doc};
    struct n_launch l = {.arguments = print_args, .handles = handles, .argument_count = 3, .handle_count = 3};
    n_handle app = n_app_launch(self, &l);
    int status = -1;
    CHECK(app >= 0 && n_app_result(app, 10000, &status) == 0 && status == 0);
    n_handle_close(app);
    const char want[] = "parent:child line 0\nchild line 1\nchild line 2\nchild log\n";
    CHECK(loaded(out, want, sizeof want - 1)); /* appended after what was there */
    n_handle_close(doc);
    n_entry_remove(out);

    /* Input from a message channel: one message larger than any buffer, an
       empty one, and a small one; then the end. */
    doc = n_document_open(out, N_DOCUMENT_CREATE);
    CHECK(doc >= 0);
    n_handle c[2];
    CHECK(n_channel_pair(c) == 0);
    const char *count_args[] = {self, "child", "count"};
    handles[0] = c[1];
    handles[1] = handles[2] = doc;
    l.arguments = count_args;
    app = n_app_launch(self, &l);
    CHECK(app >= 0);
    n_handle_close(c[1]);
    static char big[20000];
    unsigned long sum = 0;
    for (size_t i = 0; i < sizeof big; i++) sum += (unsigned char)(big[i] = (char)(i * 7 + 3));
    CHECK(n_channel_send(c[0], big, 3000, -1) == 3000);
    CHECK(n_channel_send(c[0], big + 3000, sizeof big - 3000, -1) == (n_result)(sizeof big - 3000));
    CHECK(n_channel_send(c[0], "", 0, -1) == 0);
    CHECK(n_channel_send(c[0], "tail", 4, -1) == 4);
    for (int i = 0; i < 4; i++) sum += (unsigned char)"tail"[i];
    n_handle_close(c[0]);
    CHECK(n_app_result(app, 10000, &status) == 0 && status == 0);
    n_handle_close(app);
    n_handle_close(doc);
    char line[64];
    int n = snprintf(line, sizeof line, "count %lu sum %lu\n", (unsigned long)sizeof big + 4, sum);
    CHECK(loaded(out, line, (size_t)n));
    n_entry_remove(out);
}

int main(int argc, char **argv) {
    char program[4096]; /* argv[0] is as it was typed; an app is launched by its complete path */
    if (n_path_complete(argv[0], program, sizeof program) >= 0) argv[0] = program;
    if (argc > 2 && !strcmp(argv[1], "child")) return child(argv[2]);
    documents(n_temp_folder()); /* the temporary folder, and the user's folder that is kept */
    documents(n_user_folder());
    temporary();
    relative();
    standard_streams(argv[0]);
    printf("stdiotest: %u checks, %u failed\n", checks, failed);
    return failed != 0;
}
