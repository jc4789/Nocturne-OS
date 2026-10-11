/* A host-built C++20 application using the real libc++ and native C GUI ABI. */
#include <algorithm>
#include <atomic>
#include <memory>
#include <new>
#include <numeric>
#include <string>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nocturne.h>

extern "C" int __cxa_atexit(void (*)(void *), void *, void *);
extern "C" void __cxa_finalize(void *);
extern "C" int __cxa_guard_acquire(uint64_t *);
extern "C" void __cxa_guard_release(uint64_t *);
extern "C" void __cxa_guard_abort(uint64_t *);

namespace {
bool self_test, body_passed;
int global_constructed, global_destroyed, local_destroyed, exit_order, exit_count;
void final_check() {
    if (!self_test) return;
    if (!body_passed || global_destroyed != 1 || local_destroyed != 1 || exit_order != 213 || exit_count != 40) {
        printf("CPP_EXIT_FAIL global=%d local=%d order=%d count=%d\n", global_destroyed, local_destroyed, exit_order, exit_count);
        abort();
    }
    puts("CPP_EXIT_PASS");
}
__attribute__((constructor(101))) void early_init() {
    if (atexit(final_check)) abort();
}
struct Global {
    std::string title;
    Global() : title("C++とCの併用") { ++global_constructed; }
    ~Global() { ++global_destroyed; }
} global;

std::atomic<int> constructed{0};
struct Local {
    int value;
    Local() { ++constructed; msleep(20); value = 91; }
    ~Local() { ++local_destroyed; }
};
Local &singleton() { static Local value; return value; }
uint32_t start_workers;
struct Worker { Local *result = nullptr; };
void worker(void *opaque) {
    while (!__atomic_load_n(&start_workers, __ATOMIC_ACQUIRE)) wait_on_address(&start_workers, 0, UINT32_MAX);
    static_cast<Worker *>(opaque)->result = &singleton();
}
void nested_exit() { exit_order = exit_order * 10 + 3; }
void first_exit() { exit_order = exit_order * 10 + 1; if (atexit(nested_exit)) abort(); }
void second_exit(void *) { exit_order = exit_order * 10 + 2; }
void counted_exit() { ++exit_count; }
void dso_callback(void *value) { *static_cast<int *>(value) += 1; }

int runtime_check() {
    self_test = true;
    int failures = 0;
    auto check = [&](const char *name, bool ok) {
        printf("CPP_%s %s\n", ok ? "PASS" : "FAIL", name);
        failures += !ok;
    };
    check("global-constructor-before-main", global_constructed == 1 && global.title.size() > 0);
    std::vector<int> values;
    for (int i = 63; i >= 0; --i) values.push_back(i);
    std::sort(values.begin(), values.end());
    check("libcxx-vector-algorithm", values.size() == 64 && values.front() == 0 && std::accumulate(values.begin(), values.end(), 0) == 2016);
    std::string text(300, 'x'); text += ":Nocturne";
    check("libcxx-string-c-abi", text.size() == 309 && strlen(text.c_str()) == text.size());
    int destroyed = 0;
    struct Owned { int *count; ~Owned() { ++*count; } };
    { auto owned = std::make_unique<Owned>(); owned->count = &destroyed; }
    check("unique-ptr-raii", destroyed == 1);
    struct alignas(512) Aligned { int value = 42; };
    auto aligned = std::make_unique<Aligned>();
    check("overaligned-new-delete", reinterpret_cast<uintptr_t>(aligned.get()) % 512 == 0 && aligned->value == 42);
    auto array = std::make_unique<Aligned[]>(3);
    check("overaligned-array", reinterpret_cast<uintptr_t>(array.get()) % 512 == 0 && array[2].value == 42);
    void *impossible = ::operator new(SIZE_MAX, std::nothrow);
    check("nothrow-allocation-failure", impossible == nullptr);
    if (impossible) ::operator delete(impossible);
    void *zero = ::operator new(0); check("zero-size-new", zero != nullptr); ::operator delete(zero);
    Worker a, b;
    int ta = thread_create(worker, &a), tb = thread_create(worker, &b);
    __atomic_store_n(&start_workers, 1, __ATOMIC_RELEASE); wake_address(&start_workers, UINT32_MAX);
    if (ta >= 0) thread_join(ta);
    if (tb >= 0) thread_join(tb);
    check("native-thread-static-guard", ta >= 0 && tb >= 0 && a.result && a.result == b.result && a.result->value == 91 && constructed == 1);
    uint64_t guard = 0;
    bool acquired = __cxa_guard_acquire(&guard) == 1;
    __cxa_guard_abort(&guard);
    acquired = acquired && __cxa_guard_acquire(&guard) == 1;
    __cxa_guard_release(&guard);
    check("static-guard-abort-retry", acquired && __cxa_guard_acquire(&guard) == 0);
    int token_a, token_b, calls_a = 0, calls_b = 0;
    bool registered = __cxa_atexit(dso_callback, &calls_a, &token_a) == 0 && __cxa_atexit(dso_callback, &calls_b, &token_b) == 0;
    __cxa_finalize(&token_a); __cxa_finalize(&token_a);
    check("dso-finalize-once", registered && calls_a == 1 && calls_b == 0);
    __cxa_finalize(&token_b);
    check("dso-finalize-isolation", calls_b == 1);
    check("c-long-long-division", lldiv(-19, 4).quot == -4 && lldiv(-19, 4).rem == -3);
    check("mixed-exit-registration", atexit(first_exit) == 0 && __cxa_atexit(second_exit, nullptr, nullptr) == 0);
    bool many = true; for (int i = 0; i < 40; ++i) many = (atexit(counted_exit) == 0) && many;
    check("exit-registration-over-sixteen", many);
    body_passed = failures == 0;
    puts(body_passed ? "CPP_SELFTEST_PASS" : "CPP_SELFTEST_FAIL");
    return failures ? 1 : 0;
}

struct WindowCloser { void operator()(window_t *w) const { if (w) win_close(w); } };
void draw(window_t *w, const std::vector<int> &values) {
    gfx_fill(&w->c, 0, 0, w->w, w->h, UI_BG);
    gfx_text(&w->c, 24, 24, global.title.c_str(), UI_FG, TRANSPARENT, FONT_LARGE);
    gfx_text(&w->c, 24, 66, "ホストclangで構築したC++20アプリ", UI_DIM, TRANSPARENT, FONT_SMALL);
    char buffer[100];
    snprintf(buffer, sizeof buffer, "追加した数：%zu    合計：%d", values.size(), std::accumulate(values.begin(), values.end(), 0));
    std::string line(buffer);
    gfx_text(&w->c, 24, 108, line.c_str(), UI_FG, TRANSPARENT, FONT_SMALL);
    gfx_text(&w->c, 24, 148, "Enterかボタンで追加、Escで終了", UI_DIM, TRANSPARENT, FONT_SMALL);
    ui_button(&w->c, 24, 196, 220, 44, "数を追加", false, false);
    win_update(w);
}
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--self-test")) return runtime_check();
    std::unique_ptr<window_t, WindowCloser> window(win_open(680, 280, "Nocturne C++", 0));
    if (!window) return 1;
    std::vector<int> values{1, 2, 3};
    draw(window.get(), values);
    puts("CPP_GUI_READY");
    for (;;) {
        gui_event event{};
        if (win_event(window.get(), &event, -1) < 0 || event.type == EV_CLOSE) break;
        if (event.type == EV_KEY && event.pressed && event.key == NKEY_ESC) break;
        bool add = (event.type == EV_KEY && event.pressed && event.key == NKEY_ENTER)
                   || (event.type == EV_MOUSE_UP && ui_hit(event.x, event.y, 24, 196, 220, 44));
        if (add) {
            values.push_back(static_cast<int>(values.size()) + 1);
            printf("CPP_GUI_ADD count=%zu sum=%d\n", values.size(), std::accumulate(values.begin(), values.end(), 0));
        }
        if (add || event.type == EV_RESIZE) draw(window.get(), values);
    }
    puts("CPP_GUI_CLOSED");
    return 0;
}
