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
// PJSON-SER-001/002: valid, stable output; deterministic key order; and an
// overflow-safe output-size limit tested at limit-1, limit, and limit+1.
//
#include "pjson.h"
#include "test_harness.h"
#include "test_util.h"

#include <sstream>
#include <string>

using namespace ByteDance;

//===----------------------------------------------------------------------===//
// The output-size limit is exact: exactly-limit succeeds, limit-plus-one fails,
// both for toString() and write(). This confirms the boundary arithmetic is
// off-by-one-safe.
//===----------------------------------------------------------------------===//
TEST(output_size_limit_boundary) {
    // A five-element array of single-digit ints serializes to "[1,2,3,4,5]" (11 bytes).
    pjson arr;
    for (int64_t i = 1; i <= 5; ++i)
        arr += i;
    const std::string full = arr.toString();
    const size_t exact = full.size();
    CHECK_EQ(exact, size_t(11));

    pjson::SerializeOptions atLimit;
    atLimit.maxOutputBytes = exact; // limit == output size: succeeds
    CHECK_EQ(arr.toString(atLimit), full);

    pjson::SerializeOptions belowLimit;
    belowLimit.maxOutputBytes = exact - 1; // limit-1: must fail
    bool threw = false;
    try {
        (void)arr.toString(belowLimit);
    } catch (const std::length_error&) {
        threw = true;
    }
    CHECK(threw);

    pjson::SerializeOptions abovePlusOne;
    abovePlusOne.maxOutputBytes = exact + 1; // limit+1: comfortably succeeds
    CHECK_EQ(arr.toString(abovePlusOne), full);

    // write() enforces the same budget through failbit.
    std::ostringstream tooSmall;
    arr.write(tooSmall, belowLimit);
    CHECK(tooSmall.fail());

    std::ostringstream justRight;
    arr.write(justRight, atLimit);
    CHECK(!justRight.fail());
    CHECK_EQ(justRight.str(), full);
}

//===----------------------------------------------------------------------===//
// toString() and write() are byte-for-byte equivalent for the same options.
//===----------------------------------------------------------------------===//
TEST(tostring_and_write_are_equivalent) {
    pjson_test::Parsed doc =
        pjson_test::parse("{\"b\":[1,2,{\"x\":true}],\"a\":\"hi\",\"n\":18446744073709551615}");
    CHECK(doc != nullptr);
    if (!doc)
        return;

    const pjson::SerializeOptions options[] = {
        pjson::SerializeOptions(),
        pjson::SerializeOptions::prettyPrinted(),
    };
    for (const pjson::SerializeOptions& opt : options) {
        const std::string viaString = doc->toString(opt);
        std::ostringstream viaStream;
        doc->write(viaStream, opt);
        CHECK(!viaStream.fail());
        CHECK_EQ(viaString, viaStream.str());
    }
}

//===----------------------------------------------------------------------===//
// Deterministic key order: ascending and descending are exact reverses, and
// output re-parses to a structurally equal document regardless of order.
//===----------------------------------------------------------------------===//
TEST(deterministic_key_order) {
    pjson obj = pjson::object();
    obj["c"] = int64_t(3);
    obj["a"] = int64_t(1);
    obj["b"] = int64_t(2);

    pjson::SerializeOptions asc;
    asc.keyOrder = pjson::SerializeOptions::AscendingKeys;
    pjson::SerializeOptions desc;
    desc.keyOrder = pjson::SerializeOptions::DescendingKeys;

    CHECK_EQ(obj.toString(asc), std::string("{\"a\":1,\"b\":2,\"c\":3}"));
    CHECK_EQ(obj.toString(desc), std::string("{\"c\":3,\"b\":2,\"a\":1}"));

    pjson_test::Parsed reAsc = pjson_test::parse(obj.toString(asc));
    pjson_test::Parsed reDesc = pjson_test::parse(obj.toString(desc));
    CHECK(reAsc != nullptr);
    CHECK(reDesc != nullptr);
    if (reAsc && reDesc)
        CHECK(*reAsc == *reDesc); // order does not affect structural equality
}
