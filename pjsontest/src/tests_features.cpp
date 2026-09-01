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
// Tests for higher-level library features on the settled surface: depth/resource
// guards, strict parse mode, pjson_test::Parsed ownership, equality, container
// behavior, erase, and stream I/O.
//
#include "pjson.h"
#include "test_harness.h"
#include "test_util.h"

#include <sstream>
#include <string>
#include <vector>

using namespace ByteDance;
using pjson_test::parse;

namespace {
    int64_t mustGetInt(const pjson& value) {
        int64_t out = 0;
        CHECK(value.tryGet(out));
        return out;
    }

    double mustGetDouble(const pjson& value) {
        double out = 0.0;
        CHECK(value.tryGet(out));
        return out;
    }

    std::string mustGetString(const pjson& value) {
        std::string out;
        CHECK(value.tryGet(out));
        return out;
    }
} // namespace

//===----------------------------------------------------------------------===//
// Library version.
//===----------------------------------------------------------------------===//
TEST(version_string) {
    CHECK_EQ(std::string(pjson::getVersion()), std::string("2.0.0"));
    CHECK_EQ(std::string(PJSON_VERSION), std::string("2.0.0"));
    CHECK_EQ(PJSON_VERSION_MAJOR, 2);
    CHECK_EQ(PJSON_VERSION_MINOR, 0);
    CHECK_EQ(PJSON_VERSION_PATCH, 0);
}

//===----------------------------------------------------------------------===//
// Recursion depth guard: deeply nested input fails cleanly instead of
// overflowing the stack.
//===----------------------------------------------------------------------===//
TEST(depth_guard_rejects_deep_nesting) {
    // Well past the default maxDepth of 512, but the guard must reject it
    // (return null) rather than overflow the stack.
    const int depth = 100000;
    std::string s(depth, '[');
    s += std::string(depth, ']');
    CHECK(pjson_test::parse(s) == nullptr);
}

TEST(depth_guard_allows_reasonable_nesting) {
    // Comfortably under the default limit.
    const int depth = 100;
    std::string s(depth, '[');
    s += std::string(depth, ']');
    auto p = parse(s);
    CHECK(p != nullptr);
}

TEST(depth_guard_boundary_is_configurable) {
    // maxDepth counts array/object frames. With maxDepth = 3, three nested
    // arrays are OK but four are not.
    pjson::ParseOptions opt;
    opt.maxDepth = 3;
    CHECK(pjson_test::parse("[[[1]]]", opt) != nullptr);
    CHECK(pjson_test::parse("[[[[1]]]]", opt) == nullptr);
}

//===----------------------------------------------------------------------===//
// Out-of-range numbers are rejected rather than stored as infinity (which
// would otherwise serialize back to a misleading "null").
//===----------------------------------------------------------------------===//
TEST(number_overflow_rejected) {
    CHECK(parse("1e400") == nullptr);
    CHECK(parse("-1e400") == nullptr);
    CHECK(parse("[1e400]") == nullptr);
}

TEST(number_underflow_is_zero) {
    // Underflow to 0.0 is fine and finite.
    auto p = parse("1e-400");
    CHECK(p != nullptr);
    if (p)
        CHECK_EQ(mustGetDouble(*p), 0.0);
}

TEST(huge_but_finite_number_ok) {
    auto p = parse("1e308"); // within double range
    CHECK(p != nullptr);
}

//===----------------------------------------------------------------------===//
// Parsing always enforces RFC 8259 syntax.
//===----------------------------------------------------------------------===//
TEST(strict_rejects_raw_control_char) {
    const char raw[] = {'"', 'a', '\n', 'b', '"'};
    CHECK(pjson_test::parse(raw, sizeof(raw)) == nullptr);
}

TEST(strict_rejects_unknown_escape) {
    CHECK(pjson_test::parse("\"a\\qb\"") == nullptr);
}

TEST(strict_rejects_lone_surrogate) {
    CHECK(pjson_test::parse("\"\\uD800\"") == nullptr);
}

TEST(strict_accepts_valid_surrogate_pair) {
    CHECK(pjson_test::parse("\"\\uD83D\\uDE00\"") != nullptr);
}

TEST(strict_rejects_uppercase_keywords) {
    CHECK(pjson_test::parse("NULL") == nullptr);
    CHECK(pjson_test::parse("True") == nullptr);
    CHECK(pjson_test::parse("null") != nullptr);
    CHECK(pjson_test::parse("true") != nullptr);
    CHECK(pjson_test::parse("false") != nullptr);
}

