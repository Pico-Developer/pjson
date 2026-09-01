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
// PJSON-API-001/002/003: non-allocating traversal, construction/mutation
// primitives (factories, nullptr, pushBack, insertOrAssign, reserve), and safe
// checked access (at/contains) separated from vivifying operator[].
//
#include "pjson.h"
#include "test_harness.h"

#include <stdexcept>
#include <string>
#include <vector>

using namespace ByteDance;

//===----------------------------------------------------------------------===//
// Factories build each JSON kind without relying on default construction.
//===----------------------------------------------------------------------===//
TEST(factories_build_each_kind) {
    CHECK(pjson::null().isNull());
    CHECK(pjson::object().isObject());
    CHECK(pjson::object().empty());
    CHECK(pjson::array().isArray());
    CHECK(pjson::array().empty());

    pjson v;
    v = pjson::object();
    v = nullptr; // std::nullptr_t assignment resets to null
    CHECK(v.isNull());
}

//===----------------------------------------------------------------------===//
// pushBack accepts arbitrary pjson values by copy and by move.
//===----------------------------------------------------------------------===//
TEST(pushback_copy_and_move) {
    pjson child = pjson::object();
    child["k"] = int64_t(1);

    pjson arr;
    arr.pushBack(child); // copy: child stays valid
    CHECK(child.isObject());
    CHECK(arr.isArray());
    CHECK_EQ(arr.size(), size_t(1));

    pjson moved = pjson::object();
    moved["k"] = int64_t(2);
    arr.pushBack(std::move(moved)); // move
    CHECK_EQ(arr.size(), size_t(2));

    const pjson* first = arr.find(0);
    const pjson* second = arr.find(1);
    CHECK(first != nullptr);
    CHECK(second != nullptr);
    int64_t v = 0;
    if (first)
        CHECK(first->tryGet("k", v));
    CHECK_EQ(v, int64_t(1));
    if (second)
        CHECK(second->tryGet("k", v));
    CHECK_EQ(v, int64_t(2));
}

//===----------------------------------------------------------------------===//
// insertOrAssign inserts a new member and replaces an existing one.
//===----------------------------------------------------------------------===//
TEST(insert_or_assign_semantics) {
    pjson obj = pjson::object();
    pjson valueA;
    valueA = std::string("a");
    obj.insertOrAssign("k", valueA);
    std::string s;
    CHECK(obj.tryGet("k", s));
    CHECK_EQ(s, std::string("a"));

    pjson valueB;
    valueB = std::string("b");
    obj.insertOrAssign("k", std::move(valueB)); // replaces
    CHECK(obj.tryGet("k", s));
    CHECK_EQ(s, std::string("b"));
    CHECK_EQ(obj.size(), size_t(1));
}

//===----------------------------------------------------------------------===//
// reserve() promotes to an array and does not change logical size.
//===----------------------------------------------------------------------===//
TEST(reserve_promotes_and_preserves_size) {
    pjson arr;
    arr.reserve(128);
    CHECK(arr.isArray());
    CHECK_EQ(arr.size(), size_t(0));
    arr.pushBack(pjson::null());
    CHECK_EQ(arr.size(), size_t(1));
}

//===----------------------------------------------------------------------===//
// at() is checked and non-vivifying; operator[] vivifies.
//===----------------------------------------------------------------------===//
TEST(checked_at_does_not_vivify) {
    pjson obj = pjson::object();
    obj["present"] = int64_t(1);

    bool threw = false;
    try {
        (void)obj.at("absent");
    } catch (const std::out_of_range&) {
        threw = true;
    }
    CHECK(threw);
    // at() did not create the missing key.
    CHECK(!obj.contains("absent"));
    CHECK_EQ(obj.size(), size_t(1));

    // Present key resolves.
    int64_t v = 0;
    CHECK(obj.at("present").tryGet(v));
    CHECK_EQ(v, int64_t(1));
}

//===----------------------------------------------------------------------===//
// at(index) is bounds-checked.
//===----------------------------------------------------------------------===//
TEST(checked_at_index_bounds) {
    pjson arr;
    arr.pushBack(int64_t(0) == 0 ? pjson::null() : pjson::null());
    arr += int64_t(10);
    CHECK(arr.isArray());

    bool threw = false;
    try {
        (void)arr.at(size_t(999));
    } catch (const std::out_of_range&) {
        threw = true;
    }
    CHECK(threw);
}

//===----------------------------------------------------------------------===//
// contains() mirrors hasKey().
//===----------------------------------------------------------------------===//
TEST(contains_matches_haskey) {
    pjson obj = pjson::object();
    obj["a"] = int64_t(1);
    CHECK(obj.contains("a"));
    CHECK(obj.contains(std::string("a")));
    CHECK(!obj.contains("b"));
    CHECK_EQ(obj.contains("a"), obj.hasKey("a"));
}

//===----------------------------------------------------------------------===//
// forEachMember visits every member (sorted) with a borrowed key view.
//===----------------------------------------------------------------------===//
namespace {
    struct MemberSum {
        std::string keysConcat;
        int64_t sum = 0;
    };
    bool accumulateMember(pjson::StringView key, const pjson& value, void* ctx) {
        MemberSum& state = *static_cast<MemberSum*>(ctx);
        state.keysConcat.append(key.data(), key.size());
        int64_t v = 0;
        value.tryGet(v);
        state.sum += v;
        return true;
    }
} // namespace

TEST(for_each_member_visits_all) {
    pjson obj = pjson::object();
    obj["b"] = int64_t(2);
    obj["a"] = int64_t(1);
    obj["c"] = int64_t(3);

    MemberSum state;
    const bool completed = obj.forEachMember(&accumulateMember, &state);
    CHECK(completed);
    CHECK_EQ(state.keysConcat, std::string("abc")); // sorted order
    CHECK_EQ(state.sum, int64_t(6));
}

//===----------------------------------------------------------------------===//
// forEachElement visits array elements in order and supports early stop.
//===----------------------------------------------------------------------===//
namespace {
    struct StopAtTwo {
        int64_t visited = 0;
    };
    bool countUntilTwo(const pjson& value, void* ctx) {
        StopAtTwo& state = *static_cast<StopAtTwo*>(ctx);
        int64_t v = 0;
        value.tryGet(v);
        ++state.visited;
        return v < 2; // stop after visiting value 2
    }
} // namespace

TEST(for_each_element_order_and_early_stop) {
    pjson arr;
    for (int64_t i = 0; i < 5; ++i)
        arr += i;

    StopAtTwo state;
    const bool completed = arr.forEachElement(&countUntilTwo, &state);
    CHECK(!completed);                   // stopped early
    CHECK_EQ(state.visited, int64_t(3)); // 0, 1, 2
}

//===----------------------------------------------------------------------===//
// Mutable forEachMember can edit values in place.
//===----------------------------------------------------------------------===//
namespace {
    bool multiplyByTen(pjson::StringView, pjson& value, void*) {
        int64_t v = 0;
        if (value.tryGet(v))
            value = int64_t(v * 10);
        return true;
    }
} // namespace

TEST(for_each_member_mutable_edit) {
    pjson obj = pjson::object();
    obj["x"] = int64_t(1);
    obj["y"] = int64_t(2);

    obj.forEachMember(&multiplyByTen, nullptr);
    int64_t v = 0;
    CHECK(obj.tryGet("x", v));
    CHECK_EQ(v, int64_t(10));
    CHECK(obj.tryGet("y", v));
    CHECK_EQ(v, int64_t(20));
}
