#ifndef SESSIONSCOPE_TEST_SUPPORT_H
#define SESSIONSCOPE_TEST_SUPPORT_H

// A tiny test helper, no framework. A failed check prints where it failed and the test carries on,
// so one run shows every problem. End main() with `return finish_tests();`.

#include <cmath>
#include <cstdio>

namespace sessionscope {
namespace test {

inline int g_checks = 0;
inline int g_failures = 0;

inline void check_failed(const char *file, int line, const char *expression) {
    ++g_failures;
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
}

}  // namespace test
}  // namespace sessionscope

#define CHECK(expression) \
    do { \
        ++sessionscope::test::g_checks; \
        if (!(expression)) sessionscope::test::check_failed(__FILE__, __LINE__, #expression); \
    } while (0)

// Two NaNs count as equal: NaN is how the engine says "unknown".
#define CHECK_NEAR(a, b, tolerance) \
    do { \
        ++sessionscope::test::g_checks; \
        const bool both_nan = std::isnan(a) && std::isnan(b); \
        if (!both_nan && !(std::fabs((a) - (b)) <= (tolerance))) { \
            sessionscope::test::check_failed(__FILE__, __LINE__, #a " ~ " #b); \
        } \
    } while (0)

inline int finish_tests() {
    std::printf("%d checks, %d failed\n", sessionscope::test::g_checks, sessionscope::test::g_failures);
    return sessionscope::test::g_failures ? 1 : 0;
}

#endif  // SESSIONSCOPE_TEST_SUPPORT_H
