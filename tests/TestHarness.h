#pragma once

// Minimal Floar test harness (no external framework).
//
//   FLOAR_TEST(Suite_Name) { FLOAR_CHECK(x == 1); FLOAR_REQUIRE(ptr != nullptr); }
//
// FLOAR_CHECK records a failure and continues; FLOAR_REQUIRE records a failure and
// aborts the current test. TestMain.cpp runs every test whose name starts with
// the optional command-line prefix and returns non-zero on any failure.

#include <functional>
#include <vector>

namespace FloarTest
{
    using TestFn = void (*)();

    struct TestCase
    {
        const char* name { nullptr };
        TestFn fn { nullptr };
    };

    std::vector<TestCase>& Registry();

    struct Registrar
    {
        Registrar(const char* name, TestFn fn)
        {
            Registry().push_back({ name, fn });
        }
    };

    struct RequireFailed
    {
    };

    void ReportFailure(const char* file, int line, const char* expression);

    // Runs once after all selected tests.
    void RegisterTeardown(std::function<void()> teardown);
}

#define FLOAR_TEST(name) \
    static void name(); \
    static const FloarTest::Registrar name##_registrar(#name, &name); \
    static void name()

#define FLOAR_CHECK(expr) \
    do { \
        if (!(expr)) \
        { \
            FloarTest::ReportFailure(__FILE__, __LINE__, #expr); \
        } \
    } while (0)

#define FLOAR_REQUIRE(expr) \
    do { \
        if (!(expr)) \
        { \
            FloarTest::ReportFailure(__FILE__, __LINE__, #expr); \
            throw FloarTest::RequireFailed {}; \
        } \
    } while (0)
