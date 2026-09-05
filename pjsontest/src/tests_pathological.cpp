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
// Deterministic pathological-input tests. These use fixed, moderate payload
// sizes and observable parser budgets rather than wall-clock thresholds, so
// they exercise unusually expensive paths without introducing timing flakes.
//===----------------------------------------------------------------------===//
#include "pjson.h"
#include "pjson_parser.h"
#include "test_harness.h"
#include "test_util.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>

using namespace ByteDance;
using pjson_test::parse;

namespace {

    const char* const kNodeBudgetError = "document too large (node budget exceeded)";
    const char* const kInputBudgetError = "input exceeds maxInputBytes";

    // Generates a wide array while keeping the serialized size linear and predictable.
    std::string makeFlatArray(size_t width) {
        std::string json;
        json.reserve(width * 2U + 1U);
        json += '[';
        for (size_t i = 0; i < width; ++i) {
            if (i != 0)
                json += ',';
            json += '0';
        }
        json += ']';
        return json;
    }

    // Generates a wide object and exposes the final value's byte offset for error assertions.
    std::string makeFlatObject(size_t width, size_t& lastValueOffset) {
        std::string json;
        json.reserve(width * 14U + 2U);
        json += '{';
        for (size_t i = 0; i < width; ++i) {
            if (i != 0)
                json += ',';
            json += "\"k";
            json += std::to_string(i);
            json += "\":";
            lastValueOffset = json.size();
            json += '0';
        }
        json += '}';
        return json;
    }

    // Gates exact bit-level expectations that assume an IEEE 754 binary64 double.
    bool isIeeeBinary64() {
        return std::numeric_limits<double>::is_iec559 && std::numeric_limits<double>::radix == 2 &&
               std::numeric_limits<double>::digits == 53 &&
               std::numeric_limits<double>::max_exponent == 1024;
    }

    double doubleValue(const pjson& aValue) {
        double value = 0.0;
        CHECK(aValue.tryGet(value));
        return value;
    }

    int64_t intValue(const pjson& aValue) {
        int64_t value = 0;
        CHECK(aValue.tryGet(value));
        return value;
    }

    std::string stringValue(const pjson& aValue) {
        std::string value;
        CHECK(aValue.tryGet(value));
        return value;
    }

} // namespace

// Mixed numeric equality must not round an integer through binary64. Above
// 2^53, adjacent integers can map to the same double, so comparison has to
// prove that the floating value is finite, integral, in range, and exactly
// representable before comparing it with the stored int64_t.
TEST(pathological_mixed_numeric_equality_is_exact_above_binary64_integer_precision) {
    if (!isIeeeBinary64()) {
        CHECK(std::numeric_limits<double>::is_specialized);
        return;
    }

    pjson exactInteger;
    exactInteger = int64_t(9007199254740992LL);
    pjson sameDouble;
    sameDouble = double(9007199254740992.0);
    CHECK(exactInteger == sameDouble);
    CHECK(sameDouble == exactInteger);

    pjson adjacentInteger;
    adjacentInteger = int64_t(9007199254740993LL);
    CHECK(adjacentInteger != sameDouble);
    CHECK(sameDouble != adjacentInteger);

    pjson roundedBeyondInt64;
    roundedBeyondInt64 = double(9223372036854775808.0);
    pjson maxInteger;
    maxInteger = std::numeric_limits<int64_t>::max();
    CHECK(maxInteger != roundedBeyondInt64);
    CHECK(roundedBeyondInt64 != maxInteger);
}

// A long finite mantissa and a long, zero-padded exponent must be scanned in
// full without changing their values. Very large positive values fail at a
// stable location, while a very negative exponent is rejected unless the
// caller opts in to its lossy conversion to zero.
TEST(pathological_very_long_numeric_tokens) {
    const size_t digitCount = 65536;
    pJsonParser::Error err;

    const std::string longMantissa = "1." + std::string(digitCount, '0');
    auto mantissa = pjson_test::parse(longMantissa, err);
    CHECK(mantissa != nullptr);
    CHECK(err.ok);
    if (mantissa) {
        CHECK(mantissa->isDouble());
        CHECK_EQ(doubleValue(*mantissa), 1.0);
    }

    const std::string paddedExponent = "1e+" + std::string(digitCount, '0') + std::string("1");
    auto finiteExponent = pjson_test::parse(paddedExponent, err);
    CHECK(finiteExponent != nullptr);
    CHECK(err.ok);
    if (finiteExponent)
        CHECK_EQ(doubleValue(*finiteExponent), 10.0);

    // IEC 60559 implementations have infinities, so strtod must expose these
    // positive overflows and pjson must reject them rather than storing inf.
    if (std::numeric_limits<double>::has_infinity) {
        // A very long all-nines integer exceeds the exact 64-bit range, so the
        // default policy rejects it with the integer-range diagnostic before any
        // double fallback is attempted.
        const std::string hugeInteger(digitCount, '9');
        CHECK(pjson_test::parse(hugeInteger, err) == nullptr);
        CHECK(!err.ok);
        CHECK_EQ(err.offset, size_t(0));
        CHECK_EQ(err.line, size_t(1));
        CHECK_EQ(err.column, size_t(1));
        CHECK_EQ(err.message,
                 std::string("integer out of range; enable AllowLossyNumbers to store as double"));

        const std::string hugePositiveExponent = "1e+" + std::string(digitCount, '9');
        CHECK(pjson_test::parse(hugePositiveExponent, err) == nullptr);
        CHECK(!err.ok);
        CHECK_EQ(err.offset, size_t(0));
        CHECK_EQ(err.message, std::string("number out of range"));
    }

    const std::string hugeNegativeExponent = "1e-" + std::string(digitCount, '9');
    CHECK(pjson_test::parse(hugeNegativeExponent, err) == nullptr);
    CHECK(!err.ok);
    CHECK_EQ(err.code, pJsonParser::Error::NumberRange);
    pJsonParser::Options lossy;
    lossy.numberPolicy = pJsonParser::Options::AllowLossyNumbers;
    auto underflow = pjson_test::parse(hugeNegativeExponent, err, lossy);
    CHECK(underflow != nullptr);
    CHECK(err.ok);
    if (underflow) {
        CHECK(underflow->isDouble());
        CHECK(std::isfinite(doubleValue(*underflow)));
        if (isIeeeBinary64())
            CHECK_EQ(doubleValue(*underflow), 0.0);
    }
}

