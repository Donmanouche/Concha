#pragma once

// Minimal test harness: TEST(name) { CHECK(cond); } and RUN_TESTS() in main.
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace check {
struct Test {
    const char* name;
    std::function<void()> fn;
};
inline std::vector<Test>& registry()
{
    static std::vector<Test> tests;
    return tests;
}
inline int& failures()
{
    static int n = 0;
    return n;
}
struct Register {
    Register(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};
inline int run()
{
    int failedTests = 0;
    for (const auto& t : registry()) {
        const int before = failures();
        t.fn();
        const bool ok = failures() == before;
        if (!ok) ++failedTests;
        std::printf("%s %s\n", ok ? "  ok  " : "FAILED", t.name);
    }
    std::printf("\n%zu tests, %d failed, %d failed checks\n", registry().size(), failedTests, failures());
    return failedTests ? 1 : 0;
}
} // namespace check

#define CHECK_CAT2(a, b) a##b
#define CHECK_CAT(a, b) CHECK_CAT2(a, b)
#define TEST(name)                                                                                                \
    static void CHECK_CAT(test_, __LINE__)();                                                                     \
    static check::Register CHECK_CAT(reg_, __LINE__)(name, &CHECK_CAT(test_, __LINE__));                          \
    static void CHECK_CAT(test_, __LINE__)()
#define CHECK(cond)                                                                                               \
    do {                                                                                                          \
        if (!(cond)) {                                                                                            \
            ++check::failures();                                                                                  \
            std::printf("    %s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond);                                     \
        }                                                                                                         \
    } while (0)
#define CHECK_NEAR(a, b, eps) CHECK(((a) - (b)) <= (eps) && ((b) - (a)) <= (eps))
#define RUN_TESTS() check::run()
