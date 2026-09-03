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
// PJSON-NUM-001/002/003: exact unsigned-integer support, the unrepresentable
// number policy, and explicit non-finite floating-point handling.
//
#include "pjson.h"
#include "pjson_parser.h"
#include "test_harness.h"
#include "test_util.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

using namespace ByteDance;

//===----------------------------------------------------------------------===//
// UINT64_MAX round-trips exactly instead of collapsing to a double.
//===----------------------------------------------------------------------===//
TEST(uint_max_round_trips_exactly) {
    const std::string doc = "18446744073709551615"; // UINT64_MAX
    pjson_test::Parsed p = pjson_test::parse(doc);
    CHECK(p != nullptr);
    if (!p)
        return;
    CHECK(p->isUInt());
    CHECK(p->isInteger());
    CHECK(!p->isInt());
    uint64_t v = 0;
    CHECK(p->tryGet(v));
    CHECK_EQ(v, std::numeric_limits<uint64_t>::max());
    // Exact decimal serialization, not 1.8446744073709552e+19.
    CHECK_EQ(p->toString(), doc);
}

//===----------------------------------------------------------------------===//
// The signed/unsigned boundary is classified exactly.
//===----------------------------------------------------------------------===//
TEST(int_uint_boundary_classification) {
    // INT64_MAX stays signed.
    pjson_test::Parsed maxSigned = pjson_test::parse("9223372036854775807");
    CHECK(maxSigned && maxSigned->isInt());

    // INT64_MAX + 1 becomes unsigned.
    pjson_test::Parsed firstUnsigned = pjson_test::parse("9223372036854775808");
    CHECK(firstUnsigned != nullptr);
    if (firstUnsigned) {
        CHECK(firstUnsigned->isUInt());
        uint64_t v = 0;
        CHECK(firstUnsigned->tryGet(v));
        CHECK_EQ(v, uint64_t(9223372036854775808ULL));
        // It does not fit int64_t, so a signed read fails cleanly.
        int64_t s = -1;
        CHECK(!firstUnsigned->tryGet(s));
        CHECK_EQ(s, int64_t(-1));
    }
}

//===----------------------------------------------------------------------===//
// Explicit uint64_t assignment keeps unsigned identity even for small values.
//===----------------------------------------------------------------------===//
TEST(uint_assignment_keeps_identity) {
    pjson v;
    v = uint64_t(7);
    CHECK(v.isUInt());
    CHECK(!v.isInt());
    // Cross-representation comparison is still exact: 7u == 7 == 7.0.
    pjson signedSeven;
    signedSeven = int64_t(7);
    CHECK(v == signedSeven);
    pjson doubleSeven;
    doubleSeven = double(7.0);
    CHECK(v == doubleSeven);
}

//===----------------------------------------------------------------------===//
// tryGet(uint64_t&) accepts a non-negative signed value; rejects a negative one.
//===----------------------------------------------------------------------===//
TEST(uint_tryget_from_signed) {
    pjson pos;
    pos = int64_t(42);
    uint64_t u = 0;
    CHECK(pos.tryGet(u));
    CHECK_EQ(u, uint64_t(42));

    pjson neg;
    neg = int64_t(-1);
    u = 999;
    CHECK(!neg.tryGet(u));
    CHECK_EQ(u, uint64_t(999)); // unchanged on failure
}

//===----------------------------------------------------------------------===//
// Unsigned vector assignment and append build unsigned children.
//===----------------------------------------------------------------------===//
TEST(uint_vector_assignment_and_append) {
    std::vector<uint64_t> values;
    values.push_back(1);
    values.push_back(std::numeric_limits<uint64_t>::max());

    pjson arr;
    arr = values;
    CHECK(arr.isArray());
    CHECK_EQ(arr.size(), size_t(2));
    const pjson* second = arr.find(1);
    CHECK(second != nullptr);
    if (second) {
        CHECK(second->isUInt());
        uint64_t v = 0;
        CHECK(second->tryGet(v));
        CHECK_EQ(v, std::numeric_limits<uint64_t>::max());
    }

    pjson appended;
    appended += uint64_t(5);
    CHECK(appended.isArray());
    CHECK_EQ(appended.size(), size_t(1));
    const pjson* elem = appended.find(0);
    CHECK(elem && elem->isUInt());
}

