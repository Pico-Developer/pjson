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
// PJSON-COR-002 regression matrix: every public copy/move/swap/assignment must
// be memory-safe when the source aliases the destination, including ancestor and
// descendant relationships. These cases are intended to run clean under
// AddressSanitizer, UndefinedBehaviorSanitizer, and leak checking.
//
#include "pjson.h"
#include "test_harness.h"

#include <string>
#include <utility>

using namespace ByteDance;

//===----------------------------------------------------------------------===//
// The canonical descendant move-assignment case from the requirements doc.
//===----------------------------------------------------------------------===//
TEST(aliasing_move_assign_from_descendant) {
    pjson root;
    root["child"]["value"] = std::int64_t{7};
    pjson& child = root["child"];
    root = std::move(child); // must not use freed storage
    // After the move, root became the former child object.
    CHECK(root.isObject());
    int64_t v = 0;
    CHECK(root.tryGet("value", v));
    CHECK_EQ(v, int64_t(7));
}

//===----------------------------------------------------------------------===//
// Copy-assigning a root from one of its descendants.
//===----------------------------------------------------------------------===//
TEST(aliasing_copy_assign_from_descendant) {
    pjson root;
    root["child"]["value"] = std::int64_t{11};
    root = root["child"]; // copy-and-swap keeps the deep copy alive
    CHECK(root.isObject());
    int64_t v = 0;
    CHECK(root.tryGet("value", v));
    CHECK_EQ(v, int64_t(11));
}

//===----------------------------------------------------------------------===//
// Assigning a descendant from its root (destination inside the source subtree).
//===----------------------------------------------------------------------===//
TEST(aliasing_copy_assign_descendant_from_root) {
    pjson root;
    root["a"] = std::int64_t{1};
    root["child"]["value"] = std::int64_t{2};
    root["child"] = root; // descendant becomes a copy of the whole document
    // root still valid and internally consistent.
    CHECK(root.isObject());
    CHECK(root.hasKey("a"));
    CHECK(root.hasKey("child"));
    const pjson* child = root.find("child");
    CHECK(child != nullptr);
    if (child)
        CHECK(child->hasKey("a"));
}

//===----------------------------------------------------------------------===//
// Move-assigning a descendant from its root.
//===----------------------------------------------------------------------===//
TEST(aliasing_move_assign_descendant_from_root) {
    pjson root;
    root["a"] = std::int64_t{1};
    root["child"]["value"] = std::int64_t{2};
    root["child"] = std::move(root); // snapshots the ancestor; cannot steal it
    CHECK(root.isObject());
    CHECK(root.hasKey("a"));
    const pjson* child = root.find("child");
    CHECK(child != nullptr);
    if (child != nullptr) {
        CHECK(child->isObject());
        CHECK(child->hasKey("a"));
        CHECK(child->hasKey("child"));
    }
}

TEST(aliasing_generic_insertions_snapshot_ancestors) {
    pjson appended;
    appended["value"] = int64_t(7);
    appended.pushBack(std::move(appended));
    CHECK(appended.isArray());
    CHECK_EQ(appended.size(), size_t(1));
    int64_t value = 0;
    CHECK(appended.find(0) != nullptr);
    CHECK(appended.find(0)->tryGet("value", value));
    CHECK_EQ(value, int64_t(7));

    pjson inserted;
    inserted["before"] = true;
    inserted.insertOrAssign("snapshot", std::move(inserted));
    CHECK(inserted.isObject());
    const pjson* snapshot = inserted.find("snapshot");
    CHECK(snapshot != nullptr);
    CHECK(snapshot->hasKey("before"));
    CHECK(!snapshot->hasKey("snapshot"));

    pjson siblings;
    siblings["source"]["value"] = int64_t(13);
    pjson& source = siblings["source"];
    siblings.insertOrAssign("destination", std::move(source));
    CHECK(siblings.find("source") != nullptr);
    CHECK(siblings.find("source")->isNull());
    CHECK(siblings.find("destination") != nullptr);
    int64_t siblingValue = 0;
    CHECK(siblings.find("destination")->tryGet("value", siblingValue));
    CHECK_EQ(siblingValue, int64_t(13));

    pjson arraySiblings;
    arraySiblings[0]["value"] = int64_t(17);
    pjson& first = arraySiblings[0];
    arraySiblings.pushBack(std::move(first));
    CHECK_EQ(arraySiblings.size(), size_t(2));
    CHECK(arraySiblings.find(0)->isNull());
    int64_t arrayValue = 0;
    CHECK(arraySiblings.find(1)->tryGet("value", arrayValue));
    CHECK_EQ(arrayValue, int64_t(17));
}

