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
// Parsing: valid documents of every shape, the (ptr,size) overload, and an
// exhaustive set of invalid inputs that must return nullptr without throwing.
// Number-grammar acceptance/rejection lives here too.
//
#include "pjson.h"
#include "pjson_parser.h"
#include "test_harness.h"
#include "test_util.h"

#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace ByteDance;
using pjson_test::parse;
using pjson_test::valueBool;
using pjson_test::valueDouble;
using pjson_test::valueInt;
using pjson_test::valueString;

static_assert(sizeof(pJsonParser) == sizeof(void*),
              "pJsonParser ABI must remain a one-pointer handle");
static_assert(alignof(pJsonParser) == alignof(void*),
              "pJsonParser ABI alignment must remain pointer-aligned");
static_assert(std::is_nothrow_move_constructible<pJsonParser>::value,
              "pJsonParser move construction must remain noexcept");

//===----------------------------------------------------------------------===//
// Valid top-level scalars
//===----------------------------------------------------------------------===//
TEST(parse_top_level_scalars) {
    auto n = parse("null");
    CHECK(n != nullptr);
    CHECK_EQ(n->getType(), pjson::jsonNull);

    auto t = parse("true");
    CHECK(t != nullptr);
    CHECK_EQ(t->getType(), pjson::jsonBoolean);
    CHECK_EQ(valueBool(*t), true);

    auto f = parse("false");
    CHECK(f != nullptr);
    CHECK_EQ(f->getType(), pjson::jsonBoolean);
    CHECK_EQ(valueBool(*f), false);

    auto i = parse("123");
    CHECK(i != nullptr);
    CHECK_EQ(i->getType(), pjson::jsonNumberInt);
    CHECK_EQ(valueInt(*i), int64_t(123));

    auto d = parse("1.5");
    CHECK(d != nullptr);
    CHECK_EQ(d->getType(), pjson::jsonNumberDouble);
    CHECK_EQ(valueDouble(*d), 1.5);

    auto s = parse("\"text\"");
    CHECK(s != nullptr);
    CHECK_EQ(valueString(*s), std::string("text"));
}

//===----------------------------------------------------------------------===//
// Keywords are strict RFC 8259 spellings.
//===----------------------------------------------------------------------===//
TEST(parse_keywords_are_case_sensitive) {
    CHECK(parse("NULL") == nullptr);
    CHECK(parse("True") == nullptr);
    CHECK(parse("FALSE") == nullptr);
}

//===----------------------------------------------------------------------===//
// Empty containers
//===----------------------------------------------------------------------===//
TEST(parse_empty_containers) {
    auto a = parse("[]");
    CHECK(a != nullptr);
    CHECK_EQ(a->getType(), pjson::jsonArray);
    CHECK_EQ(a->size(), size_t(0));

    auto o = parse("{}");
    CHECK(o != nullptr);
    CHECK_EQ(o->getType(), pjson::jsonObject);
    CHECK_EQ(o->size(), size_t(0));

    // With interior whitespace
    CHECK(parse("[   ]") != nullptr);
    CHECK(parse("{\n\t}") != nullptr);
}

//===----------------------------------------------------------------------===//
// Arrays with mixed element types
//===----------------------------------------------------------------------===//
TEST(parse_mixed_array) {
    auto a = parse("[1, 2.5, \"three\", true, null, [1], {\"k\":1}]");
    CHECK(a != nullptr);
    CHECK_EQ(a->size(), size_t(7));
    CHECK_EQ((*a)[0].getType(), pjson::jsonNumberInt);
    CHECK_EQ((*a)[1].getType(), pjson::jsonNumberDouble);
    CHECK_EQ((*a)[2].getType(), pjson::jsonString);
    CHECK_EQ((*a)[3].getType(), pjson::jsonBoolean);
    CHECK_EQ((*a)[4].getType(), pjson::jsonNull);
    CHECK_EQ((*a)[5].getType(), pjson::jsonArray);
    CHECK_EQ((*a)[6].getType(), pjson::jsonObject);
}