TEST(strict_rejects_invalid_utf8) {
    // 0xFF is never valid UTF-8.
    const char bad[] = {'"', static_cast<char>(0xFF), '"'};
    CHECK(pjson_test::parse(bad, sizeof(bad)) == nullptr);
}

TEST(strict_accepts_valid_utf8) {
    // "é" as UTF-8 (0xC3 0xA9) between quotes.
    const char good[] = {'"', static_cast<char>(0xC3), static_cast<char>(0xA9), '"'};
    auto p = pjson_test::parse(good, sizeof(good));
    CHECK(p != nullptr);
    if (!p)
        return;
    pjson::StringView view;
    CHECK(p->tryGet(view));
    CHECK_EQ(view.size(), size_t(2));
}

TEST(strict_still_parses_normal_documents) {
    auto p = pjson_test::parse(R"({ "a": 1, "b": [true, null, "x"] })");
    CHECK(p != nullptr);
    if (!p)
        return;
    const pjson* b = p->find("b");
    CHECK(b != nullptr);
    if (!b)
        return;
    const pjson* tail = b->find(2);
    CHECK(tail != nullptr);
    if (tail)
        CHECK_EQ(mustGetString(*tail), std::string("x"));
}

//===----------------------------------------------------------------------===//
// Value-returning parse API with ParseError-based success detection.
//===----------------------------------------------------------------------===//
TEST(parse_returns_value) {
    pjson_test::Parsed p = pjson_test::parse(R"({"k":42})");
    CHECK(static_cast<bool>(p));
    if (p) {
        const pjson* value = p->find("k");
        CHECK(value != nullptr);
        if (value)
            CHECK_EQ(mustGetInt(*value), int64_t(42));
    }

    pjson_test::Parsed bad = pjson_test::parse("{not json");
    CHECK(!bad); // reports failure via ParseError
}

TEST(parse_ptr_size_overload) {
    const char* src = "123456";
    auto p = pjson_test::parse(src, 3); // only "123"
    CHECK(static_cast<bool>(p));
    if (p)
        CHECK_EQ(mustGetInt(*p), int64_t(123));
}

//===----------------------------------------------------------------------===//
// Parse errors expose a byte offset plus one-based line/byte-column coordinates.
//===----------------------------------------------------------------------===//
TEST(parse_error_reports_success) {
    pjson::ParseError err;
    auto p = pjson_test::parse(R"({"a":1})", err);
    CHECK(static_cast<bool>(p));
    CHECK(err.ok);
    CHECK_EQ(err.line, size_t(1));
    CHECK_EQ(err.column, size_t(1));
}

TEST(parse_error_reports_offset_and_message) {
    pjson::ParseError err;
    auto p = pjson_test::parse("[1, 2, ]", err); // trailing comma at index 7
    CHECK(!p);
    CHECK(!err.ok);
    CHECK(!err.message.empty());
    CHECK_EQ(err.offset, size_t(7));
    CHECK_EQ(err.line, size_t(1));
    CHECK_EQ(err.column, size_t(8));
}

TEST(parse_error_reports_line_and_column) {
    pjson::ParseError err;
    CHECK(!pjson_test::parse("{\r\n  \"a\": 1,\r\n  \"b\": [2, ]\r\n}", err));
    CHECK_EQ(err.line, size_t(3));
    CHECK_EQ(err.column, size_t(12));
    CHECK_EQ(err.offset, size_t(25));

    CHECK(!pjson_test::parse("1\r\n2", err));
    CHECK_EQ(err.offset, size_t(3));
    CHECK_EQ(err.line, size_t(2));
    CHECK_EQ(err.column, size_t(1));

    CHECK(!pjson_test::parse("1\r2", err));
    CHECK_EQ(err.offset, size_t(2));
    CHECK_EQ(err.line, size_t(2));
    CHECK_EQ(err.column, size_t(1));
}

TEST(parse_error_trailing_garbage) {
    pjson::ParseError err;
    auto p = pjson_test::parse("42 abc", err);
    CHECK(!p);
    CHECK(!err.ok);
    CHECK_EQ(err.offset, size_t(3)); // 'a'
    CHECK_EQ(err.line, size_t(1));
    CHECK_EQ(err.column, size_t(4));
}

TEST(parse_error_depth_message) {
    pjson::ParseOptions opt;
    opt.maxDepth = 2;
    pjson::ParseError err;
    auto p = pjson_test::parse("[[[1]]]", err, opt);
    CHECK(!p);
    CHECK(!err.ok);
    CHECK(err.message.find("depth") != std::string::npos);
}

