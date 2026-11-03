/* The entry image and the program's mark (common/abi.h): what a program outside C:\Nocturne can
   reach, that a call must come by its own slot, that the image's place differs by process, and
   that a program of another generation does not start. */
#include <nocturne.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failed;
static void check(const char *what, int ok) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failed++;
}

/* A public call's SYSCALL instruction (the slot's offset 8), with another number in eax. */
static void forge(void) {
    long result;
    __asm__ volatile("call *%%rdx\n"
                     : "=a"(result) : "0"((long)SYS_PROCLIST), "d"(n_entry_slot(SYS_YIELD) + 8), "D"(0L), "S"(0L)
                     : "rcx", "r8", "r9", "r10", "r11", "memory", "cc");
    printf("forged call returned %ld\n", result);
}

static n_result launch(const char *path, const char *arg, n_handle *channel) {
    const char *arguments[2] = {path, arg};
    n_handle ends[2];
    n_result r = n_channel_pair(ends);
    if (r < 0) return r;
    const int32_t handles[] = {-1, 1, 2, ends[1]};
    struct n_launch l = {.arguments = arguments, .argument_count = arg ? 2 : 1, .handles = handles, .handle_count = 4};
    n_handle app = n_app_launch(path, &l);
    n_handle_close(ends[1]);
    if (app < 0) {
        n_handle_close(ends[0]);
        return app;
    }
    if (channel) *channel = ends[0];
    else n_handle_close(ends[0]);
    return app;
}

/* This program's bytes with the note's words changed, as a new document. */
static n_result forged_copy(const char *self, const char *path, unsigned word, uint32_t value) {
    void *data;
    size_t size;
    n_result r = n_document_load(self, &data, &size);
    if (r < 0) return r;
    uint8_t *copy = malloc(size), *bytes = data;
    if (!copy) return -N_ERR_NO_MEMORY;
    memcpy(copy, bytes, size);
    static const char name[] = "Nocturne";
    for (size_t i = 12; i + 20 <= size; i += 4) {
        if (memcmp(copy + i, name, sizeof name)) continue;
        memcpy(copy + i + 12 + word * sizeof(uint32_t), &value, sizeof value);
        break;
    }
    r = n_document_save(path, copy, size);
    free(copy);
    n_memory_release(data, size);
    return r;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "forge")) {
        forge();
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "where")) {
        uintptr_t at = n_entry_slot(0);
        n_channel_send(3, &at, sizeof at, -1);
        return 0;
    }
    char self[1024], twin[1024];
    if (!find_program(argv[0], self, sizeof self)) {
        printf("FAIL cannot find myself\n");
        return 1;
    }
    /* the public image */
    check("an internal call is refused", __syscall(SYS_PROCLIST, 0, 0, 0, 0, 0) == -N_ERR_DENIED);
    check("a public call goes through", __syscall(SYS_APP_SELF, 0, 0, 0, 0, 0) >= 0);
    /* the clocks of the image */
    uint64_t a = n_clock_monotonic(), b = n_clock_monotonic();
    check("the monotonic clock goes on", a && b >= a);
    check("the wall clock is past 2020", n_clock_wall() > 1577836800ll * N_NS_PER_S);
    check("the TSC has a rate", n_clock_tsc_hz() > 1000000);
    /* a call from another slot */
    n_handle app = launch(self, "forge", NULL);
    check("the forging child starts", app >= 0);
    if (app >= 0) {
        int status = 0;
        n_app_result(app, N_CLOCK_NEVER, &status);
        printf("the forging child ended with %d\n", status);
        check("a call from another slot ends the program", status == 128 + 13);
        n_handle_close(app);
    }
    /* the place of the image */
    uintptr_t where[2] = {0};
    for (int i = 0; i < 2; i++) {
        n_handle channel = -1, attached = -1;
        app = launch(self, "where", &channel);
        if (app < 0) continue;
        n_channel_receive(channel, &where[i], sizeof where[i], N_CHANNEL_WAIT, &attached);
        n_handle_close(channel);
        n_app_result(app, N_CLOCK_NEVER, NULL);
        n_handle_close(app);
    }
    printf("the image at %lx and %lx, here %lx\n", (unsigned long)where[0], (unsigned long)where[1], (unsigned long)n_entry_slot(0));
    check("each process has the image at its own place", where[0] && where[1] && where[0] != where[1]);
    /* the program's mark */
    if (n_path_join(n_user_folder(), "entrytest-twin", twin, sizeof twin) < 0) return 1;
    check("the copy is written", forged_copy(self, twin, 0, N_ABI_GENERATION + 1) >= 0);
    n_result r = launch(twin, NULL, NULL);
    printf("another generation: %s\n", r < 0 ? n_result_text(r) : "started");
    check("another generation does not start", r == -N_ERR_WRONG_GENERATION);
    check("the copy is written", forged_copy(self, twin, 1, N_ENTRY_SLOTS + 1) >= 0);
    check("more slots than there are does not start", launch(twin, NULL, NULL) == -N_ERR_WRONG_GENERATION);
    n_entry_remove(twin);
    printf("entrytest: %d failed\n", failed);
    return failed != 0;
}
