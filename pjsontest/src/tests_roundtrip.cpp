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
// Round-trip stability (compact + pretty), formatting details, and a
// deterministic fuzz that builds random nested documents and asserts that
// serialize -> parse -> serialize is stable.
//
#include "pjson.h"
#include "test_harness.h"
#include "test_util.h"

#include <random>
#include <string>
#include <vector>

using namespace ByteDance;

namespace {

    void expectDouble(const pjson& value, double expected) {
        double actual = 0.0;
        CHECK(value.tryGet(actual));
        CHECK_EQ(actual, expected);
    }

    pjson::SerializeOptions prettyOptions() {
        return pjson::SerializeOptions::prettyPrinted();
    }

} // namespace

//===----------------------------------------------------------------------===//
// Number formatting: stable round-tripping with preserved numeric kind
//===----------------------------------------------------------------------===//
TEST(format_double_is_short_and_stable) {
    pjson j;
    j = double(1.5);
    CHECK_EQ(j.toString(), std::string("1.5"));
    j = double(0.1);
    CHECK_EQ(j.toString(), std::string("0.1"));
    j = double(100.0);
    CHECK_EQ(j.toString(), std::string("100.0")); // keeps .0 marker
    j = double(2.5);
    CHECK_EQ(j.toString(), std::string("2.5"));
}

TEST(format_double_large_integral_value_round_trips_with_double_kind) {
    pjson value;
    value = double(2738421882738290.0);
    pjson_test::Parsed reparsed = pjson_test::parse(value.toString());
    CHECK(reparsed != nullptr);
    CHECK(reparsed->isDouble());
    CHECK(*reparsed == value);
}

TEST(format_double_high_precision_round_trips) {
    pjson j;
    j = double(3.141592653589793);
    pjson_test::Parsed rt = pjson_test::parse(j.toString());
    CHECK(rt != nullptr);
    expectDouble(*rt, 3.141592653589793);
}

TEST(format_int_has_no_decimal) {
    pjson j;
    j = static_cast<int64_t>(42);
    CHECK_EQ(j.toString(), std::string("42"));
    j = static_cast<int64_t>(-7);
    CHECK_EQ(j.toString(), std::string("-7"));
}

TEST(format_negative_and_zero) {
    pjson j;
    j = static_cast<int64_t>(0);
    CHECK_EQ(j.toString(), std::string("0"));
    j = double(0.0);
    CHECK_EQ(j.toString(), std::string("0.0"));
    j = double(-3.5);
    CHECK_EQ(j.toString(), std::string("-3.5"));
}

//===----------------------------------------------------------------------===//
// Compact round-trip: parse(serialize(x)) preserves the JSON value.
//===----------------------------------------------------------------------===//
TEST(compact_round_trip_preserves_value) {
    pjson o;
    o["s"] = std::string("text with \"quotes\" and \\slash");
    o["i"] = static_cast<int64_t>(-42);
    o["d"] = double(3.5);
    o["b"] = true;
    o["nil"]; // null
    o["arr"] = std::vector<int64_t>({1, 2, 3});
    o["nested"]["deep"]["deeper"] = std::string("value");

    std::string compact = o.toString();
    pjson_test::Parsed p1 = pjson_test::parse(compact);
    CHECK(p1 != nullptr);
    if (p1 != nullptr)
        CHECK(*p1 == o);
}

//===----------------------------------------------------------------------===//
// Pretty output re-parses to the same value.
//===----------------------------------------------------------------------===//
TEST(pretty_reparses_to_same_value) {
    pjson o;
    o["a"] = static_cast<int64_t>(1);
    o["b"]["c"] = std::vector<std::string>({"x", "y"});
    o["d"] = std::vector<int64_t>({1, 2, 3});

    pjson::SerializeOptions prettyOpts = prettyOptions();
    // Preserve deep pretty-writer traversal coverage without intentionally
    // generating quadratic indentation that exceeds the secure output budget.
    prettyOpts.indentWidth = 0;
    std::string compact = o.toString();
    std::string pretty = o.toString(prettyOpts);
    CHECK_NE(compact, pretty); // formatting differs

    pjson_test::Parsed pp = pjson_test::parse(pretty);
    CHECK(pp != nullptr);
    if (pp != nullptr)
        CHECK(*pp == o);
}

