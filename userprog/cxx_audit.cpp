// Comprehensive C++17 / C++20 / C++23 / C++26-preview feature audit. Each
// test prints PASS/FAIL/SKIP plus a short tag so the harness can grep.
// Failures don't abort -- the goal is to see EVERY gap in one run, not to
// halt at the first one.
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <future>
#include <condition_variable>
#include <regex>
#include <filesystem>
#include <variant>
#include <optional>
#include <stdexcept>
#include <exception>
#include <typeinfo>
#include <fstream>
#include <sstream>
#include <functional>
#include <cstring>
#include <cstdio>

// ---- C++20 ----------------------------------------------------------------
#if __cplusplus >= 202002L
#include <concepts>
#include <ranges>
#include <span>
#include <numbers>
#include <bit>
#include <coroutine>
#endif
#if __cpp_lib_format >= 201907L
#include <format>
#endif
#if __cpp_lib_jthread >= 201911L
#include <stop_token>
#endif

// ---- C++23 ----------------------------------------------------------------
#if __cpp_lib_expected >= 202202L
#include <expected>
#endif
#if __cpp_lib_print >= 202207L
#include <print>
#endif
#if __cpp_lib_stacktrace >= 202011L
#include <stacktrace>
#endif
#if __cpp_lib_mdspan >= 202207L
#include <mdspan>
#endif
#if __cpp_lib_flat_map >= 202207L
#include <flat_map>
#endif

namespace fs = std::filesystem;

#define CHECK(tag, cond) do { \
    std::cout << ((cond) ? "PASS " : "FAIL ") << tag << "\n"; \
} while (0)

int g_static_init_calls = 0;
struct StaticInit {
    StaticInit()  { ++g_static_init_calls; }
};
static StaticInit g_static; // global ctor

int magic_statics_counter() {
    static int n = (g_static_init_calls + 1) * 100; // Itanium ABI __cxa_guard
    return n;
}

void test_static_init() {
    CHECK("static.global_ctor_ran", g_static_init_calls == 1);
    CHECK("static.magic_static", magic_statics_counter() == 200);
    CHECK("static.magic_repeats", magic_statics_counter() == 200);
}

void test_iostream() {
    std::ostringstream oss;
    oss << "x=" << 42 << ",pi=" << 3.14;
    CHECK("iostream.ostringstream", oss.str() == "x=42,pi=3.14");

    std::istringstream iss("7 8 9");
    int a, b, c;
    iss >> a >> b >> c;
    CHECK("iostream.istringstream", a == 7 && b == 8 && c == 9);
}

void test_fstream() {
    const char *path = "/tmp/cxx_audit_io.txt";
    {
        std::ofstream out(path);
        if (!out) { CHECK("fstream.ofstream_open", false); return; }
        out << "alpha\nbeta\n";
    }
    std::ifstream in(path);
    std::string line1, line2;
    std::getline(in, line1);
    std::getline(in, line2);
    CHECK("fstream.read_back", line1 == "alpha" && line2 == "beta");
}

void test_stl() {
    std::vector<int> v{3, 1, 4, 1, 5, 9, 2, 6};
    std::sort(v.begin(), v.end());
    CHECK("stl.vector_sort", v.front() == 1 && v.back() == 9);

    std::map<std::string, int> m;
    m["a"] = 1; m["b"] = 2; m["c"] = 3;
    CHECK("stl.map", m["b"] == 2 && m.size() == 3);

    std::unordered_map<int, std::string> um{{1, "one"}, {2, "two"}};
    CHECK("stl.unordered_map", um[1] == "one");
}

void test_rtti() {
    std::vector<std::string> v;
    const std::type_info &t = typeid(v);
    CHECK("rtti.typeid_nonempty", t.name() != nullptr && t.name()[0] != 0);

    struct Base { virtual ~Base() = default; };
    struct Derived : Base { int x = 5; };
    std::unique_ptr<Base> p = std::make_unique<Derived>();
    Derived *d = dynamic_cast<Derived *>(p.get());
    CHECK("rtti.dynamic_cast", d != nullptr && d->x == 5);
}

