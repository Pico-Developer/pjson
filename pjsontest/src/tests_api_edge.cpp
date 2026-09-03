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
// Per-API normal and edge cases on the settled surface: strict typed reads via
// tryGet(), non-vivifying find()/operator[] split, SerializeOptions-based
// output, parse ownership, and deep equality/copy behavior.
//
#include "pjson.h"
#include "pjson_parser.h"
#include "test_harness.h"
#include "test_util.h"

#include <climits>
#include <cstdint>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ByteDance;

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
// Integer boundaries and double formatting stay round-trippable.
//===----------------------------------------------------------------------===//
TEST(api_int64_boundaries_round_trip) {
    pjson mx;
    mx = static_cast<int64_t>(INT64_MAX);
    pjson mn;
    mn = static_cast<int64_t>(INT64_MIN);
    CHECK_EQ(mx.toString(), std::string("9223372036854775807"));
    CHECK_EQ(mn.toString(), std::string("-9223372036854775808"));

    pjson_test::Parsed pmx = pjson_test::parse("9223372036854775807");
    pjson_test::Parsed pmn = pjson_test::parse("-9223372036854775808");
    CHECK(pmx != nullptr);
    CHECK(pmn != nullptr);
    if (pmx)
        CHECK_EQ(mustGetInt(*pmx), INT64_MAX);
    if (pmn)
        CHECK_EQ(mustGetInt(*pmn), INT64_MIN);
}

TEST(api_double_formatting_edges) {
    struct Case {
        double v;
    };
    const Case cases[] = {
        {0.0},
        {-0.0},
        {1.0},
        {0.1},
        {0.0000001},
        {123456789012345.0},
        {1e308},
        {2.2250738585072014e-308},
        {3.141592653589793},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        pjson j;
        j = cases[i].v;
        pjson_test::Parsed rt = pjson_test::parse(j.toString());
        CHECK(rt != nullptr);
        if (rt)
            CHECK_EQ(mustGetDouble(*rt), cases[i].v);
    }
}

TEST(api_negative_zero_preserved_textually) {
    pjson j;
    j = double(-0.0);
    CHECK_EQ(j.toString(), std::string("-0.0"));
}

//===----------------------------------------------------------------------===//
// Very large strings survive assignment, serialization, and round-trip.
//===----------------------------------------------------------------------===//
TEST(api_large_string_round_trip) {
    std::string big(100000, 'x');
    pjson j;
    j["blob"] = big;

    pjson_test::Parsed rt = pjson_test::parse(j.toString());
    CHECK(rt != nullptr);
    if (!rt)
        return;

    const pjson* blob = rt->find("blob");
    CHECK(blob != nullptr);
    if (!blob)
        return;

    pjson::StringView view;
    CHECK(blob->tryGet(view));
    CHECK_EQ(view.size(), size_t(100000));
    CHECK(*rt == j);
}

TEST(api_string_with_every_escape) {
    std::string s = "\"\\/\b\f\n\r\t";
    for (int c = 1; c < 0x20; ++c)
        s += static_cast<char>(c);
    pjson j;
    j["s"] = s;

    pjson_test::Parsed rt = pjson_test::parse(j.toString());
    CHECK(rt != nullptr);
    if (rt) {
        const pjson* field = rt->find("s");
        CHECK(field != nullptr);
        if (field)
            CHECK_EQ(mustGetString(*field), s);
    }
}

TEST(api_empty_string_and_empty_key) {
    pjson j;
    j[""] = std::string("");
    CHECK(j.hasKey(""));

    const pjson* field = j.find("");
    CHECK(field != nullptr);
    if (field) {
        pjson::StringView view;
        CHECK(field->tryGet(view));
        CHECK(view.empty());
    }

    pjson_test::Parsed rt = pjson_test::parse(j.toString());
    CHECK(rt != nullptr);
    if (rt)
        CHECK(*rt == j);
}

