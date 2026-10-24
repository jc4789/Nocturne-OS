// cxxtest: the C++ toolchain inside Nocturne (clang++, libc++, libc++abi and libunwind of the SDK),
// built with clang++ -std=c++23 -o .\cxxtest C:\Tests\cxxtest.cpp. Prints "FAIL ..." lines and
// "cxxtest: ok".
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <random>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

static int failed;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            std::cout << "FAIL " << __FILE__ << ':' << __LINE__ << ": " #c "\n";                   \
            failed++;                                                                              \
        }                                                                                          \
    } while (0)

// A global object's constructor runs before main.
static struct Early {
    int value;
    Early() : value(42) {}
} early;

static thread_local std::string thread_name = "main";

static void containers() {
    std::vector<int> v(100);
    std::iota(v.begin(), v.end(), 1);
    CHECK(std::accumulate(v.begin(), v.end(), 0) == 5050);
    std::sort(v.begin(), v.end(), std::greater<>());
    CHECK(v.front() == 100 && v.back() == 1);
    std::map<std::string, int> m{{"b", 2}, {"a", 1}, {"c", 3}};
    CHECK(m.begin()->first == "a" && m.rbegin()->second == 3);
    std::unordered_map<int, std::string> u;
    for (int i = 0; i < 1000; i++) u[i] = std::to_string(i * i);
    CHECK(u.size() == 1000 && u[31] == "961");
    std::variant<int, std::string> var = std::string("text");
    CHECK(std::visit([](auto &&x) { return sizeof x > 0; }, var) && std::holds_alternative<std::string>(var));
    std::optional<int> none, some = 7;
    CHECK(!none && some.value_or(0) == 7);
}

static void text() {
    CHECK(std::format("{:>5}|{:x}|{:.3f}", 42, 255, 3.14159) == "   42|ff|3.142");
    std::ostringstream out;
    out << 1.5 << ' ' << -3 << ' ' << std::hex << 255;
    CHECK(out.str() == "1.5 -3 ff");
    char buffer[32];
    auto [end, error] = std::to_chars(buffer, buffer + sizeof buffer, 0.1);
    CHECK(error == std::errc() && std::string(buffer, end) == "0.1");
    double parsed = 0;
    std::from_chars(buffer, end, parsed);
    CHECK(parsed == 0.1);
    CHECK(std::regex_match("abc123", std::regex("[a-z]+([0-9]+)")));
    std::smatch match;
    std::string s = "key=value";
    CHECK(std::regex_search(s, match, std::regex("(\\w+)=(\\w+)")) && match[2] == "value");
    std::wstring w = L"\u00e4\u00f6x";
    CHECK(w.size() == 3 && std::towupper(w[0]) == L'\u00c4' && std::iswalpha(w[1]));
}

static void threads() {
    std::mutex lock;
    std::condition_variable ready;
    long total = 0;
    int started = 0;
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; t++) {
        workers.emplace_back([&, t] {
            thread_name = "worker";
            {
                std::lock_guard<std::mutex> guard(lock);
                started++;
            }
            ready.notify_one();
            for (int i = 0; i < 10000; i++) {
                std::lock_guard<std::mutex> guard(lock);
                total += t + 1;
            }
        });
    }
    {
        std::unique_lock<std::mutex> guard(lock);
        ready.wait(guard, [&] { return started == 4; });
    }
    for (auto &w : workers) w.join();
    CHECK(total == 10000L * (1 + 2 + 3 + 4));
    CHECK(thread_name == "main");
    std::atomic<int> counter{0};
    std::jthread a([&] { counter += 5; }), b([&] { counter += 6; });
    a.join();
    b.join();
    CHECK(counter == 11);
}

static void timing() {
    auto start = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto waited = std::chrono::steady_clock::now() - start;
    CHECK(waited >= std::chrono::milliseconds(20) && waited < std::chrono::seconds(5));
    CHECK(std::chrono::system_clock::now().time_since_epoch() > std::chrono::hours(24 * 365 * 50));
    std::mt19937 random(5489);
    CHECK(random() == 3499211612u);
}

static void files() {
    namespace fs = std::filesystem;
    fs::path folder = fs::current_path() / "cxxtest-folder";
    fs::remove_all(folder);
    CHECK(fs::create_directory(folder));
    {
        std::ofstream out(folder / "a.txt");
        out << "first line\nsecond line\n";
    }
    CHECK(fs::file_size(folder / "a.txt") == 23);
    {
        std::ifstream in(folder / "a.txt");
        std::string first, second;
        std::getline(in, first);
        std::getline(in, second);
        CHECK(first == "first line" && second == "second line");
    }
    fs::rename(folder / "a.txt", folder / "b.txt");
    CHECK(!fs::exists(folder / "a.txt") && fs::is_regular_file(folder / "b.txt"));
    fs::create_directory(folder / "inner");
    int entries = 0;
    for (const auto &entry : fs::directory_iterator(folder)) entries += entry.exists();
    CHECK(entries == 2);
    CHECK(fs::remove_all(folder) == 3 && !fs::exists(folder));
}

struct Counted {
    static inline int alive = 0;
    Counted() { alive++; }
    ~Counted() { alive--; }
};

static void exceptions() {
    // A throw unwinds through frames with destructors to the handler of its type.
    int caught = 0;
    try {
        Counted local;
        auto throwing = [] {
            Counted inner;
            throw std::runtime_error("unwound");
        };
        throwing();
    } catch (const std::logic_error &) {
        caught = 1;
    } catch (const std::exception &e) {
        caught = std::string(e.what()) == "unwound" ? 2 : 3;
    }
    CHECK(caught == 2 && Counted::alive == 0);
    // The library throws too.
    try {
        (void)std::vector<int>().at(1);
        CHECK(!"at() returned");
    } catch (const std::out_of_range &) {
    }
    std::exception_ptr saved;
    try {
        throw 17;
    } catch (...) {
        saved = std::current_exception();
    }
    try {
        std::rethrow_exception(saved);
    } catch (int value) {
        CHECK(value == 17);
    }
    // Each thread unwinds its own stack.
    std::atomic<int> threads_caught{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; t++)
        workers.emplace_back([&, t] {
            try {
                throw std::to_string(t);
            } catch (const std::string &) {
                threads_caught++;
            }
        });
    for (auto &w : workers) w.join();
    CHECK(threads_caught == 4);
}

int main() {
    CHECK(early.value == 42);
    auto shared = std::make_shared<int>(3);
    std::weak_ptr<int> weak = shared;
    CHECK(weak.lock() && *weak.lock() == 3);
    shared.reset();
    CHECK(weak.expired());
    containers();
    text();
    threads();
    timing();
    files();
    exceptions();
    if (!failed) std::cout << "cxxtest: ok" << std::endl;
    return failed != 0;
}
