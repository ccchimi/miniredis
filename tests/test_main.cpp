#include <cstdio>

#include "framework.hpp"

// Runs every test group and reports a summary. Returns non-zero if any check
// failed so that ctest / CI mark the run as a failure.
int main() {
    std::printf("running store tests...\n");
    test_store();
    std::printf("running resp tests...\n");
    test_resp();
    std::printf("running commands tests...\n");
    test_commands();

    std::printf("\n%d checks, %d failures\n", testing::g_checks,
                testing::g_failures);
    if (testing::g_failures == 0) {
        std::printf("OK\n");
        return 0;
    }
    std::printf("FAILED\n");
    return 1;
}
