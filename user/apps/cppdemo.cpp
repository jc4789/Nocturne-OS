/* A host-built C++20 application using the real libc++ and the interface engine (libc/ui.c). */
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
#include <ui.h>

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

constexpr uint32_t WINDOW = 1;
uint32_t add_button; /* the button's place in the view */

struct SessionCloser { void operator()(ui_session *s) const { if (s) ui_session_free(s); } };

/* The window as widgets of the interface engine (libc/ui.c). */
void show(ui_session *s, const std::vector<int> &values) {
    char buffer[100];
    snprintf(buffer, sizeof buffer, "追加した数：%zu    合計：%d", values.size(), std::accumulate(values.begin(), values.end(), 0));
    std::string line(buffer);
    ui_msg m;
    ui_view(&m, WINDOW);
    ui_node(&m, "column"); ui_num(&m, "padding", 24); ui_num(&m, "gap", 12);
        ui_node(&m, "text"); ui_str(&m, "text", global.title.c_str()); ui_str(&m, "style", "title"); ui_end(&m);
        ui_node(&m, "text"); ui_str(&m, "text", "ホストclangで構築したC++20アプリ"); ui_str(&m, "style", "caption"); ui_end(&m);
        ui_node(&m, "card"); ui_num(&m, "padding", 16);
            ui_node(&m, "text"); ui_str(&m, "text", line.c_str()); ui_end(&m);
        ui_end(&m);
        ui_node(&m, "text"); ui_str(&m, "text", "Enterかボタンで追加、Escで終了"); ui_str(&m, "style", "caption"); ui_end(&m);
        ui_node(&m, "row");
            add_button = ui_node(&m, "button");
            ui_str(&m, "text", "数を追加"); ui_str(&m, "variant", "primary"); ui_str(&m, "icon", "plus");
            ui_num(&m, "width", 220); ui_num(&m, "height", 44);
            ui_end(&m);
        ui_end(&m);
    ui_end(&m);
    ui_send(s, &m);
}
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--self-test")) return runtime_check();
    std::unique_ptr<ui_session, SessionCloser> session(ui_session_new());
    if (!session || ui_window(session.get(), WINDOW, 680, 280, 0, "Nocturne C++", nullptr, nullptr) < 0) return 1;
    std::vector<int> values{1, 2, 3};
    show(session.get(), values);
    puts("CPP_GUI_READY");
    for (bool running = true; running && ui_session_open(session.get());) {
        ui_session_wait(session.get(), -1);
        ui_event event{};
        char text[16];
        while (ui_next_event(session.get(), &event, text, sizeof text)) {
            bool add = false;
            if (event.type == UI_EVENT_CLOSE) running = false;
            else if (event.type == UI_EVENT_KEY && !(event.flags & UI_UP)) {
                if (!strcmp(event.text, "Escape")) running = false;
                else if (!strcmp(event.text, "Enter")) add = true;
            } else if (event.type == UI_EVENT_CLICK && event.node == add_button) {
                add = true;
            }
            if (add) {
                values.push_back(static_cast<int>(values.size()) + 1);
                printf("CPP_GUI_ADD count=%zu sum=%d\n", values.size(), std::accumulate(values.begin(), values.end(), 0));
                show(session.get(), values);
            }
        }
    }
    puts("CPP_GUI_CLOSED");
    return 0;
}