void test_exceptions() {
    bool caught = false;
    std::string what;
    try {
        throw std::runtime_error("boom");
    } catch (const std::exception &e) {
        caught = true;
        what = e.what();
    }
    CHECK("ex.throw_catch", caught && what == "boom");

    // Nested + rethrow path
    bool nested_ok = false;
    try {
        try {
            throw std::out_of_range("inner");
        } catch (...) {
            std::throw_with_nested(std::runtime_error("outer"));
        }
    } catch (const std::exception &e) {
        try {
            std::rethrow_if_nested(e);
        } catch (const std::out_of_range &inner) {
            nested_ok = std::string(inner.what()) == "inner";
        }
    }
    CHECK("ex.nested_rethrow", nested_ok);
}

void test_atomic() {
    std::atomic<int> n{0};
    n.fetch_add(7, std::memory_order_relaxed);
    int expected = 7;
    bool cas = n.compare_exchange_strong(expected, 100);
    CHECK("atomic.cas", cas && n.load() == 100);
}

void test_thread() {
    std::atomic<int> count{0};
    std::thread t1([&]{ for (int i = 0; i < 1000; ++i) count.fetch_add(1); });
    std::thread t2([&]{ for (int i = 0; i < 1000; ++i) count.fetch_add(1); });
    t1.join();
    t2.join();
    CHECK("thread.join_atomic", count.load() == 2000);
}

void test_mutex_only() {
    // Mutex without cv -- two threads racing a counter through a mutex.
    // If this hangs, mutex itself is broken; if it works, the cv path is.
    std::mutex m;
    int counter = 0;
    std::thread t1([&]{
        for (int i = 0; i < 500; ++i) {
            std::lock_guard<std::mutex> g(m);
            ++counter;
        }
    });
    std::thread t2([&]{
        for (int i = 0; i < 500; ++i) {
            std::lock_guard<std::mutex> g(m);
            ++counter;
        }
    });
    t1.join();
    t2.join();
    CHECK("sync.mutex_only", counter == 1000);
}

void test_mutex_cv() {
    std::mutex m;
    std::condition_variable cv;
    bool ready = false;
    int produced = -1;

    std::thread producer([&]{
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        {
            std::lock_guard<std::mutex> lock(m);
            produced = 99;
            ready = true;
        }
        cv.notify_one();
    });

    std::unique_lock<std::mutex> lock(m);
    cv.wait(lock, [&]{ return ready; });
    producer.join();
    CHECK("sync.mutex_cv", produced == 99);
}

void test_future_async() {
    std::future<int> fut = std::async(std::launch::async, []{
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return 42;
    });
    CHECK("async.future_get", fut.get() == 42);
}

void test_chrono() {
    auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    CHECK("chrono.steady_clock", ms >= 15 && ms < 200);

    auto sysnow = std::chrono::system_clock::now();
    auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
        sysnow.time_since_epoch()).count();
    CHECK("chrono.system_clock", epoch > 1'700'000'000LL); // > 2023
}

void test_thread_local() {
    static thread_local int tls = 0;
    tls = 1;
    std::atomic<int> other_tls{-1};
    std::thread t([&]{
        other_tls = tls; // should be 0 in the other thread
        tls = 999;
    });
    t.join();
    CHECK("tls.per_thread", other_tls.load() == 0 && tls == 1);
}

void test_regex() {
    std::regex re(R"((\w+)=(\d+))");
    std::smatch m;
    std::string s = "x=42";
    bool ok = std::regex_match(s, m, re) && m.size() == 3 &&
              m[1].str() == "x" && m[2].str() == "42";
    CHECK("regex.match", ok);
}

void test_filesystem() {
    fs::path p = "/tmp/cxx_audit_fs";
    std::error_code ec;
    fs::create_directories(p / "sub", ec);
    {
        std::ofstream(p / "sub" / "file.txt") << "data\n";
    }
    bool exists = fs::exists(p / "sub" / "file.txt", ec);
    auto sz = fs::file_size(p / "sub" / "file.txt", ec);
    CHECK("filesystem.create+stat", exists && sz == 5);

    int count = 0;
    for (auto &e : fs::directory_iterator(p / "sub", ec)) {
        (void)e;
        ++count;
    }
    CHECK("filesystem.directory_iter", count == 1);
}

