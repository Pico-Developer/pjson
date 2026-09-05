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
// Malformed / hostile input: every category of invalid JSON must return null
// without throwing and (where meaningful) report a sensible error offset.
//
#include "pjson.h"
#include "pjson_parser.h"
#include "test_harness.h"
#include "test_util.h"

#include <string>
#include <vector>

using namespace ByteDance;
using pjson_test::parse;

//===----------------------------------------------------------------------===//
// Empty and whitespace-only input.
//===----------------------------------------------------------------------===//
TEST(malformed_empty_inputs) {
    CHECK_PARSE_FAILS("");
    CHECK_PARSE_FAILS(" ");
    CHECK_PARSE_FAILS("\t\n\r ");
    CHECK_PARSE_FAILS("\r\n");
    // A lone NUL byte is not a value.
    CHECK(pjson_test::parse(std::string("\0", 1)) == nullptr);
}

//===----------------------------------------------------------------------===//
// Structural garbage: stray punctuation, mismatched brackets, bare tokens.
//===----------------------------------------------------------------------===//
TEST(malformed_structural_tokens) {
    CHECK_PARSE_FAILS("}");
    CHECK_PARSE_FAILS("]");
    CHECK_PARSE_FAILS(":");
    CHECK_PARSE_FAILS(",");
    CHECK_PARSE_FAILS("[}");
    CHECK_PARSE_FAILS("{]");
    CHECK_PARSE_FAILS("[[]");
    CHECK_PARSE_FAILS("{}}");
    CHECK_PARSE_FAILS("(1)");
    CHECK_PARSE_FAILS("<xml/>");
}

//===----------------------------------------------------------------------===//
// Objects: bad keys, missing colons/values, comma misuse.
//===----------------------------------------------------------------------===//
TEST(malformed_object_shapes) {
    CHECK_PARSE_FAILS("{");
    CHECK_PARSE_FAILS("{\"a\"}");            // key with no colon/value
    CHECK_PARSE_FAILS("{\"a\":}");           // colon, no value
    CHECK_PARSE_FAILS("{\"a\" 1}");          // missing colon
    CHECK_PARSE_FAILS("{\"a\":1\"b\":2}");   // missing comma
    CHECK_PARSE_FAILS("{\"a\":1,}");         // trailing comma
    CHECK_PARSE_FAILS("{,\"a\":1}");         // leading comma
    CHECK_PARSE_FAILS("{\"a\":1,,\"b\":2}"); // doubled comma
    CHECK_PARSE_FAILS("{1:2}");              // non-string key
    CHECK_PARSE_FAILS("{true:2}");           // non-string key
    CHECK_PARSE_FAILS("{\"a\":1");           // unterminated
    CHECK_PARSE_FAILS("{\"a\"::1}");         // doubled colon
}

//===----------------------------------------------------------------------===//
// Arrays: comma misuse, unterminated, missing separators.
//===----------------------------------------------------------------------===//
TEST(malformed_array_shapes) {
    CHECK_PARSE_FAILS("[");
    CHECK_PARSE_FAILS("[1");
    CHECK_PARSE_FAILS("[1,");
    CHECK_PARSE_FAILS("[1,]");
    CHECK_PARSE_FAILS("[,1]");
    CHECK_PARSE_FAILS("[1,,2]");
    CHECK_PARSE_FAILS("[1 2]"); // missing comma
    CHECK_PARSE_FAILS("[1;2]"); // wrong separator
    CHECK_PARSE_FAILS("]1[");
}

//===----------------------------------------------------------------------===//
// Numbers: every malformed numeric form.
//===----------------------------------------------------------------------===//
TEST(malformed_numbers) {
    CHECK_PARSE_FAILS(".");
    CHECK_PARSE_FAILS(".5");
    CHECK_PARSE_FAILS("1.");
    CHECK_PARSE_FAILS("+1");
    CHECK_PARSE_FAILS("1e");
    CHECK_PARSE_FAILS("1e+");
    CHECK_PARSE_FAILS("1.5e");
    CHECK_PARSE_FAILS("e5");
    CHECK_PARSE_FAILS("--1");
    CHECK_PARSE_FAILS("1..2");
    CHECK_PARSE_FAILS("1.2.3");
    CHECK_PARSE_FAILS("-");
    CHECK_PARSE_FAILS("0x1F");  // hex not allowed
    CHECK_PARSE_FAILS("1_000"); // digit separators not allowed
    CHECK_PARSE_FAILS("Infinity");
    CHECK_PARSE_FAILS("NaN");
    CHECK_PARSE_FAILS("1,000");
}