//===----------------------------------------------------------------------===//
// Values above UINT64_MAX are rejected by default and opt-in under lossy policy.
//===----------------------------------------------------------------------===//
TEST(number_above_uint64_policy) {
    const std::string doc = "18446744073709551616"; // UINT64_MAX + 1
    pJsonParser::Error err;
    pjson_test::Parsed rejected = pjson_test::parse(doc, err);
    CHECK(rejected == nullptr);
    CHECK(!err.ok);

    pJsonParser::Options lossy;
    lossy.numberPolicy = pJsonParser::Options::AllowLossyNumbers;
    pjson_test::Parsed allowed = pjson_test::parse(doc, lossy);
    CHECK(allowed != nullptr);
    if (allowed)
        CHECK(allowed->isDouble());
}

//===----------------------------------------------------------------------===//
// Non-finite doubles: default serialization fails; opt-in policies map them.
//===----------------------------------------------------------------------===//
TEST(non_finite_serialization_policy) {
    pjson nan;
    nan = std::numeric_limits<double>::quiet_NaN();

    // Default: RejectNonFinite -> toString throws.
    bool threw = false;
    try {
        (void)nan.toString();
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    // NonFiniteToNull emits null (legacy behavior, now explicit).
    pjson::SerializeOptions toNull;
    toNull.nonFinite = pjson::SerializeOptions::NonFiniteToNull;
    CHECK_EQ(nan.toString(toNull), std::string("null"));

    // NonFiniteToString emits sentinel strings.
    pjson posInf;
    posInf = std::numeric_limits<double>::infinity();
    pjson negInf;
    negInf = -std::numeric_limits<double>::infinity();
    pjson::SerializeOptions toStr;
    toStr.nonFinite = pjson::SerializeOptions::NonFiniteToString;
    CHECK_EQ(posInf.toString(toStr), std::string("\"Infinity\""));
    CHECK_EQ(negInf.toString(toStr), std::string("\"-Infinity\""));
    CHECK_EQ(nan.toString(toStr), std::string("\"NaN\""));
}

//===----------------------------------------------------------------------===//
// Non-finite streaming output follows the same policy (failbit by default).
//===----------------------------------------------------------------------===//
TEST(non_finite_stream_policy) {
    pjson inf;
    inf = std::numeric_limits<double>::infinity();
    std::ostringstream out;
    inf.write(out); // default RejectNonFinite
    CHECK(out.fail());

    std::ostringstream out2;
    pjson::SerializeOptions toNull;
    toNull.nonFinite = pjson::SerializeOptions::NonFiniteToNull;
    inf.write(out2, toNull);
    CHECK(!out2.fail());
    CHECK_EQ(out2.str(), std::string("null"));
}

//===----------------------------------------------------------------------===//
// Finite doubles still round-trip, including negative zero and subnormals.
//===----------------------------------------------------------------------===//
TEST(finite_double_round_trips) {
    const double values[] = {
        -0.0,    std::numeric_limits<double>::denorm_min(), std::numeric_limits<double>::max(), 1.0,
        123.456,
    };
    for (double d : values) {
        pjson v;
        v = d;
        const std::string text = v.toString();
        pjson_test::Parsed p = pjson_test::parse(text);
        CHECK(p != nullptr);
        if (!p)
            continue;
        double parsed = 0.0;
        CHECK(p->tryGet(parsed));
        // Exact bit-for-bit recovery for finite values.
        CHECK(std::memcmp(&parsed, &d, sizeof(double)) == 0);
    }
}