//===----------------------------------------------------------------------===//
// Empty and edge structures round-trip
//===----------------------------------------------------------------------===//
TEST(empty_structures_round_trip) {
    pjson a;
    a.resetTo(pjson::jsonArray);
    pjson_test::Parsed ra = pjson_test::parse(a.toString());
    CHECK(ra != nullptr);
    CHECK_EQ(ra->getType(), pjson::jsonArray);
    CHECK_EQ(ra->toString(), a.toString());

    pjson m;
    m.resetTo(pjson::jsonObject);
    pjson_test::Parsed rm = pjson_test::parse(m.toString());
    CHECK(rm != nullptr);
    CHECK_EQ(rm->getType(), pjson::jsonObject);

    pjson n;
    CHECK_EQ(n.toString(), std::string("null"));
    pjson_test::Parsed rn = pjson_test::parse(n.toString());
    CHECK(rn != nullptr);
    CHECK_EQ(rn->getType(), pjson::jsonNull);
}

TEST(nested_empty_containers_round_trip) {
    pjson_test::Parsed p = pjson_test::parse("{\"a\":[],\"b\":{},\"c\":[[],{}]}");
    CHECK(p != nullptr);
    std::string compact = p->toString();
    pjson_test::Parsed p2 = pjson_test::parse(compact);
    CHECK(p2 != nullptr);
    if (p2 != nullptr)
        CHECK(*p2 == *p);
}

//===----------------------------------------------------------------------===//
// The whole value lifecycle is iterative (explicit stacks, not call
// recursion): build, serialize, copy, compare, and destroy a document far
// deeper than the parser's depth guard would ever allow, with no
// stack-overflow crash.
//===----------------------------------------------------------------------===//
TEST(deep_nesting_no_stack_overflow) {
    // ~100x the default parse maxDepth of 512, and well past the depth at which
    // a recursive implementation overflows the stack (~10k under sanitizers).
    const int depth = 50000;

    // Build depth-deep nested objects: {"a":{"a":{ ... {"a":1} ... }}}.
    pjson root;
    {
        pjson* cur = &root;
        for (int i = 0; i < depth; ++i) {
            cur = &((*cur)["a"]);
        }
        *cur = static_cast<int64_t>(1);
    }

    // Serialize (compact + pretty).
    pjson::SerializeOptions prettyOpts = prettyOptions();
    // Exercise the deep pretty traversal without producing quadratic
    // indentation that intentionally exceeds the default output budget.
    prettyOpts.indentWidth = 0;
    std::string compact = root.toString();
    // depth '{' + depth '"a":' (4 chars) + "1" + depth '}'.
    CHECK_EQ(compact.size(), static_cast<size_t>(depth) * 6 + 1);
    CHECK_EQ(compact[0], '{');
    CHECK_EQ(compact[compact.size() - 1], '}');
    CHECK(root.toString(prettyOpts).size() > compact.size());

    // Deep copy + deep equality.
    pjson copy = root;
    CHECK(copy == root);

    // clear() tears the children down iteratively.
    copy.clear();
    CHECK(copy.empty());

    // Deep arrays too: [[[ ... 1 ... ]]].
    pjson arr;
    {
        pjson* a = &arr;
        for (int i = 0; i < depth; ++i) {
            a = &((*a)[0]);
        }
        *a = static_cast<int64_t>(1);
    }
    std::string arrCompact = arr.toString();
    CHECK_EQ(arrCompact.size(), static_cast<size_t>(depth) * 2 + 1);
    pjson arrCopy = arr;
    CHECK(arrCopy == arr);
    // root, arr, arrCopy all destruct here without overflowing the stack.
}

