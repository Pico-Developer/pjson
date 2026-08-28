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
// Core semantics: type tags, strict typed access, reset/resetTo, and copy/move
// construction & assignment.
//
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

    void expectStrictMismatch(const pjson& value) {
        int64_t intValue = 17;
        double doubleValue = 17.0;
        bool boolValue = true;
        std::string stringValue = "keep";
        CHECK(!value.tryGet(intValue));
        CHECK_EQ(intValue, int64_t(17));
        CHECK(!value.tryGet(doubleValue));
        CHECK_EQ(doubleValue, 17.0);
        CHECK(!value.tryGet(boolValue));
        CHECK_EQ(boolValue, true);
        CHECK(!value.tryGet(stringValue));
        CHECK_EQ(stringValue, std::string("keep"));
    }

} // namespace

TEST(type_tags_for_every_kind) {
    pjson jnull;
    CHECK_EQ(jnull.getType(), pjson::jsonNull);

    pjson js;
    js = std::string("s");
    CHECK_EQ(js.getType(), pjson::jsonString);

    pjson ji;
    ji = static_cast<int64_t>(7);
    CHECK_EQ(ji.getType(), pjson::jsonNumberInt);

    pjson jd;
    jd = double(1.5);
    CHECK_EQ(jd.getType(), pjson::jsonNumberDouble);

    pjson jb;
    jb = true;
    CHECK_EQ(jb.getType(), pjson::jsonBoolean);

    pjson ja;
    ja = std::vector<int64_t>({1, 2});
    CHECK_EQ(ja.getType(), pjson::jsonArray);

    pjson jm;
    jm["k"] = static_cast<int64_t>(1);
    CHECK_EQ(jm.getType(), pjson::jsonObject);
}

TEST(strict_tryget_matches_public_contract) {
    pjson integerValue;
    integerValue = static_cast<int64_t>(42);
    expectInt(integerValue, int64_t(42));
    expectDouble(integerValue, 42.0);
    {
        bool boolOut = false;
        CHECK(!integerValue.tryGet(boolOut));
        CHECK_EQ(boolOut, false);
    }
    {
        std::string stringOut = "unchanged";
        CHECK(!integerValue.tryGet(stringOut));
        CHECK_EQ(stringOut, std::string("unchanged"));
    }

    pjson doubleValue;
    doubleValue = double(3.9);
    expectDouble(doubleValue, 3.9);
    {
        int64_t intOut = -1;
        CHECK(!doubleValue.tryGet(intOut));
        CHECK_EQ(intOut, int64_t(-1));
    }

    pjson stringValue;
    stringValue = std::string("hello");
    expectString(stringValue, "hello");
    pjson::StringView view;
    CHECK(stringValue.tryGet(view));
    CHECK_EQ(std::string(view.data(), view.size()), std::string("hello"));
    {
        int64_t intOut = 17;
        double doubleOut = 17.0;
        bool boolOut = true;
        CHECK(!stringValue.tryGet(intOut));
        CHECK_EQ(intOut, int64_t(17));
        CHECK(!stringValue.tryGet(doubleOut));
        CHECK_EQ(doubleOut, 17.0);
        CHECK(!stringValue.tryGet(boolOut));
        CHECK_EQ(boolOut, true);
    }

    pjson boolValue;
    boolValue = true;
    expectBool(boolValue, true);
    {
        int64_t intOut = 17;
        double doubleOut = 17.0;
        std::string stringOut = "keep";
        CHECK(!boolValue.tryGet(intOut));
        CHECK_EQ(intOut, int64_t(17));
        CHECK(!boolValue.tryGet(doubleOut));
        CHECK_EQ(doubleOut, 17.0);
        CHECK(!boolValue.tryGet(stringOut));
        CHECK_EQ(stringOut, std::string("keep"));
    }

    pjson nullValue;
    expectStrictMismatch(nullValue);
}

TEST(int_vs_double_type_is_preserved) {
    pjson i;
    i = static_cast<int64_t>(5);
    pjson d;
    d = double(5.0);
    CHECK_EQ(i.getType(), pjson::jsonNumberInt);
    CHECK_EQ(d.getType(), pjson::jsonNumberDouble);
    CHECK_EQ(i.toString(), std::string("5"));
    CHECK_EQ(d.toString(), std::string("5.0"));
}

TEST(reset_returns_to_null) {
    pjson j;
    j["a"] = static_cast<int64_t>(1);
    j["b"] = static_cast<int64_t>(2);
    CHECK_EQ(j.getType(), pjson::jsonObject);
    j.reset();
    CHECK_EQ(j.getType(), pjson::jsonNull);
    CHECK_EQ(j.size(), size_t(0));
    CHECK(!j.hasKey("a"));
}

