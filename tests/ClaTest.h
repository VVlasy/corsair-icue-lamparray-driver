/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * A ~100-line test harness.
 *
 * The project deliberately has no third-party test dependency: the core is
 * built for kernel mode, CI must work offline, and the assertions needed here
 * are simple.  Tests register themselves at static-init time and main() runs
 * them all.
 */
#ifndef CLA_TEST_H
#define CLA_TEST_H

#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <functional>
#include <type_traits>

namespace clatest {

struct TestCase
{
    std::string Name;
    std::function<void()> Body;
};

inline std::vector<TestCase> &Registry()
{
    static std::vector<TestCase> registry;
    return registry;
}

inline int &FailureCount()
{
    static int failures = 0;
    return failures;
}

inline std::string &CurrentTest()
{
    static std::string current;
    return current;
}

struct Registrar
{
    Registrar(const char *name, std::function<void()> body)
    {
        Registry().push_back(TestCase{name, std::move(body)});
    }
};

inline void ReportFailure(const char *file, int line, const std::string &message)
{
    FailureCount()++;
    std::printf("    FAIL %s:%d\n         %s\n", file, line, message.c_str());
}

inline int RunAll()
{
    int failed = 0;
    for (auto &test : Registry())
    {
        CurrentTest() = test.Name;
        const int before = FailureCount();
        std::printf("[ RUN ] %s\n", test.Name.c_str());
        test.Body();
        if (FailureCount() == before)
        {
            std::printf("[  OK ] %s\n", test.Name.c_str());
        }
        else
        {
            std::printf("[FAILED] %s\n", test.Name.c_str());
            failed++;
        }
    }
    std::printf("\n%zu tests, %d failed, %d assertion failures\n",
                Registry().size(), failed, FailureCount());
    return failed == 0 ? 0 : 1;
}

} // namespace clatest

#define CLA_TEST(name)                                                        \
    static void name();                                                       \
    static clatest::Registrar cla_registrar_##name(#name, name);              \
    static void name()

#define CLA_CHECK(cond)                                                       \
    do {                                                                      \
        if (!(cond)) {                                                        \
            clatest::ReportFailure(__FILE__, __LINE__,                        \
                std::string("expected: ") + #cond);                           \
        }                                                                     \
    } while (0)

#define CLA_CHECK_EQ(actual, expected)                                        \
    do {                                                                      \
        const auto cla_a_ = (actual);                                         \
        const auto cla_e_ = (expected);                                       \
        if (!(cla_a_ == static_cast<std::decay_t<decltype(cla_a_)>>(cla_e_))) {              \
            char cla_buf_[512];                                               \
            std::snprintf(cla_buf_, sizeof(cla_buf_),                         \
                "%s == %s\n         actual:   %lld\n         expected: %lld", \
                #actual, #expected,                                           \
                static_cast<long long>(cla_a_),                               \
                static_cast<long long>(cla_e_));                              \
            clatest::ReportFailure(__FILE__, __LINE__, cla_buf_);             \
        }                                                                     \
    } while (0)

#define CLA_TEST_MAIN() int main() { return clatest::RunAll(); }

#endif /* CLA_TEST_H */
