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
// Building values programmatically: current operator= / operator+= overloads,
// auto-vivification through operator[], and strict lookup via find()/tryGet().
//===----------------------------------------------------------------------===//
#include "pjson.h"
#include "test_harness.h"

#include <stdexcept>
#include <string>
#include <vector>

using namespace ByteDance;

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

TEST(assign_scalar_string) {
    pjson j;
    j = std::string("hi");
    CHECK_EQ(j.getType(), pjson::jsonString);
    expectString(j, "hi");
}

TEST(assign_scalar_cstring) {
    pjson j;
    j = "world";
    CHECK_EQ(j.getType(), pjson::jsonString);
    expectString(j, "world");
}

TEST(assign_scalar_bool) {
    pjson j;
    j = true;
    CHECK_EQ(j.getType(), pjson::jsonBoolean);
    expectBool(j, true);
}

TEST(assign_scalar_int64) {
    pjson j;
    j = static_cast<int64_t>(9000000000LL);
    CHECK_EQ(j.getType(), pjson::jsonNumberInt);
    expectInt(j, int64_t(9000000000LL));
}

TEST(assign_scalar_double) {
    pjson j;
    j = double(2.5);
    CHECK_EQ(j.getType(), pjson::jsonNumberDouble);
    expectDouble(j, 2.5);
}

TEST(assign_vector_string) {
    pjson j;
    j = std::vector<std::string>({"a", "b", "c"});
    CHECK_EQ(j.getType(), pjson::jsonArray);
    CHECK_EQ(j.size(), size_t(3));
    expectString(j[2], "c");
}

TEST(assign_vector_bool) {
    pjson j;
    j = std::vector<bool>({true, false, true});
    CHECK_EQ(j.size(), size_t(3));
    expectBool(j[0], true);
    expectBool(j[1], false);
    CHECK_EQ(j[0].getType(), pjson::jsonBoolean);
}

TEST(assign_vector_int64) {
    pjson j;
    j = std::vector<int64_t>({10000000000LL, 20000000000LL});
    CHECK_EQ(j.size(), size_t(2));
    CHECK_EQ(j[0].getType(), pjson::jsonNumberInt);
    expectInt(j[0], int64_t(10000000000LL));
}

TEST(assign_vector_double) {
    pjson j;
    j = std::vector<double>({1.25, 2.75});
    CHECK_EQ(j[0].getType(), pjson::jsonNumberDouble);
    expectDouble(j[0], 1.25);
    expectDouble(j[1], 2.75);
}

TEST(append_scalars_of_each_type) {
    pjson j;
    j += std::string("s");
    j += "c";
    j += true;
    j += static_cast<int64_t>(8);
    j += double(2.5);
    CHECK_EQ(j.getType(), pjson::jsonArray);
    CHECK_EQ(j.size(), size_t(5));
    expectString(j[0], "s");
    expectString(j[1], "c");
    expectBool(j[2], true);
    expectInt(j[3], int64_t(8));
    expectDouble(j[4], 2.5);
}

TEST(append_vectors_of_each_type) {
    pjson j;
    j += std::vector<std::string>({"a", "b"});
    j += std::vector<bool>({true});
    j += std::vector<int64_t>({1, 2, 3});
    j += std::vector<double>({4.5, 5.5});
    CHECK_EQ(j.size(), size_t(8));
    expectString(j[0], "a");
    expectBool(j[2], true);
    expectInt(j[5], int64_t(3));
    expectDouble(j[7], 5.5);
}

TEST(append_then_append_accumulates) {
    pjson j;
    j += static_cast<int64_t>(1);
    j += static_cast<int64_t>(2);
    j += std::vector<int64_t>({3, 4});
    CHECK_EQ(j.size(), size_t(4));
    expectInt(j[3], int64_t(4));
}

TEST(map_build_and_lookup) {
    pjson j;
    j["one"] = static_cast<int64_t>(1);
    j["two"] = static_cast<int64_t>(2);
    std::string k = "three";
    j[k] = static_cast<int64_t>(3);
    j["four"] = static_cast<int64_t>(4);
    CHECK_EQ(j.getType(), pjson::jsonObject);
    CHECK_EQ(j.size(), size_t(4));
    CHECK(j.hasKey("one"));
    CHECK(j.hasKey(std::string("three")));
    CHECK(j.hasKey("four"));
    CHECK(!j.hasKey("missing"));
}

TEST(map_reassign_same_key_overwrites) {
    pjson j;
    j["k"] = static_cast<int64_t>(1);
    j["k"] = std::string("replaced");
    CHECK_EQ(j.size(), size_t(1));
    expectString(j["k"], "replaced");
}

TEST(nested_map_and_array_build) {
    pjson j;
    j["a"]["b"]["c"] = static_cast<int64_t>(9);
    j["list"][0] = static_cast<int64_t>(10);
    j["list"][2] = static_cast<int64_t>(30);
    expectInt(j["a"]["b"]["c"], int64_t(9));
    CHECK_EQ(j["list"].size(), size_t(3));
    CHECK_EQ(j["list"][1].getType(), pjson::jsonNull);
    expectInt(j["list"][2], int64_t(30));
}

TEST(array_grows_with_nulls) {
    pjson j;
    j[5] = std::string("sixth");
    CHECK_EQ(j.getType(), pjson::jsonArray);
    CHECK_EQ(j.size(), size_t(6));
    for (int i = 0; i < 5; ++i) {
        CHECK_EQ(j[i].getType(), pjson::jsonNull);
    }
    expectString(j[5], "sixth");
}