TEST(pathological_binary64_halfway_rounding) {
    if (!isIeeeBinary64()) {
        CHECK(std::numeric_limits<double>::is_specialized);
        return;
    }

    struct Case {
        const char* text;
        double expected;
    };
    const Case cases[] = {
        {"1.00000000000000011102230246251565404236316680908203125", 1.0},
        {"1.00000000000000011102230246251565404236316680908203126", std::nextafter(1.0, 2.0)},
        {"2.47032822920623272088284396434110686182529901307162382212792841250337753635104375e-324",
         0.0},
        {"2.47032822920623272088284396434110686182529901307162382212792841250337753635104376e-324",
         std::numeric_limits<double>::denorm_min()},
    };
    pJsonParser::Options lossy;
    lossy.numberPolicy = pJsonParser::Options::AllowLossyNumbers;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        pJsonParser::Error error;
        pjson value = pJsonParser(lossy).parse(cases[i].text, error);
        CHECK(error.ok);
        CHECK_EQ(doubleValue(value), cases[i].expected);
    }
}

TEST(pathological_random_binary64_round_trips_bit_exactly) {
    if (!isIeeeBinary64()) {
        CHECK(std::numeric_limits<double>::is_specialized);
        return;
    }

    uint64_t state = UINT64_C(0x9e3779b97f4a7c15);
    for (size_t i = 0; i < size_t(10000); ++i) {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        const uint64_t bits = state * UINT64_C(2685821657736338717);
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        if (!std::isfinite(value))
            continue;
        pjson node;
        node = value;
        pJsonParser::Error error;
        pjson reparsed = pJsonParser().parse(node.toString(), error);
        CHECK(error.ok);
        double result = 0.0;
        CHECK(reparsed.tryGet(result));
        CHECK(std::memcmp(&result, &value, sizeof(value)) == 0);
    }
}

// Exercise normal/subnormal/max-finite conversion at exact binary64 values.
// The literal expectations are intentionally conditional because C++ does not
// require double to use the IEC 60559 binary64 representation.
TEST(pathological_binary64_extremes_round_trip) {
    if (!isIeeeBinary64()) {
        CHECK(std::numeric_limits<double>::is_specialized);
        return;
    }

    // Literals and expected values at normal, subnormal, and range boundaries.
    struct NumericCase {
        const char* text;
        double expected;
    };
    const NumericCase cases[] = {
        {"4.9406564584124654e-324", std::numeric_limits<double>::denorm_min()},
        {"2.2250738585072014e-308", std::numeric_limits<double>::min()},
        {"1.7976931348623157e308", std::numeric_limits<double>::max()},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        auto value = parse(cases[i].text);
        CHECK(value != nullptr);
        if (!value)
            continue;
        CHECK(value->isDouble());
        CHECK(std::isfinite(doubleValue(*value)));
        CHECK_EQ(doubleValue(*value), cases[i].expected);

        const std::string encoded = value->toString();
        auto roundTrip = parse(encoded);
        CHECK(roundTrip != nullptr);
        if (roundTrip) {
            CHECK(roundTrip->isDouble());
            CHECK_EQ(doubleValue(*roundTrip), cases[i].expected);
        }
    }
}

TEST(pathological_negative_zero_parse_round_trip) {
    const char* literals[] = {"-0.0", "-0e0", "-0E+12"};
    for (size_t i = 0; i < sizeof(literals) / sizeof(literals[0]); ++i) {
        auto value = parse(literals[i]);
        CHECK(value != nullptr);
        if (!value)
            continue;
        CHECK(value->isDouble());
        CHECK_EQ(doubleValue(*value), 0.0);

        // Check the sign only on implementations that actually distinguish
        // signed zero. All supported IEC 60559 targets take this branch.
        if (std::signbit(-0.0)) {
            CHECK(std::signbit(doubleValue(*value)));
            CHECK_EQ(value->toString(), std::string("-0.0"));
            auto roundTrip = parse(value->toString());
            CHECK(roundTrip != nullptr);
            if (roundTrip)
                CHECK(std::signbit(doubleValue(*roundTrip)));
        }
    }
}

