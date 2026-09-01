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
// PJSON-COR-001 regression matrix: object names must be preserved byte-for-byte,
// including embedded U+0000, through every length-aware std::string API. The
// const char* overloads intentionally keep NUL-terminated semantics.
//
#include "pjson.h"
#include "test_harness.h"
#include "test_util.h"

#include <string>
#include <vector>

using namespace ByteDance;

namespace {

    // Builds a std::string with an explicit embedded NUL so the byte length is
    // preserved regardless of C-string truncation.
    std::string nulKey(const char* aPrefix, const char* aSuffix) {
        std::string key(aPrefix);
        key.push_back('\0');
        key += aSuffix;
        return key;
    }

} // namespace

//===----------------------------------------------------------------------===//
// "a" and "a\u0000b" remain distinct through build, read, update, and erase.
//===----------------------------------------------------------------------===//
TEST(embedded_nul_keys_are_distinct) {
    const std::string shortKey = "a";
    const std::string longKey = nulKey("a", "b"); // "a\0b", length 3

    pjson root;
    root[shortKey] = int64_t(1);
    root[longKey] = int64_t(2);

    // Two distinct members, not one aliased through c_str().
    CHECK_EQ(root.size(), size_t(2));
    CHECK(root.hasKey(shortKey));
    CHECK(root.hasKey(longKey));

    int64_t v = 0;
    CHECK(root.tryGet(shortKey, v));
    CHECK_EQ(v, int64_t(1));
    CHECK(root.tryGet(longKey, v));
    CHECK_EQ(v, int64_t(2));

    // Update each independently.
    root[longKey] = int64_t(20);
    CHECK(root.tryGet(shortKey, v));
    CHECK_EQ(v, int64_t(1));
    CHECK(root.tryGet(longKey, v));
    CHECK_EQ(v, int64_t(20));

    // Erase the long key; the short key survives.
    CHECK(root.erase(longKey));
    CHECK_EQ(root.size(), size_t(1));
    CHECK(root.hasKey(shortKey));
    CHECK(!root.hasKey(longKey));
    CHECK(root.find(longKey) == nullptr);
}

//===----------------------------------------------------------------------===//
// Parsing an object with both "a" and "a\u0000b" preserves both members.
//===----------------------------------------------------------------------===//
TEST(embedded_nul_keys_round_trip_through_parse) {
    // {"a":1,"a\u0000b":2}
    const std::string doc = "{\"a\":1,\"a\\u0000b\":2}";
    pjson_test::Parsed parsed = pjson_test::parse(doc);
    CHECK(parsed != nullptr);
    if (!parsed)
        return;

    CHECK_EQ(parsed->size(), size_t(2));

    const std::string shortKey = "a";
    const std::string longKey = nulKey("a", "b");

    int64_t v = 0;
    CHECK(parsed->tryGet(shortKey, v));
    CHECK_EQ(v, int64_t(1));
    CHECK(parsed->tryGet(longKey, v));
    CHECK_EQ(v, int64_t(2));

    // Serialization keeps both keys, so re-parsing recovers the same structure.
    pjson_test::Parsed reparsed = pjson_test::parse(parsed->toString());
    CHECK(reparsed != nullptr);
    if (reparsed)
        CHECK(*reparsed == *parsed);
}

//===----------------------------------------------------------------------===//
// Empty names and U+0000 at the beginning, middle, and end all stay distinct.
//===----------------------------------------------------------------------===//
TEST(embedded_nul_keys_position_matrix) {
    std::vector<std::string> keys;
    keys.push_back(std::string());    // empty name
    keys.push_back(nulKey("", "x"));  // "\0x" (NUL at beginning)
    keys.push_back(nulKey("x", "y")); // "x\0y" (NUL in middle)
    keys.push_back(nulKey("z", ""));  // "z\0" (NUL at end)

    pjson root;
    for (size_t i = 0; i < keys.size(); ++i)
        root[keys[i]] = int64_t(i);

    CHECK_EQ(root.size(), keys.size());
    for (size_t i = 0; i < keys.size(); ++i) {
        int64_t v = -1;
        CHECK(root.hasKey(keys[i]));
        CHECK(root.tryGet(keys[i], v));
        CHECK_EQ(v, int64_t(i));
    }

    // Const find path resolves on full length too.
    const pjson& cref = root;
    const pjson* mid = cref.find(nulKey("x", "y"));
    CHECK(mid != nullptr);
}

//===----------------------------------------------------------------------===//
// JSON Pointer and equality preserve embedded-NUL names.
//===----------------------------------------------------------------------===//
TEST(embedded_nul_keys_pointer_and_equality) {
    const std::string longKey = nulKey("a", "b");

    pjson root;
    root[longKey]["inner"] = int64_t(7);

    // Build the pointer with the escaping helper so the NUL byte survives.
    const std::string pointer = "/" + pjson::escapePointerToken(longKey) + "/inner";
    const pjson* target = root.findPointer(pointer);
    CHECK(target != nullptr);
    int64_t v = 0;
    if (target)
        CHECK(target->tryGet(v));
    CHECK_EQ(v, int64_t(7));

    // A deep copy keeps the key and compares equal.
    pjson copy = root;
    CHECK(copy == root);
    CHECK(copy.hasKey(longKey));
}

//===----------------------------------------------------------------------===//
// The const char* overloads keep NUL-terminated behavior (documented contract).
//===----------------------------------------------------------------------===//
TEST(embedded_nul_cstring_overloads_truncate_by_contract) {
    pjson root;
    root[std::string("a")] = int64_t(1);
    root[nulKey("a", "b")] = int64_t(2);

    // "a\0b" as a C string is seen as "a": the const char* lookup matches the
    // short key, demonstrating the documented distinction from std::string.
    const char* truncating = "a\0b"; // compiler treats as "a"
    CHECK(root.hasKey(truncating));
    int64_t v = 0;
    CHECK(root.tryGet(truncating, v));
    CHECK_EQ(v, int64_t(1));
}
