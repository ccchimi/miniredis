#include "framework.hpp"

#include <cstdio>

namespace testing {

int g_checks = 0;
int g_failures = 0;

void report(bool ok, const char* expr, const char* file, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s:%d  CHECK(%s)\n", file, line, expr);
    }
}

void reportEq(const std::string& a, const std::string& b, const char* aExpr,
              const char* bExpr, const char* file, int line) {
    ++g_checks;
    if (a != b) {
        ++g_failures;
        std::printf("  FAIL  %s:%d  CHECK_EQ(%s, %s)\n", file, line, aExpr,
                    bExpr);
        std::printf("          left  = \"%s\"\n", a.c_str());
        std::printf("          right = \"%s\"\n", b.c_str());
    }
}

}  // namespace testing
