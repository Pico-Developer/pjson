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
// PJSON-API-005 and PJSON-PARSE-002: the structured ParseError::Code categories
// and early, pre-allocation duplicate-key detection.
//
#include "pjson.h"
#include "test_harness.h"
#include "test_util.h"

#include <sstream>
#include <string>

using namespace ByteDance;

namespace {

    struct AcceptingSaxHandler : pjson::SaxHandler {};

    pjson::ParseError::Code codeOf(const std::string& doc,
                                   const pjson::ParseOptions& opt = pjson::ParseOptions()) {
        pjson::ParseError err;
        pjson_test::parse(doc, err, opt);
        return err.code;
    }

} // namespace

//===----------------------------------------------------------------------===//
// Each failure class maps to its stable ParseError::Code.
//===----------------------------------------------------------------------===//
TEST(error_codes_classify_failure_categories) {
    CHECK_EQ(codeOf("[1,2,]"), pjson::ParseError::Syntax);
    CHECK_EQ(codeOf("\"\\uD800\""), pjson::ParseError::InvalidEncoding); // lone surrogate
    CHECK_EQ(codeOf("{\"a\":1,\"a\":2}"), pjson::ParseError::DuplicateKey);
    CHECK_EQ(codeOf("18446744073709551616"), pjson::ParseError::NumberRange); // > UINT64_MAX

    pjson::ParseOptions depth;
    depth.maxDepth = 2;
    CHECK_EQ(codeOf("[[[1]]]", depth), pjson::ParseError::DepthLimit);

    pjson::ParseOptions input;
    input.maxInputBytes = 3;
    CHECK_EQ(codeOf("[1, 2, 3]", input), pjson::ParseError::InputLimit);

    pjson::ParseOptions nodes;
    nodes.maxNodes = 1;
    CHECK_EQ(codeOf("[1, 2, 3]", nodes), pjson::ParseError::NodeLimit);
}

//===----------------------------------------------------------------------===//
// A successful parse leaves the success code and coordinates.
//===----------------------------------------------------------------------===//
TEST(error_code_success_state) {
    pjson::ParseError err;
    pjson_test::Parsed p = pjson_test::parse("{\"a\":1}", err);
    CHECK(p != nullptr);
    CHECK(err.ok);
    CHECK_EQ(err.code, pjson::ParseError::None);
    CHECK_EQ(err.line, size_t(1));
    CHECK_EQ(err.column, size_t(1));
}

//===----------------------------------------------------------------------===//
// A null input pointer is reported as an invalid-argument category.
//===----------------------------------------------------------------------===//
TEST(error_code_null_input_is_invalid_argument) {
    pjson::ParseError err;
    pjson_test::parse(static_cast<const char*>(nullptr), 5, err);
    CHECK(!err.ok);
    CHECK_EQ(err.code, pjson::ParseError::InvalidArgument);

    AcceptingSaxHandler handler;
    CHECK(!pjson::parseSax(static_cast<const char*>(nullptr), 5, handler, err));
    CHECK(!err.ok);
    CHECK_EQ(err.code, pjson::ParseError::InvalidArgument);
}

//===----------------------------------------------------------------------===//
// PJSON-PARSE-002: a rejected duplicate is reported at the duplicate key's own
// offset, and its (potentially large) value subtree is never materialized.
//===----------------------------------------------------------------------===//
TEST(duplicate_key_reported_early_at_key_offset) {
    // The duplicate "a" begins at byte offset 8: {"a":1,"a":[...]}
    const std::string doc = "{\"a\":1,\"a\":[1,2,3,4,5,6,7,8,9,10]}";
    pjson::ParseError err;
    pjson_test::Parsed p = pjson_test::parse(doc, err);
    CHECK(p == nullptr);
    CHECK(!err.ok);
    CHECK_EQ(err.code, pjson::ParseError::DuplicateKey);
    // Offset points at the opening quote of the second "a", before its value.
    CHECK_EQ(err.offset, size_t(7));
}

//===----------------------------------------------------------------------===//
// Even a malformed duplicate value under keep-first is still validated (the
// duplicate value is grammar-checked, not silently skipped).
//===----------------------------------------------------------------------===//
TEST(duplicate_keep_first_still_validates_value) {
    pjson::ParseOptions keepFirst;
    keepFirst.duplicateKeys = pjson::ParseOptions::KeepFirstDuplicate;
    // Second "a" has a malformed value; it must still fail.
    pjson::ParseError err;
    pjson_test::Parsed p = pjson_test::parse("{\"a\":1,\"a\":}", err, keepFirst);
    CHECK(p == nullptr);
    CHECK(!err.ok);
}

//===----------------------------------------------------------------------===//
// Embedded-NUL keys are compared on their full decoded bytes for duplicates.
//===----------------------------------------------------------------------===//
TEST(duplicate_key_uses_decoded_length_aware_names) {
    // "a" and "a\u0000b" are distinct, so this is NOT a duplicate.
    pjson_test::Parsed ok = pjson_test::parse("{\"a\":1,\"a\\u0000b\":2}");
    CHECK(ok != nullptr);
    if (ok)
        CHECK_EQ(ok->size(), size_t(2));

    // Two identical embedded-NUL names ARE duplicates.
    pjson::ParseError err;
    pjson_test::Parsed dup = pjson_test::parse("{\"a\\u0000b\":1,\"a\\u0000b\":2}", err);
    CHECK(dup == nullptr);
    CHECK_EQ(err.code, pjson::ParseError::DuplicateKey);
}
