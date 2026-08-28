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
// String escaping/unescaping and \uXXXX plus surrogate handling.
//
#include "pjson.h"
#include "test_harness.h"
#include "test_util.h"

#include <string>

using namespace ByteDance;
using pjson_test::parse;

namespace {

    std::string stringValue(const pjson& aValue) {
        std::string value;
        CHECK(aValue.tryGet(value));
        return value;
    }

} // namespace

//===----------------------------------------------------------------------===//
// Every mandatory escape is emitted on serialize
//===----------------------------------------------------------------------===//
TEST(serialize_mandatory_escapes) {
    pjson j;
    j = std::string("\" \\ \b \f \n \r \t");
    std::string out = j.toString();
    CHECK(out.find("\\\"") != std::string::npos);
    CHECK(out.find("\\\\") != std::string::npos);
    CHECK(out.find("\\b") != std::string::npos);
    CHECK(out.find("\\f") != std::string::npos);
    CHECK(out.find("\\n") != std::string::npos);
    CHECK(out.find("\\r") != std::string::npos);
    CHECK(out.find("\\t") != std::string::npos);
}

TEST(serialize_control_chars_as_u_escape) {
    pjson j;
    j = std::string("\x01\x02\x1f", 3);
    std::string out = j.toString();
    CHECK(out.find("\\u0001") != std::string::npos);
    CHECK(out.find("\\u0002") != std::string::npos);
    CHECK(out.find("\\u001f") != std::string::npos);
}

TEST(serialize_forward_slash_not_escaped) {
    pjson j;
    j = std::string("a/b/c");
    // '/' is legal unescaped; keep output clean.
    CHECK_EQ(j.toString(), std::string("\"a/b/c\""));
}

TEST(serialize_printable_ascii_unchanged) {
    pjson j;
    j = std::string("Hello, World! 123 ~");
    CHECK_EQ(j.toString(), std::string("\"Hello, World! 123 ~\""));
}

//===----------------------------------------------------------------------===//
// Round-trip: every escape survives parse(serialize(x)) == x
//===----------------------------------------------------------------------===//
TEST(escape_round_trip_all_specials) {
    pjson j;
    j["msg"] = std::string("q\"b\\s /f\b\f\n\r\t end");
    auto rt = parse(j.toString());
    CHECK(rt != nullptr);
    CHECK_EQ(stringValue((*rt)["msg"]), std::string("q\"b\\s /f\b\f\n\r\t end"));
}

TEST(escape_round_trip_control_bytes) {
    std::string all;
    for (int c = 1; c < 0x20; ++c)
        all += static_cast<char>(c);
    pjson j;
    j["c"] = all;
    auto rt = parse(j.toString());
    CHECK(rt != nullptr);
    CHECK_EQ(stringValue((*rt)["c"]), all);
}

TEST(escape_map_keys_too) {
    pjson j;
    j[std::string("key\"\\\n\t")] = int64_t(1);
    auto rt = parse(j.toString());
    CHECK(rt != nullptr);
    CHECK(rt->hasKey(std::string("key\"\\\n\t")));
}

//===----------------------------------------------------------------------===//
// Parser unescapes correctly
//===----------------------------------------------------------------------===//
TEST(parse_unescapes_simple) {
    auto p = parse("\"line1\\nline2\\ttab\"");
    CHECK(p != nullptr);
    CHECK_EQ(stringValue(*p), std::string("line1\nline2\ttab"));
}

TEST(parse_unescapes_quote_and_backslash) {
    auto p = parse("\"a\\\"b\\\\c\"");
    CHECK(p != nullptr);
    CHECK_EQ(stringValue(*p), std::string("a\"b\\c"));
}

TEST(parse_escaped_forward_slash) {
    auto p = parse("\"a\\/b\"");
    CHECK(p != nullptr);
    CHECK_EQ(stringValue(*p), std::string("a/b"));
}

TEST(parse_empty_string) {
    auto a = parse("\"\"");
    CHECK(a != nullptr);
    CHECK_EQ(stringValue(*a), std::string(""));
    auto o = parse("{\"k\":\"\"}");
    CHECK(o != nullptr);
    CHECK_EQ(stringValue((*o)["k"]), std::string(""));
}

//===----------------------------------------------------------------------===//
// \uXXXX decoding to UTF-8 (1/2/3-byte) and surrogate pairs (4-byte)
//===----------------------------------------------------------------------===//
TEST(unicode_ascii_escape) {
    auto p = parse("\"\\u0041\\u0042\""); // "AB"
    CHECK(p != nullptr);
    CHECK_EQ(stringValue(*p), std::string("AB"));
}

TEST(unicode_two_byte) {
    auto p = parse("\"\\u00e9\""); // é
    CHECK(p != nullptr);
    const std::string value = stringValue(*p);
    CHECK_EQ(value.size(), size_t(2));
    CHECK_EQ(static_cast<unsigned char>(value[0]), 0xC3u);
    CHECK_EQ(static_cast<unsigned char>(value[1]), 0xA9u);
}

TEST(unicode_three_byte) {
    auto p = parse("\"\\u20ac\""); // € (euro sign)
    CHECK(p != nullptr);
    CHECK_EQ(stringValue(*p).size(), size_t(3));
}

TEST(unicode_surrogate_pair_four_byte) {
    auto p = parse("\"\\uD83D\\uDE00\""); // 😀 U+1F600
    CHECK(p != nullptr);
    const std::string value = stringValue(*p);
    CHECK_EQ(value.size(), size_t(4));
    CHECK_EQ(static_cast<unsigned char>(value[0]), 0xF0u);
}

TEST(unicode_invalid_hex_rejected) {
    CHECK_PARSE_FAILS("\"\\uZZZZ\"");
    CHECK_PARSE_FAILS("\"\\u12\""); // too few hex digits
    CHECK_PARSE_FAILS("\"\\u\"");
}

TEST(unicode_round_trip_through_serialize) {
    // A parsed multibyte value re-serializes as raw UTF-8 (passthrough) and
    // parses back to the identical bytes.
    auto p = parse("\"\\u20ac\"");
    CHECK(p != nullptr);
    std::string euro = stringValue(*p);
    pjson j;
    j = euro;
    auto rt = parse(j.toString());
    CHECK(rt != nullptr);
    CHECK_EQ(stringValue(*rt), euro);
}