TEST(malformed_leading_zeros) {
    // JSON forbids leading zeros on multi-digit integers.
    CHECK_PARSE_FAILS("01");
    CHECK_PARSE_FAILS("00");
    CHECK_PARSE_FAILS("[00]");
    CHECK_PARSE_FAILS("-01");
    // But a bare zero and "0.x" are fine.
    CHECK(parse("0") != nullptr);
    CHECK(parse("0.5") != nullptr);
    CHECK(parse("-0") != nullptr);
}

TEST(malformed_number_out_of_range) {
    // Overflow to a non-finite double is rejected, not stored as inf.
    CHECK_PARSE_FAILS("1e400");
    CHECK_PARSE_FAILS("-1e400");
    CHECK_PARSE_FAILS("1e309");
    // A 400-digit integer overflows int64, falls back to double, overflows
    // that too, and is rejected.
    std::string huge(400, '9');
    CHECK(pjson_test::parse(huge) == nullptr);
    // Just inside range is fine.
    CHECK(parse("1e308") != nullptr);
}

//===----------------------------------------------------------------------===//
// Strings: unterminated, bad escapes, bad \u.
//===----------------------------------------------------------------------===//
TEST(malformed_strings) {
    CHECK_PARSE_FAILS("\"");
    CHECK_PARSE_FAILS("\"abc");
    CHECK_PARSE_FAILS("\"a\\\"");         // escaped closing quote -> unterminated
    CHECK_PARSE_FAILS("\"line\\");        // dangling backslash
    CHECK_PARSE_FAILS("'single quoted'"); // single quotes not allowed
    CHECK_PARSE_FAILS("\"\\u\"");         // \u with no hex
    CHECK_PARSE_FAILS("\"\\u12\"");       // \u with too few hex
    CHECK_PARSE_FAILS("\"\\uZZZZ\"");     // \u with non-hex
    CHECK_PARSE_FAILS("\"\\u123\"");      // 3 hex digits then quote
}

//===----------------------------------------------------------------------===//
// Trailing content after a complete value.
//===----------------------------------------------------------------------===//
TEST(malformed_trailing_content) {
    CHECK_PARSE_FAILS("1 2");
    CHECK_PARSE_FAILS("1abc");
    CHECK_PARSE_FAILS("nulltrue");
    CHECK_PARSE_FAILS("{}[]");
    CHECK_PARSE_FAILS("[1] [2]");
    CHECK_PARSE_FAILS("\"a\" \"b\"");
    CHECK_PARSE_FAILS("true false");
    CHECK_PARSE_FAILS("1.5 .5");
}

//===----------------------------------------------------------------------===//
// Incomplete / wrong-case keyword literals.
//===----------------------------------------------------------------------===//
TEST(malformed_keywords) {
    CHECK_PARSE_FAILS("nul");
    CHECK_PARSE_FAILS("nulll");
    CHECK_PARSE_FAILS("tru");
    CHECK_PARSE_FAILS("truee");
    CHECK_PARSE_FAILS("fals");
    CHECK_PARSE_FAILS("undefined");
    CHECK_PARSE_FAILS("None");
    CHECK_PARSE_FAILS("nil");
    CHECK(parse("NULL") == nullptr);
    CHECK(parse("True") == nullptr);
}

//===----------------------------------------------------------------------===//
// A byte-order mark is not whitespace; input starting with one is rejected.
//===----------------------------------------------------------------------===//
TEST(malformed_bom_prefix) {
    const char bom[] = {(char)0xEF, (char)0xBB, (char)0xBF, '1'};
    CHECK(pjson_test::parse(bom, sizeof(bom)) == nullptr);
}