//===----------------------------------------------------------------------===//
// Deterministic fuzz: random documents survive a serialize/parse cycle
//===----------------------------------------------------------------------===//
namespace {

    // Builds a random pjson value up to the given depth using the provided RNG.
    void build_random(pjson& node, std::mt19937& rng, int depth) {
        std::uniform_int_distribution<int> kind(0, depth > 0 ? 6 : 4);
        switch (kind(rng)) {
            case 0:
                node.reset();
                break; // null
            case 1:
                node = (std::uniform_int_distribution<int>(0, 1)(rng) != 0);
                break;
            case 2:
                node = static_cast<int64_t>(
                    std::uniform_int_distribution<long long>(-1000000, 1000000)(rng));
                break;
            case 3:
                node = std::uniform_real_distribution<double>(-1000.0, 1000.0)(rng);
                break;
            case 4: {
                // Random string including some characters that require escaping.
                static const char pool[] = "abc \"\\\n\t/\x01 z";
                std::uniform_int_distribution<int> len(0, 8);
                std::uniform_int_distribution<int> pick(0, sizeof(pool) - 2);
                std::string s;
                int n = len(rng);
                for (int i = 0; i < n; ++i)
                    s += pool[pick(rng)];
                node = s;
                break;
            }
            case 5: {
                // Array of random children.
                node.resetTo(pjson::jsonArray);
                std::uniform_int_distribution<int> len(0, 4);
                int n = len(rng);
                for (int i = 0; i < n; ++i) {
                    build_random(node[i], rng, depth - 1);
                }
                break;
            }
            default: {
                // Map of random children under generated keys.
                node.resetTo(pjson::jsonObject);
                std::uniform_int_distribution<int> len(0, 4);
                int n = len(rng);
                for (int i = 0; i < n; ++i) {
                    std::string key = "k" + std::to_string(i);
                    build_random(node[key], rng, depth - 1);
                }
                break;
            }
        }
    }

} // namespace

TEST(fuzz_round_trip_preserves_value) {
    std::mt19937 rng(0xC0FFEE); // fixed seed -> deterministic, reproducible
    const pjson::SerializeOptions prettyOpts = prettyOptions();
    for (int iter = 0; iter < 500; ++iter) {
        pjson doc;
        build_random(doc, rng, 4);

        // Compact: parse(serialize(x)) must preserve the represented value.
        std::string compact = doc.toString();
        pjson_test::Parsed rc = pjson_test::parse(compact);
        CHECK(rc != nullptr);
        if (rc)
            CHECK(*rc == doc);

        // Pretty: must re-parse to the same represented value.
        std::string pretty = doc.toString(prettyOpts);
        pjson_test::Parsed rp = pjson_test::parse(pretty);
        CHECK(rp != nullptr);
        if (rp)
            CHECK(*rp == doc);
    }
}

TEST(fuzz_never_throws_on_arbitrary_bytes) {
    // Feeding random bytes to the parser must never throw or crash; it may
    // succeed or return null, but must terminate cleanly.
    std::mt19937 rng(0xBADF00D);
    std::uniform_int_distribution<int> byte(0, 255);
    std::uniform_int_distribution<int> len(0, 40);
    int handled = 0;
    for (int iter = 0; iter < 1000; ++iter) {
        std::string s;
        int n = len(rng);
        for (int i = 0; i < n; ++i)
            s += static_cast<char>(byte(rng));
        pjson_test::Parsed p = pjson_test::parse(s); // must not throw
        if (p) {
            // If it parsed, it must re-serialize and re-parse consistently.
            std::string out = p->toString();
            pjson_test::Parsed p2 = pjson_test::parse(out);
            CHECK(p2 != nullptr);
            if (p2)
                CHECK(*p2 == *p);
        }
        ++handled;
    }
    CHECK_EQ(handled, 1000);
}
