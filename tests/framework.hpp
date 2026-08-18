#pragma once

#include <string>

// A deliberately tiny test framework: no external dependencies, so the tests
// build anywhere the core library does. Each CHECK records a pass/fail; main
// prints a summary and returns non-zero if anything failed (which is what lets
// `ctest` and CI report the run as failed).

namespace testing {

extern int g_checks;
extern int g_failures;

void report(bool ok, const char* expr, const char* file, int line);
void reportEq(const std::string& a, const std::string& b, const char* aExpr,
              const char* bExpr, const char* file, int line);

}  // namespace testing

#define CHECK(cond) \
    ::testing::report(static_cast<bool>(cond), #cond, __FILE__, __LINE__)

// Compares two values that are (or convert to) std::string and prints both on
// mismatch, which makes failures far easier to read than a bare CHECK(a == b).
#define CHECK_EQ(a, b)                                                        \
    ::testing::reportEq((a), (b), #a, #b, __FILE__, __LINE__)

// Each test file defines one of these; test_main.cpp runs them all.
void test_store();
void test_resp();
void test_commands();
void test_types();
void test_persistence();