//===----------------------------------------------------------------------===//
// Objects: values of every type, and deep nesting
//===----------------------------------------------------------------------===//
TEST(parse_object_all_value_types) {
    auto o = parse("{\"s\":\"x\",\"i\":1,\"d\":2.5,\"b\":true,\"n\":null,"
                   "\"a\":[1,2],\"m\":{\"k\":9}}");
    CHECK(o != nullptr);
    CHECK_EQ(o->size(), size_t(7));
    CHECK_EQ(valueString((*o)["s"]), std::string("x"));
    CHECK_EQ(valueInt((*o)["i"]), int64_t(1));
    CHECK_EQ(valueDouble((*o)["d"]), 2.5);
    CHECK_EQ(valueBool((*o)["b"]), true);
    CHECK_EQ((*o)["n"].getType(), pjson::jsonNull);
    CHECK_EQ((*o)["a"].size(), size_t(2));
    CHECK_EQ(valueInt((*o)["m"]["k"]), int64_t(9));
}

TEST(parse_deeply_nested) {
    auto o = parse("{\"a\":{\"b\":{\"c\":{\"d\":[1,[2,[3,[4]]]]}}}}");
    CHECK(o != nullptr);
    CHECK_EQ(valueInt((*o)["a"]["b"]["c"]["d"][0]), int64_t(1));
    CHECK_EQ(valueInt((*o)["a"]["b"]["c"]["d"][1][1][1][0]), int64_t(4));
}

//===----------------------------------------------------------------------===//
// Whitespace tolerance: spaces, tabs, newlines, CRLF everywhere legal
//===----------------------------------------------------------------------===//
TEST(parse_whitespace_variations) {
    CHECK(parse("   42   ") != nullptr);
    CHECK(parse("\t\n 42 \r\n") != nullptr);
    auto o = parse("{ \n\t \"a\" \r\n : \t 1 \n , \"b\" : 2 \r\n }");
    CHECK(o != nullptr);
    CHECK_EQ(valueInt((*o)["a"]), int64_t(1));
    CHECK_EQ(valueInt((*o)["b"]), int64_t(2));
}

TEST(parse_crlf_document) {
    auto o = parse("{\r\n  \"a\" : 1,\r\n  \"b\" : 2\r\n}");
    CHECK(o != nullptr);
    CHECK(o->hasKey("a"));
    CHECK(o->hasKey("b"));
}

//===----------------------------------------------------------------------===//
// Duplicate-key handling is explicit: the default rejects, while callers may
// request keep-first or keep-last independently of the always-strict grammar.
//===----------------------------------------------------------------------===//
TEST(parse_duplicate_key_policies) {
    const std::string document = "{\"a\":1,\n\"a\":2}";
    pJsonParser::Error err;
    CHECK(pjson_test::parse(document, err) == nullptr);
    CHECK(!err.ok);
    CHECK_EQ(err.offset, size_t(8));
    CHECK_EQ(err.line, size_t(2));
    CHECK_EQ(err.column, size_t(1));
    CHECK(err.message.find("duplicate") != std::string::npos);

    pJsonParser::Options keepLast;
    keepLast.duplicateKeys = pJsonParser::Options::KeepLastDuplicate;
    auto last = pjson_test::parse(document, keepLast);
    CHECK(last != nullptr);
    CHECK_EQ(last->size(), size_t(1));
    CHECK_EQ(valueInt((*last)["a"]), int64_t(2));

    pJsonParser::Options keepFirst;
    keepFirst.duplicateKeys = pJsonParser::Options::KeepFirstDuplicate;
    auto first = pjson_test::parse(document, keepFirst);
    CHECK(first != nullptr);
    CHECK_EQ(valueInt((*first)["a"]), int64_t(1));

    pJsonParser::Options strictLast;
    strictLast.duplicateKeys = pJsonParser::Options::KeepLastDuplicate;
    CHECK(pjson_test::parse(document, strictLast) != nullptr);
}