//===----------------------------------------------------------------------===//
// tryGet() is strict, leaves outputs untouched on failure, and widens only
// integers to double.
//===----------------------------------------------------------------------===//
TEST(api_tryget_node_matrix) {
    pjson value;
    int64_t integer = 91;
    double floating = 9.5;
    bool boolean = true;
    std::string string = "sentinel";
    pjson::StringView view;

    value = static_cast<int64_t>(42);
    CHECK(value.tryGet(integer));
    CHECK_EQ(integer, int64_t(42));
    CHECK(value.tryGet(floating));
    CHECK_EQ(floating, 42.0);
    CHECK(!value.tryGet(boolean));
    CHECK_EQ(boolean, true);
    CHECK(!value.tryGet(string));
    CHECK_EQ(string, std::string("sentinel"));
    CHECK(!value.tryGet(view));
    CHECK(view.data() == nullptr);

    value = double(3.5);
    CHECK(!value.tryGet(integer));
    CHECK_EQ(integer, int64_t(42));
    CHECK(value.tryGet(floating));
    CHECK_EQ(floating, 3.5);

    value = false;
    CHECK(value.tryGet(boolean));
    CHECK_EQ(boolean, false);

    value = std::string("hi");
    CHECK(value.tryGet(string));
    CHECK_EQ(string, std::string("hi"));
    CHECK(value.tryGet(view));
    CHECK_EQ(std::string(view.data(), view.size()), std::string("hi"));
}

TEST(api_tryget_child_overloads_and_find_are_non_vivifying) {
    pjson object;
    object["i"] = static_cast<int64_t>(7);
    object["d"] = double(2.5);
    object["b"] = true;
    object["s"] = std::string("value");
    object["arr"][0] = static_cast<int64_t>(11);
    object["arr"][1] = double(4.5);
    object["arr"][2] = false;
    object["arr"][3] = std::string("tail");

    int64_t integer = -1;
    double floating = -1.0;
    bool boolean = false;
    std::string string = "old";
    pjson::StringView view;

    CHECK(object.tryGet("i", integer));
    CHECK_EQ(integer, int64_t(7));
    CHECK(object.tryGet("i", floating));
    CHECK_EQ(floating, 7.0);
    CHECK(object.tryGet("b", boolean));
    CHECK_EQ(boolean, true);
    CHECK(object.tryGet("s", string));
    CHECK_EQ(string, std::string("value"));
    CHECK(object.tryGet("s", view));
    CHECK_EQ(std::string(view.data(), view.size()), std::string("value"));

    const pjson* arr = object.find("arr");
    CHECK(arr != nullptr);
    if (!arr)
        return;

    CHECK(arr->tryGet(0, integer));
    CHECK_EQ(integer, int64_t(11));
    CHECK(arr->tryGet(0, floating));
    CHECK_EQ(floating, 11.0);
    CHECK(arr->tryGet(-2, boolean));
    CHECK_EQ(boolean, false);
    CHECK(arr->tryGet(-1, string));
    CHECK_EQ(string, std::string("tail"));
    CHECK(arr->tryGet(3, view));
    CHECK_EQ(std::string(view.data(), view.size()), std::string("tail"));

    const size_t before = object.size();
    CHECK(object.find("missing") == nullptr);
    CHECK(!object.tryGet("missing", integer));
    CHECK_EQ(integer, int64_t(11));
    CHECK_EQ(object.size(), before);
}

TEST(api_tryget_failure_preserves_outputs_and_wrong_types_do_not_mutate) {
    pjson object;
    object["number"] = static_cast<int64_t>(5);
    object["text"] = std::string("value");
    const size_t objectSize = object.size();

    int64_t integer = 77;
    double floating = 8.5;
    bool boolean = true;
    std::string string = "keep";
    pjson::StringView view;
    pjson held;
    held = std::string("held");
    CHECK(held.tryGet(view));
    const char* viewData = view.data();

    CHECK(!object.tryGet("missing", integer));
    CHECK(!object.tryGet("number", boolean));
    CHECK(!object.tryGet("text", floating));
    CHECK(!object.tryGet("number", string));
    CHECK_EQ(integer, int64_t(77));
    CHECK_EQ(floating, 8.5);
    CHECK_EQ(boolean, true);
    CHECK_EQ(string, std::string("keep"));
    CHECK_EQ(view.data(), viewData);
    CHECK_EQ(object.size(), objectSize);

    pjson array;
    array[0] = static_cast<int64_t>(1);
    CHECK(!array.tryGet(1, integer));
    CHECK(!array.tryGet(-2, string));
    CHECK(!array.tryGet(0, view));
    CHECK_EQ(integer, int64_t(77));
    CHECK_EQ(string, std::string("keep"));
    CHECK_EQ(view.data(), viewData);
    CHECK_EQ(array.size(), size_t(1));
}

//===----------------------------------------------------------------------===//
// find()/hasKey()/keys()/size()/empty() stay non-mutating on read paths.
//===----------------------------------------------------------------------===//
TEST(api_find_haskey_on_non_object) {
    pjson arr;
    arr = std::vector<int64_t>(2, int64_t(0));
    CHECK(arr.find("k") == nullptr);
    CHECK(!arr.hasKey("k"));

    pjson num;
    num = static_cast<int64_t>(5);
    CHECK(num.find("k") == nullptr);
    const pjson& cnum = num;
    CHECK(cnum.find("k") == nullptr);
    CHECK(num.isInt());
}