TEST(reset_to_each_type_has_zero_default) {
    pjson j;

    j.resetTo(pjson::jsonNumberInt);
    expectInt(j, int64_t(0));

    j.resetTo(pjson::jsonNumberDouble);
    expectDouble(j, 0.0);

    j.resetTo(pjson::jsonBoolean);
    expectBool(j, false);

    j.resetTo(pjson::jsonString);
    expectString(j, "");

    j.resetTo(pjson::jsonArray);
    CHECK(j.isArray());
    CHECK_EQ(j.size(), size_t(0));

    j.resetTo(pjson::jsonObject);
    CHECK(j.isObject());
    CHECK_EQ(j.size(), size_t(0));

    j.resetTo(pjson::jsonNull);
    CHECK_EQ(j.getType(), pjson::jsonNull);
}

TEST(reset_to_invalid_enum_throws_and_preserves_old_value) {
    pjson j;
    j["keep"]["nested"] = std::string("value");
    j["count"] = static_cast<int64_t>(2);
    const std::string before = j.toString();

    bool threw = false;
    try {
        j.resetTo(static_cast<pjson::jsonType>(999));
    } catch (const std::invalid_argument&) {
        threw = true;
    }

    CHECK(threw);
    CHECK_EQ(j.toString(), before);
    CHECK(j.isObject());
    expectString(j["keep"]["nested"], "value");
    expectInt(j["count"], int64_t(2));
}

TEST(reassignment_changes_type_and_frees_old) {
    pjson j;
    j["a"] = static_cast<int64_t>(1);
    CHECK_EQ(j.getType(), pjson::jsonObject);

    j = std::string("now a string");
    CHECK_EQ(j.getType(), pjson::jsonString);
    expectString(j, "now a string");

    j = std::vector<int64_t>({1, 2, 3});
    CHECK_EQ(j.getType(), pjson::jsonArray);
    CHECK_EQ(j.size(), size_t(3));
}

TEST(copy_construct_is_deep_and_independent) {
    pjson a;
    a["name"] = std::string("original");
    a["nums"] = std::vector<int64_t>({1, 2, 3});
    a["nested"]["deep"] = static_cast<int64_t>(9);

    pjson b(a);
    CHECK_EQ(b.toString(), a.toString());

    b["name"] = std::string("changed");
    b["nested"]["deep"] = static_cast<int64_t>(100);
    expectString(a["name"], "original");
    expectInt(a["nested"]["deep"], int64_t(9));
    expectString(b["name"], "changed");
    expectInt(b["nested"]["deep"], int64_t(100));
}

TEST(copy_assign_is_deep) {
    pjson a;
    a["x"] = std::vector<std::string>({"p", "q"});
    pjson b;
    b = static_cast<int64_t>(12345);
    b = a;
    CHECK_EQ(b.toString(), a.toString());
    b["x"][0] = std::string("z");
    expectString(a["x"][0], "p");
}

TEST(self_assignment_is_safe) {
    pjson a;
    a["k"] = std::string("v");
    pjson& ref = a;
    a = ref;
    expectString(a["k"], "v");

    pjson& mref = a;
    a = std::move(mref);
    expectString(a["k"], "v");
}

TEST(child_assignment_is_safe) {
    pjson j;
    j["a"]["b"] = static_cast<int64_t>(42);
    j = j["a"];
    CHECK(j.hasKey("b"));
    expectInt(j["b"], int64_t(42));
}

TEST(move_construct_transfers) {
    pjson a;
    a["k"] = std::vector<int64_t>({1, 2, 3});
    const std::string before = a.toString();
    pjson b(std::move(a));
    CHECK_EQ(b.toString(), before);
    CHECK_EQ(a.getType(), pjson::jsonNull);
}

TEST(move_assign_transfers) {
    pjson a;
    a["k"] = std::string("value");
    const std::string before = a.toString();
    pjson b;
    b["old"] = static_cast<int64_t>(1);
    b = std::move(a);
    CHECK_EQ(b.toString(), before);
    CHECK_EQ(a.getType(), pjson::jsonNull);
}

TEST(copyfrom_deep_copies) {
    pjson a;
    a["arr"] = std::vector<double>({1.5, 2.5});
    pjson b;
    b.copyFrom(a);
    CHECK_EQ(b.toString(), a.toString());
    b["arr"][0] = double(9.9);
    expectDouble(a["arr"][0], 1.5);
}

TEST(unique_ptr_owns_ordinary_root_values) {
    pjson::unique_ptr owned(new pjson());
    CHECK(owned != nullptr);
    (*owned)["value"] = static_cast<int64_t>(1);
    expectInt((*owned)["value"], int64_t(1));
}