TEST(parse_error_reuse_across_calls) {
    pJsonParser::Error err;

    CHECK(pjson_test::parse("{", err) == nullptr);
    CHECK(!err.ok);
    CHECK(!err.message.empty());

    pjson_test::Parsed ok = pjson_test::parse("42", err);
    CHECK(ok != nullptr);
    CHECK(err.ok);
    CHECK_EQ(err.offset, size_t(0));
    CHECK_EQ(err.line, size_t(1));
    CHECK_EQ(err.column, size_t(1));
    CHECK(err.message.empty());
    CHECK_EQ(valueInt(*ok), int64_t(42));

    CHECK(pjson_test::parse("[1,]", err) == nullptr);
    CHECK(!err.ok);
    CHECK_EQ(err.offset, size_t(3));
    CHECK(!err.message.empty());
}

TEST(parser_retains_configuration_and_is_reusable) {
    pJsonParser::Options options;
    options.maxDepth = 7;
    options.maxNodes = 23;
    options.maxInputBytes = 4096;
    options.duplicateKeys = pJsonParser::Options::KeepLastDuplicate;
    options.numberPolicy = pJsonParser::Options::AllowLossyNumbers;
    pJsonParser parser(options);

    CHECK_EQ(parser.options().maxDepth, 7);
    CHECK_EQ(parser.options().maxNodes, size_t(23));
    CHECK_EQ(parser.options().maxInputBytes, size_t(4096));
    CHECK_EQ(parser.options().duplicateKeys, pJsonParser::Options::KeepLastDuplicate);
    CHECK_EQ(parser.options().numberPolicy, pJsonParser::Options::AllowLossyNumbers);

    pJsonParser::Error error;
    pjson first = parser.parse(R"({"value":1,"value":2})", error);
    CHECK(error.ok);
    CHECK_EQ(valueInt(first["value"]), int64_t(2));

    pjson rejected = parser.parse("[1,]", error);
    CHECK(!error.ok);
    CHECK(rejected.isNull());

    pjson second = parser.parse("true", error);
    CHECK(error.ok);
    CHECK_EQ(valueBool(second), true);
    CHECK(&second.getAllocator() == &parser.allocator());
}

TEST(parser_copy_and_move_preserve_configuration) {
    pJsonParser::Options options;
    options.maxDepth = 9;
    options.duplicateKeys = pJsonParser::Options::KeepLastDuplicate;
    pJsonParser parser(options);

    pJsonParser copy(parser);
    CHECK_EQ(copy.options().maxDepth, 9);
    CHECK_EQ(copy.options().duplicateKeys, pJsonParser::Options::KeepLastDuplicate);

    pJsonParser moved(std::move(copy));
    pJsonParser::Error error;
    pjson value = moved.parse(R"({"a":1,"a":2})", error);
    CHECK(error.ok);
    CHECK_EQ(valueInt(value["a"]), int64_t(2));

    // A moved-from parser remains usable with default configuration.
    pjson rejected = copy.parse(R"({"a":1,"a":2})", error);
    CHECK(!error.ok);
    CHECK(rejected.isNull());
}

//===----------------------------------------------------------------------===//
// The (ptr, size) overload: embedded NUL, explicit length, nullptr, partial
//===----------------------------------------------------------------------===//
TEST(parse_ptr_size_respects_length) {
    // Only the first 3 bytes ("123") are in-range; the "456" is ignored.
    const char* src = "123456";
    auto p = parse(src, 3);
    CHECK(p != nullptr);
    CHECK_EQ(valueInt(*p), int64_t(123));
}

TEST(parse_ptr_size_with_embedded_nul_in_string) {
    // Raw NUL is not legal RFC 8259 JSON even when the buffer length is explicit.
    const char raw[] = {'"', 'a', '\0', 'b', '"'};
    CHECK(parse(raw, sizeof(raw)) == nullptr);
}

