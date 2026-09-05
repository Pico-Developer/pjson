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
// Storage-focused tests: scalar copy/move/swap behavior, transitions between
// scalar and container kinds, and ABI/noexcept guarantees.
//
#include "pjson.h"
#include "test_harness.h"
#include "test_util.h"

#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace ByteDance;

static_assert(std::is_nothrow_default_constructible<pjson>::value,
              "pjson null construction must remain noexcept");
static_assert(std::is_nothrow_move_constructible<pjson>::value,
              "pjson move construction must remain noexcept");
static_assert(sizeof(pjson) == sizeof(void*) * 2, "pjson ABI must remain a two-pointer handle");
static_assert(alignof(pjson) == alignof(void*), "pjson ABI alignment must remain pointer-aligned");
static_assert(noexcept(std::declval<pjson&>().swap(std::declval<pjson&>())),
              "pjson::swap must remain noexcept");

namespace {

    void expectInt(const pjson& value, int64_t expected) {
        int64_t actual = 0;
        CHECK(value.tryGet(actual));
        CHECK_EQ(actual, expected);
    }

    void expectDouble(const pjson& value, double expected) {
        double actual = 0.0;
        CHECK(value.tryGet(actual));
        CHECK_EQ(actual, expected);
    }

    void expectBool(const pjson& value, bool expected) {
        bool actual = !expected;
        CHECK(value.tryGet(actual));
        CHECK_EQ(actual, expected);
    }

    void expectString(const pjson& value, const std::string& expected) {
        std::string actual = "<unset>";
        CHECK(value.tryGet(actual));
        CHECK_EQ(actual, expected);
    }

} // namespace

//===----------------------------------------------------------------------===//
// Copy and move preserve scalar values and reset moved-from sources
//===----------------------------------------------------------------------===//

TEST(storage_copy_constructs_inline_scalars) {
    pjson intNode;
    intNode = static_cast<int64_t>(42);
    pjson intCopy(intNode);
    CHECK_EQ(intCopy.getType(), pjson::jsonNumberInt);
    expectInt(intCopy, int64_t(42));
    intCopy = static_cast<int64_t>(7);
    expectInt(intNode, int64_t(42));

    pjson doubleNode;
    doubleNode = double(3.25);
    pjson doubleCopy(doubleNode);
    CHECK_EQ(doubleCopy.getType(), pjson::jsonNumberDouble);
    expectDouble(doubleCopy, 3.25);
    doubleCopy = double(9.5);
    expectDouble(doubleNode, 3.25);

    pjson boolNode;
    boolNode = true;
    pjson boolCopy(boolNode);
    CHECK_EQ(boolCopy.getType(), pjson::jsonBoolean);
    expectBool(boolCopy, true);
    boolCopy = false;
    expectBool(boolNode, true);
}

TEST(storage_copy_assigns_inline_scalars_over_other_types) {
    pjson fromInt;
    fromInt = static_cast<int64_t>(99);
    pjson intoString;
    intoString = "old";
    intoString = fromInt;
    CHECK_EQ(intoString.getType(), pjson::jsonNumberInt);
    expectInt(intoString, int64_t(99));
    expectInt(fromInt, int64_t(99));

    pjson fromDouble;
    fromDouble = double(6.5);
    pjson intoArray;
    intoArray += static_cast<int64_t>(1);
    intoArray = fromDouble;
    CHECK_EQ(intoArray.getType(), pjson::jsonNumberDouble);
    expectDouble(intoArray, 6.5);
    expectDouble(fromDouble, 6.5);

    pjson fromBool;
    fromBool = true;
    pjson intoObject;
    intoObject["k"] = static_cast<int64_t>(1);
    intoObject = fromBool;
    CHECK_EQ(intoObject.getType(), pjson::jsonBoolean);
    expectBool(intoObject, true);
    expectBool(fromBool, true);
}

TEST(storage_move_constructs_inline_scalars) {
    pjson intNode;
    intNode = static_cast<int64_t>(1234);
    pjson movedInt(std::move(intNode));
    CHECK_EQ(movedInt.getType(), pjson::jsonNumberInt);
    expectInt(movedInt, int64_t(1234));
    CHECK(intNode.isNull());

    pjson doubleNode;
    doubleNode = double(8.75);
    pjson movedDouble(std::move(doubleNode));
    CHECK_EQ(movedDouble.getType(), pjson::jsonNumberDouble);
    expectDouble(movedDouble, 8.75);
    CHECK(doubleNode.isNull());

    pjson boolNode;
    boolNode = false;
    pjson movedBool(std::move(boolNode));
    CHECK_EQ(movedBool.getType(), pjson::jsonBoolean);
    expectBool(movedBool, false);
    CHECK(boolNode.isNull());
}