TEST(aliasing_generic_insertions_survive_container_promotion) {
    pjson arraySource;
    arraySource[0]["value"] = int64_t(9);
    pjson& arrayChild = arraySource[0];
    arrayChild.insertOrAssign("root", std::move(arraySource));
    CHECK(arraySource.isArray());
    const pjson* promoted = arraySource.find(0);
    CHECK(promoted != nullptr);
    const pjson* rootSnapshot = promoted == nullptr ? nullptr : promoted->find("root");
    CHECK(rootSnapshot != nullptr);
    CHECK(rootSnapshot->isArray());

    pjson objectSource;
    objectSource["child"]["value"] = int64_t(11);
    pjson& objectChild = objectSource["child"];
    objectChild.pushBack(std::move(objectSource));
    CHECK(objectSource.isObject());
    const pjson* childArray = objectSource.find("child");
    CHECK(childArray != nullptr);
    CHECK(childArray->isArray());
    CHECK_EQ(childArray->size(), size_t(1));
}

//===----------------------------------------------------------------------===//
// Sibling-to-sibling assignment.
//===----------------------------------------------------------------------===//
TEST(aliasing_sibling_assignment) {
    pjson root;
    root["x"]["v"] = std::int64_t{100};
    root["y"] = std::string("old");
    root["y"] = root["x"];
    const pjson* y = root.find("y");
    CHECK(y != nullptr);
    if (y) {
        int64_t v = 0;
        CHECK(y->tryGet("v", v));
        CHECK_EQ(v, int64_t(100));
    }
    // Original sibling unaffected.
    const pjson* x = root.find("x");
    CHECK(x != nullptr);
    if (x) {
        int64_t v = 0;
        CHECK(x->tryGet("v", v));
        CHECK_EQ(v, int64_t(100));
    }
}

//===----------------------------------------------------------------------===//
// Self copy and self move.
//===----------------------------------------------------------------------===//
TEST(aliasing_self_copy_and_move) {
    pjson a;
    a["k"] = std::string("v");
    pjson& ref = a;
    a = ref; // self copy
    int64_t unused = 0;
    (void)unused;
    std::string s;
    CHECK(a.tryGet("k", s));
    CHECK_EQ(s, std::string("v"));

    pjson& mref = a;
    a = std::move(mref); // self move
    CHECK(a.tryGet("k", s));
    CHECK_EQ(s, std::string("v"));
}

//===----------------------------------------------------------------------===//
// Swapping a root with a descendant is rejected as a safe no-op: they must be in
// the same allocator domain (they are) but swap of overlapping storage would be
// unsound, so pjson leaves both operands valid. We assert no crash and a valid
// tree afterward.
//===----------------------------------------------------------------------===//
TEST(aliasing_swap_root_and_descendant_is_safe) {
    pjson root;
    root["child"]["value"] = std::int64_t{5};
    pjson& child = root["child"];
    CHECK(!root.canSwap(child));
    CHECK(!child.canSwap(root));
    root.swap(child); // overlapping swap; must not corrupt memory
    // The tree must remain traversable and destructible without error.
    CHECK(root.isObject());
    const std::string serialized = root.toString();
    CHECK(!serialized.empty());
}