TEST(negative_index_from_end) {
    pjson arr;
    arr[0] = static_cast<int64_t>(10);
    arr[1] = static_cast<int64_t>(20);
    arr[2] = static_cast<int64_t>(30);
    expectInt(arr[-1], int64_t(30));
    expectInt(arr[-2], int64_t(20));
    expectInt(arr[-3], int64_t(10));
}

TEST(negative_index_past_start_throws_without_mutation) {
    pjson arr;
    arr[0] = static_cast<int64_t>(10);
    arr[1] = static_cast<int64_t>(20);
    arr[2] = static_cast<int64_t>(30);
    const std::string before = arr.toString();
    bool threw = false;
    try {
        (void)arr[-4];
    } catch (const std::out_of_range&) {
        threw = true;
    }
    CHECK(threw);
    CHECK_EQ(arr.toString(), before);
}

TEST(negative_index_on_empty_array_throws_without_growth) {
    pjson arr;
    arr.resetTo(pjson::jsonArray);
    bool threw = false;
    try {
        (void)arr[-1];
    } catch (const std::out_of_range&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(arr.empty());
}

TEST(negative_index_on_scalar_throws_without_type_change) {
    pjson value;
    value = int64_t(7);
    bool threw = false;
    try {
        (void)value[-1];
    } catch (const std::out_of_range&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(value.isInt());
}

TEST(size_t_index_supports_positive_builder_access) {
    pjson arr;
    const size_t index = 2;
    arr[index] = int64_t(9);
    CHECK_EQ(arr.size(), size_t(3));
    expectInt(arr[2], int64_t(9));
}

TEST(find_returns_pointer_or_null) {
    pjson j;
    j["a"] = static_cast<int64_t>(1);
    CHECK(j.find("a") != nullptr);
    CHECK(j.find(std::string("a")) != nullptr);
    CHECK(j.find("missing") == nullptr);
    if (const pjson* value = j.find("a"))
        expectInt(*value, int64_t(1));
}

TEST(find_does_not_vivify) {
    pjson j;
    j["a"] = static_cast<int64_t>(1);
    CHECK(j.find("ghost") == nullptr);
    CHECK(!j.hasKey("ghost"));
    CHECK_EQ(j.size(), size_t(1));
}

TEST(find_on_non_map_returns_null) {
    pjson j;
    j = static_cast<int64_t>(5);
    CHECK(j.find("a") == nullptr);
    const pjson& cj = j;
    CHECK(cj.find("a") == nullptr);
}

TEST(const_find_works) {
    pjson j;
    j["k"] = std::string("v");
    const pjson& cj = j;
    const pjson* p = cj.find("k");
    CHECK(p != nullptr);
    if (p != nullptr)
        expectString(*p, "v");
}

TEST(strict_tryget_by_key_and_index) {
    pjson j;
    j["n"] = static_cast<int64_t>(77);
    j["d"] = double(2.5);
    j["b"] = true;
    j["s"] = std::string("hi");
    j["a"] = std::vector<int64_t>({10, 20, 30});

    int64_t intOut = -1;
    double doubleOut = 0.0;
    bool boolOut = false;
    std::string stringOut = "orig";

    CHECK(j.tryGet("n", intOut));
    CHECK_EQ(intOut, int64_t(77));
    CHECK(j.tryGet("d", doubleOut));
    CHECK_EQ(doubleOut, 2.5);
    CHECK(j.tryGet("b", boolOut));
    CHECK_EQ(boolOut, true);
    CHECK(j.tryGet("s", stringOut));
    CHECK_EQ(stringOut, std::string("hi"));
    CHECK(j["a"].tryGet(1, intOut));
    CHECK_EQ(intOut, int64_t(20));
}

TEST(strict_tryget_preserves_outputs_on_failure) {
    pjson j;
    j["n"] = static_cast<int64_t>(1);
    j["s"] = std::string("text");

    int64_t intOut = 42;
    bool boolOut = true;
    std::string stringOut = "keep";

    CHECK(!j.tryGet("missing", intOut));
    CHECK_EQ(intOut, int64_t(42));
    CHECK(!j.tryGet("s", intOut));
    CHECK_EQ(intOut, int64_t(42));
    CHECK(!j.tryGet("n", boolOut));
    CHECK_EQ(boolOut, true);
    CHECK(!j.tryGet(0, stringOut));
    CHECK_EQ(stringOut, std::string("keep"));
}

TEST(find_index_is_non_mutating) {
    pjson j;
    j[0] = static_cast<int64_t>(10);
    j[1] = static_cast<int64_t>(20);
    CHECK(j.find(0) != nullptr);
    CHECK(j.find(2) == nullptr);
    CHECK(j.find(-1) != nullptr);
    CHECK(j.find(-3) == nullptr);
    CHECK_EQ(j.size(), size_t(2));
}

TEST(keys_are_complete_and_read_only_iteration_uses_find) {
    pjson j;
    j["b"] = static_cast<int64_t>(2);
    j["a"] = static_cast<int64_t>(1);
    j["c"] = static_cast<int64_t>(3);

    const std::vector<std::string> keys = j.keys();
    CHECK_EQ(keys.size(), size_t(3));
    for (size_t i = 0; i < keys.size(); ++i) {
        CHECK(j.hasKey(keys[i]));
        for (size_t k = i + 1; k < keys.size(); ++k)
            CHECK(keys[i] != keys[k]);
    }

    int64_t sum = 0;
    for (size_t i = 0; i < keys.size(); ++i) {
        const pjson* value = j.find(keys[i]);
        CHECK(value != nullptr);
        if (value != nullptr) {
            int64_t element = 0;
            CHECK(value->tryGet(element));
            sum += element;
        }
    }
    CHECK_EQ(sum, int64_t(6));
}