//===----------------------------------------------------------------------===//
// Deep structural equality between values.
//===----------------------------------------------------------------------===//
TEST(equality_scalars) {
    pjson a;
    a = static_cast<int64_t>(5);
    pjson b;
    b = static_cast<int64_t>(5);
    CHECK(a == b);

    pjson c;
    c = double(5.0);
    CHECK(a == c); // 5 (int) == 5.0 (double)

    pjson d;
    d = static_cast<int64_t>(6);
    CHECK(a != d);

    pjson s1;
    s1 = std::string("x");
    pjson s2;
    s2 = "x";
    CHECK(s1 == s2);

    pjson t;
    t = true;
    pjson one;
    one = static_cast<int64_t>(1);
    CHECK(t != one); // bool is not a number

    pjson exactInt;
    exactInt = static_cast<int64_t>(9007199254740994LL);
    pjson exactDouble;
    exactDouble = double(9007199254740994.0);
    CHECK(exactInt == exactDouble);

    pjson roundedInt;
    roundedInt = static_cast<int64_t>(9007199254740993LL);
    pjson roundedDouble;
    roundedDouble = double(9007199254740992.0);
    CHECK(roundedInt != roundedDouble);
}

TEST(equality_deep_structures) {
    auto a = parse(R"({"k":[1,2,{"x":true}], "s":"v"})");
    auto b = parse(R"({"s":"v", "k":[1,2,{"x":true}]})"); // different key order in text
    CHECK(a != nullptr);
    CHECK(b != nullptr);
    CHECK(*a == *b); // objects compare regardless of source order

    auto c = parse(R"({"k":[1,2,{"x":false}], "s":"v"})");
    CHECK(*a != *c); // one nested value differs
}

TEST(equality_array_order_matters) {
    auto a = parse("[1,2,3]");
    auto b = parse("[3,2,1]");
    CHECK(*a != *b);
}

//===----------------------------------------------------------------------===//
// Container conveniences: size, empty, clear, and type predicates.
//===----------------------------------------------------------------------===//
TEST(size_and_empty) {
    pjson arr;
    arr = std::vector<int64_t>({1, 2, 3});
    CHECK_EQ(arr.size(), size_t(3));
    CHECK(!arr.empty());

    pjson obj;
    obj["a"] = int64_t(1);
    obj["b"] = int64_t(2);
    CHECK_EQ(obj.size(), size_t(2));

    pjson scalar;
    scalar = int64_t(5);
    CHECK_EQ(scalar.size(), size_t(0));
    CHECK(scalar.empty());

    pjson nul;
    CHECK(nul.empty());
}

TEST(clear_container_keeps_type) {
    pjson arr;
    arr = std::vector<int64_t>({1, 2, 3});
    arr.clear();
    CHECK_EQ(arr.getType(), pjson::jsonArray);
    CHECK_EQ(arr.size(), size_t(0));

    pjson obj;
    obj["a"] = int64_t(1);
    obj.clear();
    CHECK_EQ(obj.getType(), pjson::jsonObject);
    CHECK(obj.empty());

    pjson scalar;
    scalar = static_cast<int64_t>(5);
    scalar.clear();
    CHECK_EQ(scalar.getType(), pjson::jsonNull);
}

TEST(type_predicates) {
    pjson n;
    CHECK(n.isNull());
    pjson s;
    s = "x";
    CHECK(s.isString());
    pjson i;
    i = static_cast<int64_t>(5);
    CHECK(i.isNumber());
    CHECK(i.isInt());
    CHECK(!i.isDouble());
    pjson d;
    d = double(1.5);
    CHECK(d.isNumber());
    CHECK(d.isDouble());
    CHECK(!d.isInt());
    pjson b;
    b = true;
    CHECK(b.isBool());
    pjson a;
    a = std::vector<int64_t>({1});
    CHECK(a.isArray());
    pjson m;
    m["k"] = int64_t(1);
    CHECK(m.isObject());
}

//===----------------------------------------------------------------------===//
// Removing map keys and array elements with erase().
//===----------------------------------------------------------------------===//
TEST(erase_map_key) {
    pjson j;
    j["a"] = static_cast<int64_t>(1);
    j["b"] = static_cast<int64_t>(2);
    j["c"] = static_cast<int64_t>(3);
    CHECK(j.erase("b"));
    CHECK_EQ(j.size(), size_t(2));
    CHECK(!j.hasKey("b"));
    CHECK(!j.erase("missing")); // returns false, no-op
}