TEST(parse_nullptr_is_null_not_crash) {
    CHECK(pjson_test::parse(nullptr, 10) == nullptr);
}

TEST(parse_zero_length_is_null) {
    CHECK(pjson_test::parse("anything", 0) == nullptr);
}

//===----------------------------------------------------------------------===//
// Invalid input: empty / whitespace-only
//===----------------------------------------------------------------------===//
TEST(parse_invalid_empty) {
    CHECK_PARSE_FAILS("");
    CHECK_PARSE_FAILS("    ");
    CHECK_PARSE_FAILS("\r\n\t ");
}

//===----------------------------------------------------------------------===//
// Invalid input: truncated / unterminated containers and strings
//===----------------------------------------------------------------------===//
TEST(parse_invalid_truncated) {
    CHECK_PARSE_FAILS("{");
    CHECK_PARSE_FAILS("{\"a\"");
    CHECK_PARSE_FAILS("{\"a\":");
    CHECK_PARSE_FAILS("{\"a\":1");
    CHECK_PARSE_FAILS("{\"a\":1,");
    CHECK_PARSE_FAILS("[");
    CHECK_PARSE_FAILS("[1");
    CHECK_PARSE_FAILS("[1,");
    CHECK_PARSE_FAILS("[1,2");
}

TEST(parse_invalid_unterminated_string) {
    CHECK_PARSE_FAILS("\"unterminated");
    CHECK_PARSE_FAILS("{\"a\":\"unterminated}");
    CHECK_PARSE_FAILS("[\"x\", \"y]");
    CHECK_PARSE_FAILS("\"trailing backslash\\");
}

//===----------------------------------------------------------------------===//
// Invalid input: structural errors
//===----------------------------------------------------------------------===//
TEST(parse_invalid_structure) {
    CHECK_PARSE_FAILS("}");
    CHECK_PARSE_FAILS("]");
    CHECK_PARSE_FAILS("{:1}");              // missing key
    CHECK_PARSE_FAILS("{\"a\" 1}");         // missing colon
    CHECK_PARSE_FAILS("{\"a\":1 \"b\":2}"); // missing comma
    CHECK_PARSE_FAILS("{1:2}");             // non-string key
    CHECK_PARSE_FAILS("{\"a\":}");          // missing value
    CHECK_PARSE_FAILS("[1 2]");             // missing comma between elements
}

//===----------------------------------------------------------------------===//
// Invalid input: comma misuse (trailing / leading / doubled)
//===----------------------------------------------------------------------===//
TEST(parse_invalid_commas) {
    CHECK_PARSE_FAILS("[1,2,3,]");
    CHECK_PARSE_FAILS("[,1]");
    CHECK_PARSE_FAILS("[1,,2]");
    CHECK_PARSE_FAILS("[,]");
    CHECK_PARSE_FAILS("{\"a\":1,}");
    CHECK_PARSE_FAILS("{,\"a\":1}");
    CHECK_PARSE_FAILS("{\"a\":1,,\"b\":2}");
}

//===----------------------------------------------------------------------===//
// Invalid input: trailing garbage after a complete value
//===----------------------------------------------------------------------===//
TEST(parse_invalid_trailing_garbage) {
    CHECK_PARSE_FAILS("1 2");
    CHECK_PARSE_FAILS("1abc");
    CHECK_PARSE_FAILS("truefalse");
    CHECK_PARSE_FAILS("{\"a\":1} junk");
    CHECK_PARSE_FAILS("[1,2] [3]");
    CHECK_PARSE_FAILS("null null");
    CHECK_PARSE_FAILS("\"a\"\"b\"");
}

