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
// PJSON-SEC-001 and PJSON-PARSE-001: an arbitrarily large configured maxDepth
// (up to INT_MAX) must not exhaust the native stack, and all parser front ends
// (string, byte span, DOM stream, buffered SAX, incremental SAX) must agree on
// acceptance and rejection for the same input and options.
//
#include "pjson.h"
#include "test_harness.h"
#include "test_util.h"

#include <climits>
#include <sstream>
#include <string>

using namespace ByteDance;

namespace {

    // Builds N nested arrays: "[[[...]]]" with matching closers.
    std::string nestedArrays(size_t depth) {
        return std::string(depth, '[') + std::string(depth, ']');
    }

    // Counts container-start events so we can compare SAX front ends.
    struct CountingHandler : pjson::SaxHandler {
        size_t starts = 0;
        bool onStartArray() override {
            ++starts;
            return true;
        }
    };

} // namespace

//===----------------------------------------------------------------------===//
// A huge configured maxDepth is clamped to a stack-safe hard ceiling: extreme
// nesting returns a resource-limit error rather than overflowing the stack.
//===----------------------------------------------------------------------===//
TEST(depth_limit_intmax_is_clamped_and_safe) {
    pjson::ParseOptions opt;
    opt.maxDepth = INT_MAX; // caller requests effectively unlimited depth

    // 100,000 levels is far beyond any safe native-recursion ceiling. With the
    // clamp in place this must fail cleanly (empty result) instead of crashing.
    const std::string doc = nestedArrays(100000);
    pjson::ParseError err;
    pjson_test::Parsed p = pjson_test::parse(doc, err, opt);
    CHECK(p == nullptr);
    CHECK(!err.ok);
}

//===----------------------------------------------------------------------===//
// The same clamp protects the SAX front end.
//===----------------------------------------------------------------------===//
TEST(depth_limit_intmax_is_clamped_for_sax) {
    pjson::ParseOptions opt;
    opt.maxDepth = INT_MAX;

    const std::string doc = nestedArrays(100000);
    CountingHandler handler;
    pjson::ParseError err;
    const bool ok = pjson::parseSax(doc, handler, err, opt);
    CHECK(!ok);
    CHECK(!err.ok);
}

//===----------------------------------------------------------------------===//
// A streaming SAX parse over the same input is also protected.
//===----------------------------------------------------------------------===//
TEST(depth_limit_intmax_is_clamped_for_stream_sax) {
    pjson::ParseOptions opt;
    opt.maxDepth = INT_MAX;

    const std::string doc = nestedArrays(100000);
    std::istringstream in(doc);
    CountingHandler handler;
    pjson::ParseError err;
    const bool ok = pjson::parseSaxStream(in, handler, err, opt);
    CHECK(!ok);
    CHECK(!err.ok);
}

//===----------------------------------------------------------------------===//
// Front-end equivalence: string, byte span, DOM stream, buffered SAX, and
// streaming SAX must agree on accepting a representative document.
//===----------------------------------------------------------------------===//
TEST(parser_front_ends_agree_on_acceptance) {
    const std::string doc = "{\"a\":[1,2,3],\"b\":{\"c\":true},\"n\":18446744073709551615}";

    pjson_test::Parsed fromString = pjson_test::parse(doc);
    pjson_test::Parsed fromSpan = pjson_test::parse(doc.data(), doc.size());
    std::istringstream in(doc);
    pjson_test::Parsed fromStream = pjson_test::parseStream(in);

    CHECK(fromString != nullptr);
    CHECK(fromSpan != nullptr);
    CHECK(fromStream != nullptr);
    if (fromString && fromSpan)
        CHECK(*fromString == *fromSpan);
    if (fromString && fromStream)
        CHECK(*fromString == *fromStream);

    CountingHandler bufferHandler;
    CHECK(pjson::parseSax(doc, bufferHandler));
    std::istringstream saxStream(doc);
    CountingHandler streamHandler;
    CHECK(pjson::parseSaxStream(saxStream, streamHandler));
    // Both SAX front ends see the same array/object structure.
    CHECK_EQ(bufferHandler.starts, streamHandler.starts);
}

//===----------------------------------------------------------------------===//
// Front-end equivalence on rejection: an out-of-range integer token is rejected
// by every front end under the default number policy.
//===----------------------------------------------------------------------===//
TEST(parser_front_ends_agree_on_rejection) {
    const std::string doc = "18446744073709551616"; // UINT64_MAX + 1

    CHECK(pjson_test::parse(doc) == nullptr);
    CHECK(pjson_test::parse(doc.data(), doc.size()) == nullptr);
    std::istringstream in(doc);
    CHECK(pjson_test::parseStream(in) == nullptr);

    CountingHandler h1;
    CHECK(!pjson::parseSax(doc, h1));
    std::istringstream saxStream(doc);
    CountingHandler h2;
    CHECK(!pjson::parseSaxStream(saxStream, h2));
}
