/*
 * tests/test_framework.h — minimal dependency-free test harness.
 *
 * Runs under plain g++/clang++ (this repo's CI native job) with no external
 * library so tests are reproducible everywhere. Also compatible with a
 * PlatformIO `native` env when invoked via the test runner in tests/main.cpp.
 */
#ifndef SWC_TEST_FRAMEWORK_H
#define SWC_TEST_FRAMEWORK_H

#include <cstdio>
#include <cmath>
#include <cstdint>
#include <vector>
#include <string>
#include <functional>

namespace swctest {

struct Case { std::string name; std::function<void()> fn; };

inline std::vector<Case>& registry() { static std::vector<Case> r; return r; }
inline int& failures() { static int f = 0; return f; }
inline int& checks()   { static int c = 0; return c; }

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

inline void report_fail(const char* file, int line, const std::string& msg) {
    ++failures();
    std::printf("    FAIL %s:%d  %s\n", file, line, msg.c_str());
}

inline int run_all() {
    int failed_cases = 0;
    for (auto& c : registry()) {
        int before = failures();
        std::printf("[ RUN  ] %s\n", c.name.c_str());
        c.fn();
        if (failures() == before) {
            std::printf("[  OK  ] %s\n", c.name.c_str());
        } else {
            std::printf("[ FAIL ] %s\n", c.name.c_str());
            ++failed_cases;
        }
    }
    std::printf("\n==== %zu cases, %d checks, %d failures ====\n",
                registry().size(), checks(), failures());
    return failed_cases == 0 ? 0 : 1;
}

} // namespace swctest

#define TEST(name)                                                            \
    static void name();                                                       \
    static ::swctest::Registrar reg_##name(#name, name);                      \
    static void name()

#define CHECK(cond)                                                           \
    do {                                                                      \
        ++::swctest::checks();                                                 \
        if (!(cond)) ::swctest::report_fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)

#define CHECK_EQ(a, b)                                                        \
    do {                                                                      \
        ++::swctest::checks();                                                 \
        auto _va = (a); auto _vb = (b);                                       \
        if (!(_va == _vb)) {                                                  \
            char _m[256];                                                     \
            std::snprintf(_m, sizeof(_m), "CHECK_EQ(" #a ", " #b ") -> %lld vs %lld", \
                          (long long)_va, (long long)_vb);                    \
            ::swctest::report_fail(__FILE__, __LINE__, _m);                   \
        }                                                                     \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                 \
    do {                                                                      \
        ++::swctest::checks();                                                 \
        double _va = (double)(a); double _vb = (double)(b);                   \
        if (std::fabs(_va - _vb) > (double)(eps)) {                           \
            char _m[256];                                                     \
            std::snprintf(_m, sizeof(_m), "CHECK_NEAR(" #a ", " #b ") -> %.5f vs %.5f", _va, _vb); \
            ::swctest::report_fail(__FILE__, __LINE__, _m);                   \
        }                                                                     \
    } while (0)

#endif // SWC_TEST_FRAMEWORK_H