void test_variant_optional() {
    std::variant<int, std::string> vv = 5;
    CHECK("variant.holds_int", std::holds_alternative<int>(vv));
    vv = std::string("hi");
    CHECK("variant.holds_str", std::get<std::string>(vv) == "hi");

    std::optional<int> o;
    CHECK("optional.empty", !o.has_value());
    o = 7;
    CHECK("optional.value", o && *o == 7);
}

void test_lambda_function() {
    std::function<int(int, int)> add = [](int a, int b) { return a + b; };
    CHECK("lambda.std_function", add(3, 4) == 7);

    auto counter = [n = 0]() mutable { return ++n; };
    CHECK("lambda.capture_mutable", counter() == 1 && counter() == 2);
}

// ---- C++20 tests ----------------------------------------------------------

#if __cplusplus >= 202002L
template <typename T>
concept Addable = requires(T a, T b) { { a + b } -> std::same_as<T>; };

template <Addable T>
T add(T a, T b) { return a + b; }

void test_cxx20_concepts() {
    CHECK("cxx20.concepts", add(2, 3) == 5);
}

void test_cxx20_ranges() {
    std::vector<int> v{5, 1, 4, 2, 3};
    auto evens = v | std::views::filter([](int x){ return x % 2 == 0; })
                   | std::views::transform([](int x){ return x * 10; });
    int sum = 0;
    for (int x : evens) sum += x;
    CHECK("cxx20.ranges_pipe", sum == 60); // (4 + 2) * 10
}

void test_cxx20_span() {
    int arr[5] = {10, 20, 30, 40, 50};
    std::span<int> sp(arr);
    int total = 0;
    for (int x : sp) total += x;
    CHECK("cxx20.span", total == 150 && sp.size() == 5);
}

void test_cxx20_bit_numbers() {
    CHECK("cxx20.bit_popcount", std::popcount(0xF0u) == 4);
    CHECK("cxx20.numbers_pi", std::numbers::pi > 3.14 && std::numbers::pi < 3.15);
}