//===----------------------------------------------------------------------===//
// Deeply nested input must fail (not crash) once past the depth limit.
//===----------------------------------------------------------------------===//
TEST(malformed_excessive_depth_arrays) {
    const int depth = 200000;
    std::string s(depth, '[');
    s += std::string(depth, ']');
    CHECK(pjson_test::parse(s) == nullptr); // default maxDepth guards this
}

TEST(malformed_excessive_depth_objects) {
    // Build {"a":{"a":{ ... }}} well beyond the default limit.
    std::string s;
    const int depth = 2000;
    for (int i = 0; i < depth; ++i)
        s += "{\"a\":";
    s += "1";
    for (int i = 0; i < depth; ++i)
        s += "}";
    CHECK(pjson_test::parse(s) == nullptr);
}

//===----------------------------------------------------------------------===//
// Parsing rejects raw control characters, unsupported escapes, lone
// surrogates, and invalid UTF-8.
//===----------------------------------------------------------------------===//
TEST(malformed_control_chars) {
    const char rawNL[] = {'"', 'a', '\n', 'b', '"'};
    const char rawTab[] = {'"', '\t', '"'};
    CHECK(pjson_test::parse(rawNL, sizeof(rawNL)) == nullptr);
    CHECK(pjson_test::parse(rawTab, sizeof(rawTab)) == nullptr);
}

TEST(malformed_bad_escapes_and_surrogates) {
    CHECK(pjson_test::parse("\"\\x41\"") == nullptr);
    CHECK(pjson_test::parse("\"\\uD800\"") == nullptr); // lone high surrogate
    CHECK(pjson_test::parse("\"\\uDC00\"") == nullptr); // lone low surrogate
}

TEST(malformed_invalid_utf8) {
    const char bad[] = {'"', (char)0xC3, (char)0x28, '"'}; // 0xC3 not followed by continuation
    const char lone[] = {'"', (char)0xFF, '"'};
    CHECK(pjson_test::parse(bad, sizeof(bad)) == nullptr);
    CHECK(pjson_test::parse(lone, sizeof(lone)) == nullptr);
}

//===----------------------------------------------------------------------===//
// Error offsets point at the offending byte.
//===----------------------------------------------------------------------===//
TEST(malformed_error_offsets) {
    pJsonParser::Error err;

    CHECK(!pjson_test::parse("[1, 2, ]", err));
    CHECK_EQ(err.offset, size_t(7)); // the ']' after a trailing comma

    pjson_test::parse("   @", err);
    CHECK_EQ(err.offset, size_t(3)); // first non-ws garbage

    pjson_test::parse("{\"a\":1 \"b\":2}", err);
    CHECK(!err.ok);
    CHECK(!err.message.empty());
}

//===----------------------------------------------------------------------===//
// The parser never throws, even on random byte soup (smoke test).
//===----------------------------------------------------------------------===//
TEST(malformed_random_bytes_never_throw) {
    const char* soups[] = {
        "{[}]", "\"\\\\\\", "1e-e-1", "{\"\":}",  "[null,]", "\xff\xfe\x00\x01",
        "}{",   "::,,",     "[[[[[[", "\"\t\r\"",
    };
    for (const char* s : soups) {
        // Must terminate and return a value or null; the harness would crash
        // if it threw or segfaulted.
        auto p = parse(s);
        CHECK(true); // reaching here means no throw/crash
    }
}

// Malformed inputs that allocate several partial subtrees must unwind every
// node cleanly. The --asan test lane runs these under LeakSanitizer on Linux.
TEST(malformed_partial_tree_teardown_is_leak_free) {
    const char* cases[] = {
        R"({"a":[1,2,{"b":[3,4,})",
        R"([[[{"x":"y"}, {"z":[true,false,null]}],]])",
        R"({"one":{"two":{"three":[1,2,3]}},"bad":"\uD800"})",
        R"([{"a":1},{"b":2},{"c":3},])",
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        pJsonParser::Error err;
        CHECK(pjson_test::parse(cases[i], err) == nullptr);
        CHECK(!err.ok);
    }
}
