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
// Complex mutation scenarios: building deep trees, overwriting with type
// changes, growing/shrinking arrays, erase in various orders, clear-and-rebuild,
// and aliasing safety under move/copy.
//
#include "pjson.h"
#include "test_harness.h"
#include <string>
#include <vector>

using namespace ByteDance;

namespace {

    void expectInt(const pjson& value, int64_t expected) {
        int64_t actual = 0;
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
// Build a deep mixed tree and read every leaf back.
//===----------------------------------------------------------------------===//
TEST(mutate_build_deep_mixed_tree) {
    pjson j;
    j["user"]["name"] = "Ada";
    j["user"]["roles"] = std::vector<std::string>({"admin", "dev"});
    j["user"]["settings"]["theme"] = "dark";
    j["user"]["settings"]["volume"] = static_cast<int64_t>(7);
    j["user"]["history"][0]["action"] = "login";
    j["user"]["history"][0]["ts"] = static_cast<int64_t>(1000);
    j["user"]["history"][1]["action"] = "logout";
    j["user"]["history"][1]["ts"] = static_cast<int64_t>(2000);

    expectString(j["user"]["name"], "Ada");
    CHECK_EQ(j["user"]["roles"].size(), size_t(2));
    expectString(j["user"]["roles"][1], "dev");
    expectInt(j["user"]["settings"]["volume"], int64_t(7));
    CHECK_EQ(j["user"]["history"].size(), size_t(2));
    expectString(j["user"]["history"][1]["action"], "logout");

    // The whole thing round-trips.
    pjson::unique_ptr rt = pjson::parse(j.toString());
    CHECK(rt != nullptr);
    CHECK(*rt == j);
}

//===----------------------------------------------------------------------===//
// Overwriting a key repeatedly with different types keeps only the last.
//===----------------------------------------------------------------------===//
TEST(mutate_overwrite_changes_type) {
    pjson j;
    j["x"] = static_cast<int64_t>(1);
    CHECK(j["x"].isInt());
    j["x"] = std::string("s");
    CHECK(j["x"].isString());
    j["x"] = std::vector<int64_t>({1, 2});
    CHECK(j["x"].isArray());
    j["x"] = true;
    CHECK(j["x"].isBool());
    j["x"] = double(3.5);
    CHECK(j["x"].isDouble());
    j["x"].reset();
    CHECK(j["x"].isNull());
    CHECK_EQ(j.size(), size_t(1)); // still exactly one key
}

//===----------------------------------------------------------------------===//
// Promoting a scalar to a container by indexing replaces its value.
//===----------------------------------------------------------------------===//
TEST(mutate_scalar_to_container_promotion) {
    pjson s;
    s = std::string("hi");
    s[0] = static_cast<int64_t>(1); // indexing turns it into an array
    CHECK(s.isArray());
    CHECK_EQ(s.size(), size_t(1));
    expectInt(s[0], int64_t(1));

    pjson m;
    m = static_cast<int64_t>(5);
    m["k"] = static_cast<int64_t>(1); // keying turns it into an object
    CHECK(m.isObject());
    CHECK(m.hasKey("k"));
}

//===----------------------------------------------------------------------===//
// Array growth auto-fills gaps with null; shrink via erase preserves order.
//===----------------------------------------------------------------------===//
TEST(mutate_array_grow_with_gaps) {
    pjson j;
    j[3] = "fourth";
    CHECK_EQ(j.size(), size_t(4));
    CHECK(j[0].isNull());
    CHECK(j[1].isNull());
    CHECK(j[2].isNull());
    expectString(j[3], "fourth");
}

TEST(mutate_array_erase_preserves_order) {
    pjson j;
    j = std::vector<int64_t>({0, 1, 2, 3, 4});
    CHECK(j.erase(size_t(2))); // remove the middle
    CHECK_EQ(j.toString(), std::string("[0,1,3,4]"));
    CHECK(j.erase(size_t(0))); // remove the front
    CHECK_EQ(j.toString(), std::string("[1,3,4]"));
    CHECK(j.erase(j.size() - 1)); // remove the back
    CHECK_EQ(j.toString(), std::string("[1,3]"));
    CHECK(!j.erase(size_t(99))); // out of range no-op
}

TEST(mutate_array_erase_all_front) {
    pjson j;
    for (int i = 0; i < 50; ++i)
        j[i] = static_cast<int64_t>(i);
    while (j.size() > 0) {
        CHECK(j.erase(size_t(0)));
    }
    CHECK(j.isArray()); // still an (empty) array
    CHECK(j.empty());
}

TEST(mutate_array_erase_all_back) {
    pjson j;
    for (int i = 0; i < 50; ++i)
        j[i] = static_cast<int64_t>(i);
    while (j.size() > 0) {
        CHECK(j.erase(j.size() - 1));
    }
    CHECK(j.empty());
}

//===----------------------------------------------------------------------===//
// Object erase: middle keys, all keys, and re-add after erase.
//===----------------------------------------------------------------------===//
TEST(mutate_object_erase_and_readd) {
    pjson j;
    j["a"] = static_cast<int64_t>(1);
    j["b"] = static_cast<int64_t>(2);
    j["c"] = static_cast<int64_t>(3);
    CHECK(j.erase("b"));
    CHECK_EQ(j.size(), size_t(2));
    CHECK(!j.hasKey("b"));
    CHECK(!j.erase("b"));              // already gone
    j["b"] = static_cast<int64_t>(20); // re-add
    expectInt(j["b"], int64_t(20));
    CHECK_EQ(j.size(), size_t(3));
}

TEST(mutate_object_erase_frees_subtree) {
    pjson j;
    j["big"]["nested"]["deep"] = std::vector<int64_t>({1, 2, 3});
    j["keep"] = static_cast<int64_t>(1);
    CHECK(j.erase("big")); // frees the whole subtree
    CHECK_EQ(j.size(), size_t(1));
    CHECK(j.hasKey("keep"));
}

//===----------------------------------------------------------------------===//
// clear() empties in place (keeping the container type), then rebuild.
//===----------------------------------------------------------------------===//
TEST(mutate_clear_and_rebuild) {
    pjson arr;
    arr = std::vector<int64_t>({1, 2, 3});
    arr.clear();
    CHECK(arr.isArray());
    CHECK(arr.empty());
    arr += static_cast<int64_t>(10);
    arr += static_cast<int64_t>(20);
    CHECK_EQ(arr.size(), size_t(2));

    pjson obj;
    obj["a"] = static_cast<int64_t>(1);
    obj["b"] = static_cast<int64_t>(2);
    obj.clear();
    CHECK(obj.isObject());
    CHECK(obj.empty());
    obj["c"] = static_cast<int64_t>(3);
    CHECK_EQ(obj.size(), size_t(1));
}

//===----------------------------------------------------------------------===//
// Editing a parsed document in place, then re-serializing.
//===----------------------------------------------------------------------===//
TEST(mutate_edit_parsed_document) {
    pjson::unique_ptr p = pjson::parse(R"({ "list":[10,20,30], "meta":{"v":1}, "drop":true })");
    CHECK(p != nullptr);
    pjson& j = *p;

    j["list"][1] = static_cast<int64_t>(99);  // change a value
    j["list"][3] = "appended";                // extend the array
    j["meta"]["v"] = static_cast<int64_t>(2); // edit nested
    j["meta"]["new"] = std::vector<int64_t>({7, 8});
    CHECK(j.erase("drop")); // remove a key

    expectInt(j["list"][1], int64_t(99));
    CHECK_EQ(j["list"].size(), size_t(4));
    expectString(j["list"][3], "appended");
    expectInt(j["meta"]["v"], int64_t(2));
    CHECK(!j.hasKey("drop"));

    // Still valid JSON after all the edits.
    pjson::unique_ptr rt = pjson::parse(j.toString());
    CHECK(rt != nullptr);
    CHECK(*rt == j);
}

//===----------------------------------------------------------------------===//
// Negative-index edits reach elements from the end.
//===----------------------------------------------------------------------===//
TEST(mutate_negative_index_edits) {
    pjson j;
    j = std::vector<int64_t>({1, 2, 3});
    j[-1] = static_cast<int64_t>(30); // last
    j[-3] = static_cast<int64_t>(10); // first
    expectInt(j[0], int64_t(10));
    expectInt(j[2], int64_t(30));
    // Out-of-range negative clamps to the front element.
    j[-10] = static_cast<int64_t>(0);
    expectInt(j[0], int64_t(0));
}

//===----------------------------------------------------------------------===//
// Building a large object then reading it back (stress the map).
//===----------------------------------------------------------------------===//
TEST(mutate_large_object) {
    pjson j;
    const int n = 500;
    for (int i = 0; i < n; ++i) {
        j["k" + std::to_string(i)] = static_cast<int64_t>(i);
    }
    CHECK_EQ(j.size(), size_t(n));
    CHECK_EQ(j.keys().size(), size_t(n));
    expectInt(j["k0"], int64_t(0));
    expectInt(j["k499"], int64_t(499));
    // Erase half.
    for (int i = 0; i < n; i += 2) {
        CHECK(j.erase("k" + std::to_string(i)));
    }
    CHECK_EQ(j.size(), size_t(n / 2));
    CHECK(!j.hasKey("k0"));
    CHECK(j.hasKey("k1"));
}

//===----------------------------------------------------------------------===//
// Aliasing safety: assigning from a child / self must not corrupt.
//===----------------------------------------------------------------------===//
TEST(mutate_assign_from_child) {
    pjson j;
    j["outer"]["inner"] = std::vector<int64_t>({1, 2, 3});
    j = j["outer"]; // copy-and-swap keeps this safe
    CHECK(j.hasKey("inner"));
    CHECK_EQ(j["inner"].size(), size_t(3));
}

TEST(mutate_swap_via_move) {
    pjson a;
    a["x"] = static_cast<int64_t>(1);
    pjson b;
    b["y"] = static_cast<int64_t>(2);
    pjson tmp = std::move(a);
    a = std::move(b);
    b = std::move(tmp);
    // a and b have exchanged contents.
    CHECK(a.hasKey("y"));
    CHECK(b.hasKey("x"));
}

//===----------------------------------------------------------------------===//
// resetTo transitions between every type free the previous storage cleanly.
//===----------------------------------------------------------------------===//
TEST(mutate_reset_to_every_type) {
    pjson j;
    j["a"] = std::vector<int64_t>({1, 2, 3}); // start as object holding an array
    j.resetTo(pjson::jsonArray);
    CHECK(j.isArray());
    CHECK(j.empty());
    j += static_cast<int64_t>(1);
    CHECK_EQ(j.size(), size_t(1));
    j.resetTo(pjson::jsonString);
    CHECK(j.isString());
    expectString(j, "");
    j.resetTo(pjson::jsonObject);
    CHECK(j.isObject());
    CHECK(j.empty());
    j.resetTo(pjson::jsonNumberInt);
    CHECK(j.isInt());
    expectInt(j, int64_t(0));
    j.resetTo(pjson::jsonNumberDouble);
    CHECK(j.isDouble());
    j.resetTo(pjson::jsonBoolean);
    CHECK(j.isBool());
    j.resetTo(pjson::jsonNull);
    CHECK(j.isNull());
}

//===----------------------------------------------------------------------===//
// resetIfNeeded only rebuilds when the type differs: an existing container of
// the requested type keeps its contents, while a mismatched type is replaced.
//===----------------------------------------------------------------------===//
TEST(mutate_reset_if_needed) {
    pjson j;
    j += static_cast<int64_t>(1);
    j += static_cast<int64_t>(2);
    j += static_cast<int64_t>(3);
    CHECK(j.isArray());
    // Already an array -> contents preserved.
    j.resetIfNeeded(pjson::jsonArray);
    CHECK(j.isArray());
    CHECK_EQ(j.size(), size_t(3));
    // Different type -> rebuilt as an empty value of that type.
    j.resetIfNeeded(pjson::jsonObject);
    CHECK(j.isObject());
    CHECK(j.empty());
    // Idempotent on the fresh type too.
    j["k"] = static_cast<int64_t>(7);
    j.resetIfNeeded(pjson::jsonObject);
    CHECK(j.isObject());
    CHECK_EQ(j.size(), size_t(1));
}

//===----------------------------------------------------------------------===//
// swap() exchanges two nodes' contents in place, including differing types and
// self-swap, without copying or leaking.
//===----------------------------------------------------------------------===//
TEST(mutate_swap_contents) {
    pjson a;
    a["x"] = static_cast<int64_t>(1);
    pjson b;
    b += std::vector<std::string>({"one", "two"});

    a.swap(b);
    // a is now the array, b is now the object.
    CHECK(a.isArray());
    CHECK_EQ(a.size(), size_t(2));
    expectString(a[0], "one");
    CHECK(b.isObject());
    CHECK(b.hasKey("x"));
    expectInt(b["x"], int64_t(1));

    // Self-swap is a harmless no-op.
    a.swap(a);
    CHECK(a.isArray());
    CHECK_EQ(a.size(), size_t(2));
}

//===----------------------------------------------------------------------===//
// Append operators accumulate and can mix scalar + vector appends.
//===----------------------------------------------------------------------===//
TEST(mutate_append_accumulation) {
    pjson j;
    j += static_cast<int64_t>(1);
    j += std::vector<int64_t>({2, 3});
    j += "four";
    j += std::vector<std::string>({"five", "six"});
    j += true;
    CHECK_EQ(j.size(), size_t(7));
    expectInt(j[0], int64_t(1));
    expectString(j[3], "four");
    {
        bool tail = false;
        CHECK(j[6].tryGet(tail));
        CHECK_EQ(tail, true);
    }
}

//===----------------------------------------------------------------------===//
// A full add -> edit -> delete -> rebuild lifecycle stays consistent.
//===----------------------------------------------------------------------===//
TEST(mutate_full_lifecycle) {
    pjson doc;
    // Add.
    doc["users"][0]["id"] = static_cast<int64_t>(1);
    doc["users"][0]["name"] = "Ada";
    doc["users"][1]["id"] = static_cast<int64_t>(2);
    doc["users"][1]["name"] = "Bob";
    doc["count"] = static_cast<int64_t>(2);
    CHECK_EQ(doc["users"].size(), size_t(2));

    // Edit.
    doc["users"][0]["name"] = "Ada Lovelace";
    doc["count"] = static_cast<int64_t>(3); // deliberately inconsistent, then fixed

    // Delete user 0, fix count.
    CHECK(doc["users"].erase(size_t(0)));
    doc["count"] = static_cast<int64_t>(doc["users"].size());
    CHECK_EQ(doc["users"].size(), size_t(1));
    expectString(doc["users"][0]["name"], "Bob");
    expectInt(doc["count"], int64_t(1));

    // Rebuild from scratch on the same object.
    doc.clear();
    CHECK(doc.empty());
    doc["ok"] = true;
    CHECK_EQ(doc.size(), size_t(1));

    // Everything still serializes and round-trips.
    pjson::unique_ptr rt = pjson::parse(doc.toString());
    CHECK(rt != nullptr);
    CHECK(*rt == doc);
}
