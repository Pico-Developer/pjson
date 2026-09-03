//
// Copyright 2025 ByteDance Ltd. and/or its affiliates. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//===----------------------------------------------------------------------===//
// A tiny, dependency-free assertion harness for pjson.
//
// Usage:
//   #include "test_harness.h"
//   TEST(my_case) { CHECK(1 + 1 == 2); CHECK_EQ(getX(), 42); }
//   // exactly ONE translation unit provides main():
//   int main() { return pjson_test::run_all(); }
//
// TEST() blocks self-register at static-init time, so tests may be spread
// across any number of .cpp files that all link into a single executable.
// run_all() prints a per-test PASS/FAIL line plus a summary. CTest invokes the
// same executable with --run-test NAME, so it reports every TEST() separately.
//
#ifndef PJSON_TEST_HARNESS_H
#define PJSON_TEST_HARNESS_H

#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace pjson_test {

    // Per-test assertion totals, reset immediately before each registered test runs.
    struct Stats {
        int checks = 0;
        int failures = 0;
    };

    // Counters for the test currently executing.
    inline Stats& current() {
        static Stats s;
        return s;
    }

    // Aggregate failing-check count used as the process exit status for run_all().
    inline int& total_failures() {
        static int n = 0;
        return n;
    }

    typedef void (*TestFn)();

    // Keeps tests in static-registration order for deterministic output.
    struct Registry {
        std::vector<std::pair<std::string, TestFn>> tests;
    };

    // Function-local construction avoids initialization-order dependencies between test files.
    inline Registry& registry() {
        static Registry r;
        return r;
    }

    // TEST() creates one static Registrar whose constructor records the test function.
    struct Registrar {
        Registrar(const char* name, TestFn fn) {
            registry().tests.push_back(std::make_pair(std::string(name), fn));
        }
    };

    // Stringifies assertion operands through operator<<. CHECK_EQ/CHECK_NE
    // therefore require stream-insertable operand types.
    template <typename T> inline std::string to_str(const T& v) {
        std::ostringstream os;
        os << v;
        return os.str();
    }
    inline std::string to_str(bool v) {
        return v ? "true" : "false";
    }
    inline std::string to_str(const std::string& v) {
        return "\"" + v + "\"";
    }
    inline std::string to_str(std::nullptr_t) {
        return "nullptr";
    }

    // Records a failed check and emits its source location plus optional value details.
    inline void report_failure(const char* file, int line, const char* expr,
                               const std::string& detail = std::string()) {
        current().failures += 1;
        if (detail.empty()) {
            std::printf("    FAIL %s:%d  %s\n", file, line, expr);
        } else {
            std::printf("    FAIL %s:%d  %s  [%s]\n", file, line, expr, detail.c_str());
        }
    }

    // Runs the registry in order and returns the number of failing checks.
    inline int run_all() {
        total_failures() = 0;
        int failed_tests = 0;
        for (size_t i = 0; i < registry().tests.size(); ++i) {
            current() = Stats();
            registry().tests[i].second();
            bool ok = current().failures == 0;
            std::printf("[%s] %s  (%d checks)\n", ok ? "PASS" : "FAIL",
                        registry().tests[i].first.c_str(), current().checks);
            if (!ok) {
                failed_tests += 1;
                total_failures() += current().failures;
            }
        }
        std::printf("----------------------------------\n");
        std::printf("%zu tests, %d failed, %d failing checks\n", registry().tests.size(),
                    failed_tests, total_failures());
        return total_failures();
    }

    // CTest discovery support: print one registered name per line.
    inline int list_tests() {
        for (size_t i = 0; i < registry().tests.size(); ++i) {
            std::printf("%s\n", registry().tests[i].first.c_str());
        }
        return 0;
    }

    // Runs exactly one registered test. This preserves the same diagnostics as
    // run_all() while letting CTest report every TEST() as its own test case.
    inline int run_one(const std::string& name) {
        for (size_t i = 0; i < registry().tests.size(); ++i) {
            if (registry().tests[i].first != name)
                continue;
            current() = Stats();
            total_failures() = 0;
            registry().tests[i].second();
            const bool ok = current().failures == 0;
            std::printf("[%s] %s  (%d checks)\n", ok ? "PASS" : "FAIL", name.c_str(),
                        current().checks);
            return current().failures;
        }
        std::fprintf(stderr, "Unknown test: %s\n", name.c_str());
        return 2;
    }

} // namespace pjson_test

#define PJSON_TOKENPASTE(a, b) a##b
#define PJSON_TOKENPASTE2(a, b) PJSON_TOKENPASTE(a, b)

#define TEST(name)                                                                                 \
    static void name();                                                                            \
    static ::pjson_test::Registrar PJSON_TOKENPASTE2(reg_, name)(#name, name);                     \
    static void name()

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        ::pjson_test::current().checks += 1;                                                       \
        if (!(expr)) {                                                                             \
            ::pjson_test::report_failure(__FILE__, __LINE__, #expr);                               \
        }                                                                                          \
    } while (0)

#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        ::pjson_test::current().checks += 1;                                                       \
        auto _pa = (a);                                                                            \
        auto _pb = (b);                                                                            \
        if (!(_pa == _pb)) {                                                                       \
            ::pjson_test::report_failure(__FILE__, __LINE__, #a " == " #b,                         \
                                         ::pjson_test::to_str(_pa) + " vs " +                      \
                                             ::pjson_test::to_str(_pb));                           \
        }                                                                                          \
    } while (0)

#define CHECK_NE(a, b)                                                                             \
    do {                                                                                           \
        ::pjson_test::current().checks += 1;                                                       \
        auto _pa = (a);                                                                            \
        auto _pb = (b);                                                                            \
        if (!(_pa != _pb)) {                                                                       \
            ::pjson_test::report_failure(__FILE__, __LINE__, #a " != " #b,                         \
                                         ::pjson_test::to_str(_pa) + " vs " +                      \
                                             ::pjson_test::to_str(_pb));                           \
        }                                                                                          \
    } while (0)

// Asserts that parsing aStr fails (returns an error) without throwing.
#define CHECK_PARSE_FAILS(aStr)                                                                    \
    do {                                                                                           \
        ::pjson_test::current().checks += 1;                                                       \
        ByteDance::pJsonParser::Error _e;                                                          \
        ByteDance::pjson _p = ByteDance::pJsonParser().parse(aStr, _e);                            \
        if (_e.ok) {                                                                               \
            ::pjson_test::report_failure(__FILE__, __LINE__, "parse(" #aStr ") should fail",       \
                                         "parsed to: " + _p.toString());                           \
        }                                                                                          \
    } while (0)

#endif // PJSON_TEST_HARNESS_H