TEST(api_keys_sorted_and_empty) {
    pjson j;
    j["z"] = static_cast<int64_t>(1);
    j["a"] = static_cast<int64_t>(2);
    j["m"] = static_cast<int64_t>(3);
    std::vector<std::string> k = j.keys();
    CHECK_EQ(k.size(), size_t(3));
    CHECK_EQ(k[0], std::string("a"));
    CHECK_EQ(k[1], std::string("m"));
    CHECK_EQ(k[2], std::string("z"));

    pjson_test::Parsed array = pjson_test::parse("[1,2]");
    pjson_test::Parsed scalar = pjson_test::parse("5");
    CHECK(array != nullptr);
    CHECK(scalar != nullptr);
    if (array)
        CHECK(array->keys().empty());
    if (scalar)
        CHECK(scalar->keys().empty());
}

TEST(api_size_empty_all_types) {
    pjson_test::Parsed array = pjson_test::parse("[1,2,3]");
    pjson_test::Parsed object = pjson_test::parse(R"({"a":1,"b":2})");
    pjson_test::Parsed emptyArray = pjson_test::parse("[]");
    pjson_test::Parsed emptyObject = pjson_test::parse("{}");
    pjson_test::Parsed scalar = pjson_test::parse("5");
    pjson_test::Parsed string = pjson_test::parse("\"hello\"");
    pjson_test::Parsed nullValue = pjson_test::parse("null");
    CHECK(array != nullptr);
    CHECK(object != nullptr);
    CHECK(emptyArray != nullptr);
    CHECK(emptyObject != nullptr);
    CHECK(scalar != nullptr);
    CHECK(string != nullptr);
    CHECK(nullValue != nullptr);
    if (!array || !object || !emptyArray || !emptyObject || !scalar || !string || !nullValue)
        return;

    CHECK_EQ(array->size(), size_t(3));
    CHECK_EQ(object->size(), size_t(2));
    CHECK_EQ(emptyArray->size(), size_t(0));
    CHECK_EQ(emptyObject->size(), size_t(0));
    CHECK_EQ(scalar->size(), size_t(0));
    CHECK_EQ(string->size(), size_t(0));
    CHECK(emptyArray->empty());
    CHECK(nullValue->empty());
    CHECK(!array->empty());
}

//===----------------------------------------------------------------------===//
// Serialization uses SerializeOptions rather than bool pretty flags.
//===----------------------------------------------------------------------===//
TEST(api_serialization_forms_agree) {
    pjson j;
    j["a"] = static_cast<int64_t>(1);
    j["b"] = std::vector<int64_t>({2, 3});

    pjson::SerializeOptions compact;
    std::ostringstream compactOut;
    j.write(compactOut, compact);
    CHECK_EQ(compactOut.str(), j.toString(compact));

    pjson::SerializeOptions pretty = pjson::SerializeOptions::prettyPrinted();
    std::ostringstream prettyOut;
    j.write(prettyOut, pretty);
    CHECK_EQ(prettyOut.str(), j.toString(pretty));
}

TEST(api_pretty_reparses_to_same_data) {
    pjson_test::Parsed value =
        pjson_test::parse(R"({ "nested": { "arr": [1, 2, {"x": true}] }, "s": "v" })");
    CHECK(value != nullptr);
    if (!value)
        return;

    pjson::SerializeOptions pretty = pjson::SerializeOptions::prettyPrinted();
    std::string text = value->toString(pretty);
    pjson_test::Parsed rt = pjson_test::parse(text);
    CHECK(rt != nullptr);
    if (rt)
        CHECK(*rt == *value);
}

//===----------------------------------------------------------------------===//
// Stream parsing and byte-span parsing keep ownership/error semantics.
//===----------------------------------------------------------------------===//
TEST(api_parse_stream_success_and_failure) {
    std::istringstream good(R"({ "k": [1,2,3] })");
    pjson_test::Parsed p = pjson_test::parseStream(good);
    CHECK(p != nullptr);
    if (p) {
        const pjson* field = p->find("k");
        CHECK(field != nullptr);
        if (field)
            CHECK_EQ(field->size(), size_t(3));
    }

    std::istringstream bad("{not valid");
    pJsonParser::Error err;
    pjson_test::Parsed q = pjson_test::parseStream(bad, err);
    CHECK(q == nullptr);
    CHECK(!err.ok);
    CHECK(!err.message.empty());
}