// Minimal coroutine smoke test (synchronous generator returning a single int).
struct Task {
    struct promise_type {
        int value = 0;
        Task get_return_object() { return Task{this}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_value(int v) noexcept { value = v; }
        void unhandled_exception() {}
    };
    promise_type *p;
    int get() { return p->value; }
};

Task make_seven() { co_return 7; }

void test_cxx20_coroutine() {
    auto t = make_seven();
    CHECK("cxx20.coroutine_basic", t.get() == 7);
}
#endif

#if __cpp_lib_format >= 201907L
void test_cxx20_format() {
    std::string s = std::format("{}+{}={}", 2, 3, 2 + 3);
    CHECK("cxx20.std_format", s == "2+3=5");
}
#else
void test_cxx20_format() { std::cout << "SKIP cxx20.std_format (no <format>)\n"; }
#endif

#if __cpp_lib_jthread >= 201911L
void test_cxx20_jthread() {
    std::atomic<int> reached{0};
    {
        std::jthread t([&reached](std::stop_token st){
            for (int i = 0; i < 1000 && !st.stop_requested(); ++i) {
                reached.fetch_add(1);
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        t.request_stop();
    } // jthread dtor joins
    CHECK("cxx20.jthread_stop", reached.load() > 0);
}
#else
void test_cxx20_jthread() { std::cout << "SKIP cxx20.jthread (no <stop_token>)\n"; }
#endif

// ---- C++23 tests ----------------------------------------------------------

#if __cpp_lib_expected >= 202202L
std::expected<int, std::string> parse_int(const std::string &s) {
    if (s.empty()) return std::unexpected("empty");
    int n = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return std::unexpected("not_digit");
        n = n * 10 + (c - '0');
    }
    return n;
}

void test_cxx23_expected() {
    auto ok = parse_int("42");
    auto bad = parse_int("4x");
    CHECK("cxx23.expected_ok", ok && *ok == 42);
    CHECK("cxx23.expected_err", !bad && bad.error() == "not_digit");
}
#else
void test_cxx23_expected() { std::cout << "SKIP cxx23.expected (no <expected>)\n"; }
#endif

#if __cpp_lib_print >= 202207L
void test_cxx23_print() {
    // std::print writes directly to stdout via std::format machinery.
    // We can't capture it for a content check, but the call going through
    // without crashing exercises the same format path.
    std::print("PASS cxx23.print_hello={}\n", 99);
}
#else
void test_cxx23_print() { std::cout << "SKIP cxx23.print (no <print>)\n"; }
#endif

#if __cpp_lib_stacktrace >= 202011L
void test_cxx23_stacktrace() {
    auto st = std::stacktrace::current();
    CHECK("cxx23.stacktrace", st.size() > 0);
}
#else
void test_cxx23_stacktrace() { std::cout << "SKIP cxx23.stacktrace (no <stacktrace>)\n"; }
#endif

#if __cpp_lib_mdspan >= 202207L
void test_cxx23_mdspan() {
    int data[6] = {1, 2, 3, 4, 5, 6};
    using extents_t = std::extents<size_t, 2, 3>;
    std::mdspan<int, extents_t> m(data);
    const bool ok = (m[1, 2] == 6);
    CHECK("cxx23.mdspan", ok);
}
#else
void test_cxx23_mdspan() { std::cout << "SKIP cxx23.mdspan (no <mdspan>)\n"; }
#endif

// ---- C++26 preview --------------------------------------------------------
// Most C++26 features are still gated behind __cpp_lib_* macros that won't
// trip on libc++ 21. We test what compiles and gracefully skip the rest.

void test_cxx26_misc() {
    // C++26 P2300 std::execution / senders are not in libc++ yet.
    // C++26 reflection is still a proposal.
    // Report which feature-test macros the compiler actually sees so we
    // know what's available.
    std::cout << "INFO __cplusplus=" << __cplusplus << "\n";
#if defined(__cpp_concepts)
    std::cout << "INFO __cpp_concepts=" << __cpp_concepts << "\n";
#endif
#if defined(__cpp_lib_ranges)
    std::cout << "INFO __cpp_lib_ranges=" << __cpp_lib_ranges << "\n";
#endif
#if defined(__cpp_lib_format)
    std::cout << "INFO __cpp_lib_format=" << __cpp_lib_format << "\n";
#endif
#if defined(__cpp_lib_expected)
    std::cout << "INFO __cpp_lib_expected=" << __cpp_lib_expected << "\n";
#endif
#if defined(__cpp_lib_print)
    std::cout << "INFO __cpp_lib_print=" << __cpp_lib_print << "\n";
#endif
#if defined(__cpp_lib_mdspan)
    std::cout << "INFO __cpp_lib_mdspan=" << __cpp_lib_mdspan << "\n";
#endif
#if defined(__cpp_lib_jthread)
    std::cout << "INFO __cpp_lib_jthread=" << __cpp_lib_jthread << "\n";
#endif
#if defined(__cpp_lib_stacktrace)
    std::cout << "INFO __cpp_lib_stacktrace=" << __cpp_lib_stacktrace << "\n";
#endif
}

int main() {
    std::cout << "[cxx_audit start]\n";
    test_static_init();
    test_iostream();
    test_fstream();
    test_stl();
    test_rtti();
    test_exceptions();
    test_atomic();
    test_thread();
    test_mutex_only();
    test_mutex_cv();
    test_future_async();
    test_chrono();
    test_thread_local();
    test_regex();
    test_filesystem();
    test_variant_optional();
    test_lambda_function();
#if __cplusplus >= 202002L
    test_cxx20_concepts();
    test_cxx20_ranges();
    test_cxx20_span();
    test_cxx20_bit_numbers();
    test_cxx20_coroutine();
#endif
    test_cxx20_format();
    test_cxx20_jthread();
    test_cxx23_expected();
    test_cxx23_print();
    test_cxx23_stacktrace();
    test_cxx23_mdspan();
    test_cxx26_misc();
    std::cout << "[cxx_audit end]\n";
    return 0;
}
