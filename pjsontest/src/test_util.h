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
// Shared helpers for the pjson test suite.
//
#ifndef PJSON_TEST_UTIL_H
#define PJSON_TEST_UTIL_H

#include "pjson.h"
#include "test_harness.h"

#include <string>

namespace pjson_test {

    // Parses via the public API and returns the owning unique_ptr, so tests read
    // naturally and never leak even on a failed assertion.
    inline ByteDance::pjson::unique_ptr parse(const std::string& s) {
        return ByteDance::pjson::parse(s);
    }

    // Length-aware counterpart used for embedded-NUL and truncated-buffer cases.
    inline ByteDance::pjson::unique_ptr parse(const char* s, size_t n) {
        return ByteDance::pjson::parse(s, n);
    }

    inline int64_t valueInt(const ByteDance::pjson& value) {
        int64_t result = 0;
        CHECK(value.tryGet(result));
        return result;
    }

    inline double valueDouble(const ByteDance::pjson& value) {
        double result = 0.0;
        CHECK(value.tryGet(result));
        return result;
    }

    inline bool valueBool(const ByteDance::pjson& value) {
        bool result = false;
        CHECK(value.tryGet(result));
        return result;
    }

    inline std::string valueString(const ByteDance::pjson& value) {
        std::string result;
        CHECK(value.tryGet(result));
        return result;
    }

} // namespace pjson_test

#endif // PJSON_TEST_UTIL_H