TEST(api_parse_ptr_size_edges) {
    pjson_test::Parsed p = pjson_test::parse("12345xyz", 3);
    CHECK(p != nullptr);
    if (p)
        CHECK_EQ(mustGetInt(*p), int64_t(123));

    const char raw[] = {'"', 'a', '\0', 'b', '"'};
    CHECK(pjson_test::parse(raw, sizeof(raw)) == nullptr);

    CHECK(pjson_test::parse(nullptr, 5) == nullptr);
    CHECK(pjson_test::parse("x", 0) == nullptr);
}

TEST(api_parse_resource_budgets) {
    const pJsonParser::Options defaults;
    CHECK_EQ(defaults.maxDepth, 512);
    CHECK_EQ(defaults.maxNodes, size_t(1000000));
    CHECK_EQ(defaults.maxInputBytes, size_t(64) * 1024U * 1024U);
    CHECK_EQ(defaults.duplicateKeys, pJsonParser::Options::RejectDuplicateKeys);

    pJsonParser::Options nodes;
    nodes.maxNodes = 3;
    pJsonParser::Error err;
    CHECK(pjson_test::parse("[1,2]", err, nodes) != nullptr);
    CHECK(err.ok);

    CHECK(pjson_test::parse("[1,2,3]", err, nodes) == nullptr);
    CHECK(!err.ok);
    CHECK(err.message.find("node budget") != std::string::npos);

    pJsonParser::Options bytes;
    bytes.maxInputBytes = 4;
    CHECK(pjson_test::parse("null", err, bytes) != nullptr);
    CHECK(pjson_test::parse("false", err, bytes) == nullptr);
    CHECK(!err.ok);
    CHECK(err.message.find("maxInputBytes") != std::string::npos);

    std::istringstream oversizedStream("false");
    CHECK(pjson_test::parseStream(oversizedStream, err, bytes) == nullptr);
    CHECK(!err.ok);
    CHECK_EQ(err.offset, size_t(4));
    CHECK_EQ(err.line, size_t(1));
    CHECK_EQ(err.column, size_t(5));
}

//===----------------------------------------------------------------------===//
// Copy / move independence at depth and scale.
//===----------------------------------------------------------------------===//
TEST(api_deep_copy_independence) {
    pjson a;
    for (int64_t i = 0; i < 100; ++i)
        a["arr"][static_cast<int>(i)] = i;
    a["nested"]["deep"]["leaf"] = std::string("orig");

    pjson b = a;
    b["arr"][50] = static_cast<int64_t>(999);
    b["nested"]["deep"]["leaf"] = std::string("changed");

    CHECK_EQ(mustGetInt(a["arr"][50]), int64_t(50));
    CHECK_EQ(mustGetString(a["nested"]["deep"]["leaf"]), std::string("orig"));
    CHECK_EQ(mustGetInt(b["arr"][50]), int64_t(999));
}

TEST(api_move_leaves_source_null) {
    pjson a;
    a["k"] = std::vector<int64_t>({1, 2, 3});
    pjson b = std::move(a);
    CHECK(a.isNull());
    CHECK(b.hasKey("k"));

    const pjson* k = b.find("k");
    CHECK(k != nullptr);
    if (k)
        CHECK_EQ(k->size(), size_t(3));

    pjson c;
    c["old"] = static_cast<int64_t>(1);
    c = std::move(b);
    CHECK(b.isNull());
    CHECK(c.hasKey("k"));
}

//===----------------------------------------------------------------------===//
// Equality: cross-type numeric cases, including above-2^53 exactness.
//===----------------------------------------------------------------------===//
TEST(api_equality_rules) {
    CHECK(*pjson_test::parse("1") == *pjson_test::parse("1.0"));
    CHECK(*pjson_test::parse("1.5") == *pjson_test::parse("1.5"));
    CHECK(*pjson_test::parse("[1,2]") != *pjson_test::parse("[2,1]"));
    CHECK(*pjson_test::parse(R"({"a":1,"b":2})") == *pjson_test::parse(R"({"b":2,"a":1})"));
    CHECK(*pjson_test::parse("true") != *pjson_test::parse("1"));
    CHECK(*pjson_test::parse("null") == *pjson_test::parse("null"));
    CHECK(*pjson_test::parse("\"\"") != *pjson_test::parse("null"));
    CHECK(*pjson_test::parse("{}") != *pjson_test::parse("[]"));

    CHECK(*pjson_test::parse("9007199254740994") == *pjson_test::parse("9007199254740994.0"));
    CHECK(*pjson_test::parse("9007199254740993") != *pjson_test::parse("9007199254740992.0"));
}