TEST(erase_array_index) {
    pjson j;
    j = std::vector<int64_t>({10, 20, 30});
    CHECK(j.erase(size_t(1))); // remove the 20
    CHECK_EQ(j.size(), size_t(2));
    CHECK_EQ(mustGetInt(j[0]), int64_t(10));
    CHECK_EQ(mustGetInt(j[1]), int64_t(30));
    CHECK(!j.erase(size_t(5))); // out of range -> false
}

TEST(erase_wrong_type_is_false) {
    pjson j;
    j = static_cast<int64_t>(5);
    CHECK(!j.erase("a"));
    CHECK(!j.erase(size_t(0)));
}

//===----------------------------------------------------------------------===//
// Listing object keys for iteration.
//===----------------------------------------------------------------------===//
TEST(keys_returns_sorted_keys) {
    pjson j;
    j["gamma"] = static_cast<int64_t>(1);
    j["alpha"] = static_cast<int64_t>(2);
    j["beta"] = static_cast<int64_t>(3);
    std::vector<std::string> k = j.keys();
    CHECK_EQ(k.size(), size_t(3));
    CHECK_EQ(k[0], std::string("alpha"));
    CHECK_EQ(k[1], std::string("beta"));
    CHECK_EQ(k[2], std::string("gamma"));

    pjson notMap;
    notMap = static_cast<int64_t>(5);
    CHECK(notMap.keys().empty());
}

//===----------------------------------------------------------------------===//
// Strict typed keyed reads via tryGet().
//===----------------------------------------------------------------------===//
TEST(tryget_keyed_reads) {
    pjson j;
    j["i"] = static_cast<int64_t>(7);
    j["d"] = double(2.5);
    j["b"] = true;
    j["s"] = std::string("hi");

    int64_t integer = -1;
    double floating = -1.0;
    bool boolean = false;
    std::string string = "old";

    CHECK(j.tryGet("i", integer));
    CHECK_EQ(integer, int64_t(7));
    CHECK(j.tryGet("i", floating));
    CHECK_EQ(floating, 7.0);
    CHECK(j.tryGet("d", floating));
    CHECK_EQ(floating, 2.5);
    CHECK(j.tryGet("b", boolean));
    CHECK_EQ(boolean, true);
    CHECK(j.tryGet("s", string));
    CHECK_EQ(string, std::string("hi"));

    CHECK(!j.tryGet("missing", integer));
    CHECK(!j.tryGet("s", integer));
    CHECK_EQ(integer, int64_t(7));
}

//===----------------------------------------------------------------------===//
// Stream I/O: writing to and parsing from std::ostream / std::istream.
//===----------------------------------------------------------------------===//
TEST(write_to_ostream) {
    pjson j;
    j["a"] = static_cast<int64_t>(1);
    j["b"] = std::vector<int64_t>({2, 3});
    std::ostringstream os;
    j.write(os);
    CHECK_EQ(os.str(), j.toString());
}

TEST(write_to_stream) {
    pjson j;
    j = std::vector<int64_t>({1, 2, 3});
    std::ostringstream os;
    j.write(os);
    CHECK_EQ(os.str(), j.toString());
}

TEST(parse_from_stream) {
    std::istringstream is(R"({ "name": "Ada", "scores": [90, 82] })");
    auto p = pjson_test::parseStream(is);
    CHECK(static_cast<bool>(p));
    if (!p)
        return;
    const pjson* name = p->find("name");
    const pjson* scores = p->find("scores");
    CHECK(name != nullptr);
    CHECK(scores != nullptr);
    if (name)
        CHECK_EQ(mustGetString(*name), std::string("Ada"));
    if (scores)
        CHECK_EQ(scores->size(), size_t(2));
}

TEST(parse_from_stream_with_error) {
    std::istringstream is("{bad");
    pjson::ParseError err;
    auto p = pjson_test::parseStream(is, err);
    CHECK(!p);
    CHECK(!err.ok);
}

TEST(stream_round_trip) {
    pjson j;
    j["a"] = int64_t(1);
    j["b"]["c"] = std::vector<std::string>({"x", "y"});
    std::ostringstream os;
    j.write(os);
    std::istringstream is(os.str());
    auto rt = pjson_test::parseStream(is);
    CHECK(static_cast<bool>(rt));
    CHECK(*rt == j);
}