TEST(storage_move_assigns_inline_scalars_over_other_types) {
    pjson fromInt;
    fromInt = static_cast<int64_t>(-7);
    pjson intoMap;
    intoMap["v"] = "x";
    intoMap = std::move(fromInt);
    CHECK_EQ(intoMap.getType(), pjson::jsonNumberInt);
    expectInt(intoMap, int64_t(-7));
    CHECK(fromInt.isNull());

    pjson fromDouble;
    fromDouble = double(-2.5);
    pjson intoString;
    intoString = "before";
    intoString = std::move(fromDouble);
    CHECK_EQ(intoString.getType(), pjson::jsonNumberDouble);
    expectDouble(intoString, -2.5);
    CHECK(fromDouble.isNull());

    pjson fromBool;
    fromBool = true;
    pjson intoArray;
    intoArray += std::vector<int64_t>({1, 2, 3});
    intoArray = std::move(fromBool);
    CHECK_EQ(intoArray.getType(), pjson::jsonBoolean);
    expectBool(intoArray, true);
    CHECK(fromBool.isNull());
}

//===----------------------------------------------------------------------===//
// Swap behavior across inline and heap-backed storage
//===----------------------------------------------------------------------===//

TEST(storage_self_swap_preserves_inline_scalars) {
    pjson intNode;
    intNode = static_cast<int64_t>(-44);
    intNode.swap(intNode);
    CHECK_EQ(intNode.getType(), pjson::jsonNumberInt);
    expectInt(intNode, int64_t(-44));

    pjson doubleNode;
    doubleNode = double(-0.25);
    doubleNode.swap(doubleNode);
    CHECK_EQ(doubleNode.getType(), pjson::jsonNumberDouble);
    expectDouble(doubleNode, -0.25);

    pjson boolNode;
    boolNode = true;
    boolNode.swap(boolNode);
    CHECK_EQ(boolNode.getType(), pjson::jsonBoolean);
    expectBool(boolNode, true);
}

TEST(storage_swaps_inline_and_heap_backed_values) {
    pjson intNode;
    intNode = static_cast<int64_t>(11);
    pjson stringNode;
    stringNode = "eleven";
    intNode.swap(stringNode);
    CHECK(intNode.isString());
    expectString(intNode, "eleven");
    CHECK(stringNode.isInt());
    expectInt(stringNode, int64_t(11));

    pjson boolNode;
    boolNode = false;
    pjson arrayNode;
    arrayNode += static_cast<int64_t>(1);
    arrayNode += static_cast<int64_t>(2);
    boolNode.swap(arrayNode);
    CHECK(boolNode.isArray());
    CHECK_EQ(boolNode.size(), size_t(2));
    CHECK(arrayNode.isBool());
    expectBool(arrayNode, false);

    pjson doubleNode;
    doubleNode = double(4.5);
    pjson mapNode;
    mapNode["pi"] = double(3.14);
    doubleNode.swap(mapNode);
    CHECK(doubleNode.isObject());
    CHECK(doubleNode.hasKey("pi"));
    CHECK(mapNode.isDouble());
    expectDouble(mapNode, 4.5);
}

//===----------------------------------------------------------------------===//
// Type transitions and end-to-end scalar round trips
//===----------------------------------------------------------------------===//

TEST(storage_scalar_type_transitions_preserve_behavior) {
    pjson value;
    value = static_cast<int64_t>(5);
    CHECK_EQ(value.getType(), pjson::jsonNumberInt);
    CHECK_EQ(value.toString(), std::string("5"));

    value = double(5.5);
    CHECK_EQ(value.getType(), pjson::jsonNumberDouble);
    CHECK_EQ(value.toString(), std::string("5.5"));

    value = true;
    CHECK_EQ(value.getType(), pjson::jsonBoolean);
    CHECK_EQ(value.toString(), std::string("true"));

    value = "text";
    CHECK_EQ(value.getType(), pjson::jsonString);
    expectString(value, "text");

    value += static_cast<int64_t>(1);
    CHECK(value.isArray());
    CHECK_EQ(value.size(), size_t(1));
    expectInt(value[0], int64_t(1));

    value = static_cast<int64_t>(8);
    CHECK(value.isInt());
    expectInt(value, int64_t(8));

    value["answer"] = static_cast<int64_t>(42);
    CHECK(value.isObject());
    expectInt(value["answer"], int64_t(42));

    value = false;
    CHECK(value.isBool());
    CHECK_EQ(value.toString(), std::string("false"));
}

TEST(storage_scalar_parse_copy_move_and_serialize_round_trip) {
    pjson_test::Parsed parsed = pjson_test::parse(R"({"i":1,"d":2.5,"b":true})");
    CHECK(parsed != nullptr);
    expectInt((*parsed)["i"], int64_t(1));
    expectDouble((*parsed)["d"], 2.5);
    expectBool((*parsed)["b"], true);

    pjson copied(*parsed);
    CHECK(copied == *parsed);
    pjson moved(std::move(copied));
    CHECK(moved == *parsed);
    CHECK(copied.isNull());
    pjson_test::Parsed expected = pjson_test::parse(R"({"b":true,"d":2.5,"i":1})");
    CHECK(expected != nullptr);
    if (expected != nullptr)
        CHECK(moved == *expected);
}