//===----------------------------------------------------------------------===//
// resetTo produces valid empty values without raw container getters.
//===----------------------------------------------------------------------===//
TEST(api_reset_to_defaults) {
    pjson j;

    j.resetTo(pjson::jsonString);
    CHECK(j.isString());
    {
        pjson::StringView view;
        CHECK(j.tryGet(view));
        CHECK(view.empty());
    }

    j.resetTo(pjson::jsonNumberInt);
    CHECK(j.isInt());
    CHECK_EQ(mustGetInt(j), int64_t(0));

    j.resetTo(pjson::jsonNumberDouble);
    CHECK(j.isDouble());
    CHECK_EQ(mustGetDouble(j), 0.0);

    j.resetTo(pjson::jsonBoolean);
    CHECK(j.isBool());
    {
        bool value = true;
        CHECK(j.tryGet(value));
        CHECK_EQ(value, false);
    }

    j.resetTo(pjson::jsonArray);
    CHECK(j.isArray());
    CHECK(j.empty());
    CHECK(j.find(0) == nullptr);

    j.resetTo(pjson::jsonObject);
    CHECK(j.isObject());
    CHECK(j.empty());
    CHECK(j.keys().empty());
}

TEST(api_extreme_builder_indexes_are_safe_and_preserve_state_on_failure) {
    pjson emptyArray;
    emptyArray.resetTo(pjson::jsonArray);
    bool emptyArrayThrew = false;
    try {
        emptyArray[INT_MAX] = int64_t(1);
    } catch (const std::length_error&) {
        emptyArrayThrew = true;
    } catch (const std::bad_alloc&) {
        emptyArrayThrew = true;
    }
    CHECK(emptyArrayThrew);
    CHECK(emptyArray.isArray());
    CHECK(emptyArray.empty());

    pjson scalar;
    scalar = std::string("keep");
    bool emptyThrew = false;
    try {
        scalar[INT_MAX] = int64_t(1);
    } catch (const std::length_error&) {
        emptyThrew = true;
    } catch (const std::bad_alloc&) {
        emptyThrew = true;
    }
    CHECK(emptyThrew);
    CHECK(scalar.isString());
    CHECK_EQ(mustGetString(scalar), std::string("keep"));

    pjson array;
    array[0] = int64_t(7);
    const std::string before = array.toString();
    bool populatedThrew = false;
    try {
        array[INT_MAX] = int64_t(9);
    } catch (const std::length_error&) {
        populatedThrew = true;
    } catch (const std::bad_alloc&) {
        populatedThrew = true;
    }
    CHECK(populatedThrew);
    CHECK_EQ(array.toString(), before);

    bool negativeThrew = false;
    try {
        array[INT_MIN] = int64_t(11);
    } catch (const std::out_of_range&) {
        negativeThrew = true;
    }
    CHECK(negativeThrew);
    CHECK_EQ(array.size(), size_t(1));
    CHECK_EQ(mustGetInt(array[0]), int64_t(7));

    pjson empty;
    bool emptyNegativeThrew = false;
    try {
        empty[INT_MIN] = int64_t(3);
    } catch (const std::out_of_range&) {
        emptyNegativeThrew = true;
    }
    CHECK(emptyNegativeThrew);
    CHECK(empty.isNull());
}

TEST(api_null_cstring_mutations_throw_and_preserve_prior_value) {
    const char* nullString = nullptr;

    pjson assigned;
    assigned["keep"] = int64_t(1);
    const std::string assignedBefore = assigned.toString();
    bool assignThrew = false;
    try {
        assigned = nullString;
    } catch (const std::invalid_argument&) {
        assignThrew = true;
    }
    CHECK(assignThrew);
    CHECK_EQ(assigned.toString(), assignedBefore);

    pjson appended;
    appended = std::vector<int64_t>({1, 2});
    const std::string appendedBefore = appended.toString();
    bool appendThrew = false;
    try {
        appended += nullString;
    } catch (const std::invalid_argument&) {
        appendThrew = true;
    }
    CHECK(appendThrew);
    CHECK_EQ(appended.toString(), appendedBefore);

    pjson indexed;
    indexed["keep"] = int64_t(3);
    const std::string indexedBefore = indexed.toString();
    bool indexThrew = false;
    try {
        (void)indexed[nullString];
    } catch (const std::invalid_argument&) {
        indexThrew = true;
    }
    CHECK(indexThrew);
    CHECK_EQ(indexed.toString(), indexedBefore);
}