// maxNodes counts the root array plus each element. A wide document exactly at
// a configured budget succeeds; reducing that budget by one deterministically
// fails on the final element. The fixed width caps test memory independently
// of the much larger production default.
TEST(pathological_wide_array_node_budget_boundary) {
    const size_t width = 32768;
    const std::string json = makeFlatArray(width);
    CHECK_EQ(json.size(), width * 2U + 1U);

    pJsonParser::Options opts;
    opts.maxNodes = width + 1U;
    opts.maxInputBytes = json.size();
    pJsonParser::Error err;
    auto atLimit = pjson_test::parse(json, err, opts);
    CHECK(atLimit != nullptr);
    CHECK(err.ok);
    if (atLimit) {
        CHECK(atLimit->isArray());
        CHECK_EQ(atLimit->size(), width);
        CHECK_EQ(atLimit->toString(), json);
    }

    opts.maxNodes = width;
    CHECK(pjson_test::parse(json, err, opts) == nullptr);
    CHECK(!err.ok);
    CHECK_EQ(err.offset, json.size() - 2U);
    CHECK_EQ(err.line, size_t(1));
    CHECK_EQ(err.column, err.offset + 1U);
    CHECK_EQ(err.message, std::string(kNodeBudgetError));
}

// Object keys do not consume the value-node budget. This wide object therefore
// has the same root-plus-values boundary as a flat array, despite its larger
// byte footprint and map allocations.
TEST(pathological_wide_object_node_budget_boundary) {
    const size_t width = 8192;
    size_t lastValueOffset = 0;
    const std::string json = makeFlatObject(width, lastValueOffset);

    pJsonParser::Options opts;
    opts.maxNodes = width + 1U;
    opts.maxInputBytes = json.size();
    pJsonParser::Error err;
    auto atLimit = pjson_test::parse(json, err, opts);
    CHECK(atLimit != nullptr);
    CHECK(err.ok);
    if (atLimit) {
        CHECK(atLimit->isObject());
        CHECK_EQ(atLimit->size(), width);
        const pjson* last = atLimit->find("k8191");
        CHECK(last != nullptr);
        if (last)
            CHECK_EQ(intValue(*last), int64_t(0));
    }

    opts.maxNodes = width;
    CHECK(pjson_test::parse(json, err, opts) == nullptr);
    CHECK(!err.ok);
    CHECK_EQ(err.offset, lastValueOffset);
    CHECK_EQ(err.line, size_t(1));
    CHECK_EQ(err.column, lastValueOffset + 1U);
    CHECK_EQ(err.message, std::string(kNodeBudgetError));
}

// A payload rich in escaping has a predictable expansion factor. Parse both
// buffer and stream forms exactly at their byte budget, then prove that the
// same input is rejected before parsing when it is one byte over that budget.
TEST(pathological_large_escaped_payload_and_byte_budget) {
    const size_t repeats = 16384;
    std::string raw;
    raw.reserve(repeats * 5U);
    for (size_t i = 0; i < repeats; ++i) {
        raw += 'x';
        raw += '"';
        raw += '\\';
        raw += '\n';
        raw += '\0';
    }

    pjson source;
    source = raw;
    const std::string json = source.toString();
    CHECK_EQ(raw.size(), repeats * 5U);
    CHECK_EQ(json.size(), repeats * 13U + 2U);

    pJsonParser::Options opts;
    opts.maxInputBytes = json.size();
    pJsonParser::Error err;
    auto fromBuffer = pjson_test::parse(json, err, opts);
    CHECK(fromBuffer != nullptr);
    CHECK(err.ok);
    if (fromBuffer)
        CHECK_EQ(stringValue(*fromBuffer), raw);

    std::istringstream acceptedStream(json);
    auto fromStream = pjson_test::parseStream(acceptedStream, err, opts);
    CHECK(fromStream != nullptr);
    CHECK(err.ok);
    if (fromStream)
        CHECK_EQ(stringValue(*fromStream), raw);

    opts.maxInputBytes = json.size() - 1U;
    CHECK(pjson_test::parse(json, err, opts) == nullptr);
    CHECK(!err.ok);
    CHECK_EQ(err.offset, json.size() - 1U);
    CHECK_EQ(err.line, size_t(1));
    CHECK_EQ(err.column, json.size());
    CHECK_EQ(err.message, std::string(kInputBudgetError));

    std::istringstream rejectedStream(json);
    CHECK(pjson_test::parseStream(rejectedStream, err, opts) == nullptr);
    CHECK(!err.ok);
    CHECK_EQ(err.offset, json.size() - 1U);
    CHECK_EQ(err.line, size_t(1));
    CHECK_EQ(err.column, json.size());
    CHECK_EQ(err.message, std::string(kInputBudgetError));
}