//===----------------------------------------------------------------------===//
// Invalid input: incomplete keywords
//===----------------------------------------------------------------------===//
TEST(parse_invalid_keywords) {
    CHECK_PARSE_FAILS("nul");
    CHECK_PARSE_FAILS("tru");
    CHECK_PARSE_FAILS("fals");
    CHECK_PARSE_FAILS("n");
    CHECK_PARSE_FAILS("t");
    CHECK_PARSE_FAILS("undefined");
    CHECK_PARSE_FAILS("None");
}

//===----------------------------------------------------------------------===//
// Number grammar: valid forms accepted with correct type
//===----------------------------------------------------------------------===//
TEST(parse_valid_numbers) {
    pjson_test::Parsed zero = parse("0");
    pjson_test::Parsed negative = parse("-123");
    pjson_test::Parsed exponent = parse("1e3");
    pjson_test::Parsed fraction = parse("123.456");
    CHECK(zero != nullptr);
    CHECK(parse("-0") != nullptr);
    CHECK(parse("123") != nullptr);
    CHECK(negative != nullptr);
    CHECK(parse("0.5") != nullptr);
    CHECK(parse("-0.5") != nullptr);
    CHECK(fraction != nullptr);
    CHECK(exponent != nullptr);
    CHECK(parse("1E3") != nullptr);
    CHECK(parse("1e+3") != nullptr);
    CHECK(parse("1e-3") != nullptr);
    CHECK(parse("1.5e10") != nullptr);
    CHECK(parse("-2.5E-4") != nullptr);

    CHECK_EQ(zero->getType(), pjson::jsonNumberInt);
    CHECK_EQ(valueInt(*negative), int64_t(-123));
    CHECK_EQ(exponent->getType(), pjson::jsonNumberDouble);
    CHECK_EQ(valueDouble(*exponent), 1000.0);
    CHECK_EQ(valueDouble(*fraction), 123.456);
}

TEST(parse_bigint_rejected_by_default) {
    // Beyond uint64 range: rejected by default (PJSON-NUM-001) rather than
    // silently rounded to a double.
    auto p = parse("100000000000000000000000");
    CHECK(p == nullptr);
}

TEST(parse_bigint_lossy_opt_in_stores_double) {
    // With the explicit opt-in, the same token stores the nearest double.
    pJsonParser::Options opt;
    opt.numberPolicy = pJsonParser::Options::AllowLossyNumbers;
    auto p = pjson_test::parse("100000000000000000000000", opt);
    CHECK(p != nullptr);
    if (p)
        CHECK_EQ(p->getType(), pjson::jsonNumberDouble);
}

TEST(parse_int64_boundary) {
    auto p = parse("9223372036854775807"); // INT64_MAX
    CHECK(p != nullptr);
    CHECK_EQ(p->getType(), pjson::jsonNumberInt);
    CHECK_EQ(valueInt(*p), int64_t(9223372036854775807LL));
}

//===----------------------------------------------------------------------===//
// Number grammar: malformed forms rejected
//===----------------------------------------------------------------------===//
TEST(parse_invalid_numbers) {
    CHECK_PARSE_FAILS(".");
    CHECK_PARSE_FAILS(".5");   // leading dot
    CHECK_PARSE_FAILS("1.");   // trailing dot
    CHECK_PARSE_FAILS("+1");   // leading plus
    CHECK_PARSE_FAILS("1e");   // exponent without digits
    CHECK_PARSE_FAILS("1.5e"); // exponent without digits
    CHECK_PARSE_FAILS("1e+");  // exponent sign without digits
    CHECK_PARSE_FAILS("e5");   // no mantissa
    CHECK_PARSE_FAILS("--1");
    CHECK_PARSE_FAILS("1..2");
    CHECK_PARSE_FAILS("-");
}

TEST(parse_invalid_numbers_in_context) {
    CHECK_PARSE_FAILS("[1.]");
    CHECK_PARSE_FAILS("[.5]");
    CHECK_PARSE_FAILS("[1e]");
    CHECK_PARSE_FAILS("{\"x\":+1}");
    CHECK_PARSE_FAILS("{\"x\":.}");
}
