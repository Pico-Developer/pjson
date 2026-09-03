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
// Serialization policy controls and non-vivifying value-access APIs.
//
#include "pjson.h"
#include "test_harness.h"
#include "test_util.h"

#include <climits>
#include <cstdint>
#include <ios>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

using ByteDance::pjson;

namespace {
    int64_t mustGetInt(const pjson& value) {
        int64_t out = 0;
        CHECK(value.tryGet(out));
        return out;
    }

    // Serializes through the ostream API and asserts that the sink stayed healthy.
    std::string streamed(const pjson& value, const pjson::SerializeOptions& options) {
        std::ostringstream out;
        value.write(out, options);
        CHECK(out.good());
        return out.str();
    }

    // Checks the byte-level postcondition promised by asciiOnly serialization.
    bool isAscii(const std::string& text) {
        for (size_t i = 0; i < text.size(); ++i) {
            if (static_cast<unsigned char>(text[i]) >= 0x80)
                return false;
        }
        return true;
    }

} // namespace

//===----------------------------------------------------------------------===//
// SerializeOptions behavior across string and ostream sinks
//===----------------------------------------------------------------------===//

TEST(serialize_options_defaults_match_convenience_api) {
    pjson::SerializeOptions defaults;
    CHECK_EQ(defaults.maxOutputBytes, size_t(64) * 1024U * 1024U);

    pjson value;
    value["z"] = static_cast<int64_t>(3);
    value["a"][0] = std::string("text\n");

    pjson::SerializeOptions compact;
    CHECK_EQ(value.toString(compact), value.toString());
    CHECK_EQ(streamed(value, compact), value.toString());

    pjson::SerializeOptions pretty = pjson::SerializeOptions::prettyPrinted();
    CHECK_EQ(streamed(value, pretty), value.toString(pretty));
}

TEST(serialize_options_custom_indentation) {
    pjson value;
    value["a"][0]["b"] = static_cast<int64_t>(1);

    pjson::SerializeOptions options = pjson::SerializeOptions::prettyPrinted();
    options.indentWidth = 4;
    CHECK_EQ(value.toString(options),
             std::string("{\n    \"a\": [\n        {\n            \"b\": 1\n        }\n    ]\n}"));

    options.indentWidth = 1;
    options.indentCharacter = '\t';
    CHECK_EQ(value.toString(options),
             std::string("{\n\t\"a\": [\n\t\t{\n\t\t\t\"b\": 1\n\t\t}\n\t]\n}"));

    options.indentWidth = 0;
    CHECK_EQ(value.toString(options), std::string("{\n\"a\": [\n{\n\"b\": 1\n}\n]\n}"));
}

TEST(serialize_options_invalid_indent_falls_back_and_compact_ignores_it) {
    pjson value;
    value["a"][0] = static_cast<int64_t>(1);

    pjson::SerializeOptions options = pjson::SerializeOptions::prettyPrinted();
    options.indentWidth = 3;
    options.indentCharacter = 'x';
    CHECK_EQ(value.toString(options), std::string("{\n   \"a\": [\n      1\n   ]\n}"));

    options.pretty = false;
    options.indentWidth = static_cast<size_t>(-1);
    CHECK_EQ(value.toString(options), std::string("{\"a\":[1]}"));
}

TEST(serialize_options_pretty_indent_overflow_throws_length_error_and_write_fails) {
    pjson value;
    value["a"][0] =
        static_cast<int64_t>(1); // nested enough that pretty indentation reaches depth 2

    pjson::SerializeOptions options = pjson::SerializeOptions::prettyPrinted();
    options.indentWidth = static_cast<size_t>(-1);

    bool threwLengthError = false;
    try {
        (void)value.toString(options);
    } catch (const std::length_error&) {
        threwLengthError = true;
    }
    CHECK(threwLengthError);

    std::ostringstream out;
    value.write(out, options);
    CHECK(out.fail());

    bool threwStreamFailure = false;
    std::ostringstream throwingOut;
    throwingOut.exceptions(std::ios::failbit);
    try {
        value.write(throwingOut, options);
    } catch (const std::ios_base::failure&) {
        threwStreamFailure = true;
    }
    CHECK(threwStreamFailure);
}

TEST(serialize_options_output_byte_budget_is_exact_and_atomic) {
    pjson value;
    value["key"] = std::string("value");
    const std::string compact = value.toString();

    pjson::SerializeOptions exact;
    exact.maxOutputBytes = compact.size();
    CHECK_EQ(value.toString(exact), compact);
    CHECK_EQ(streamed(value, exact), compact);

    pjson::SerializeOptions shortBudget = exact;
    shortBudget.maxOutputBytes = compact.size() - 1U;
    bool threw = false;
    try {
        (void)value.toString(shortBudget);
    } catch (const std::length_error&) {
        threw = true;
    }
    CHECK(threw);

    std::ostringstream out;
    value.write(out, shortBudget);
    CHECK(out.fail());
    CHECK(out.str().empty());

    bool streamThrew = false;
    std::ostringstream throwingOut;
    throwingOut.exceptions(std::ios::failbit);
    try {
        value.write(throwingOut, shortBudget);
    } catch (const std::ios_base::failure&) {
        streamThrew = true;
    }
    CHECK(streamThrew);
    CHECK(throwingOut.str().empty());

    pjson::SerializeOptions unlimited = shortBudget;
    unlimited.maxOutputBytes = 0;
    CHECK_EQ(value.toString(unlimited), compact);
}

TEST(serialize_options_output_budget_counts_pretty_escaped_values_and_keys) {
    const std::string unicode = "\xC3\xA9";
    pjson value;
    value[unicode] = unicode;

    pjson::SerializeOptions options = pjson::SerializeOptions::prettyPrinted();
    options.escapeNonAscii = true;
    options.indentWidth = 4;
    options.maxOutputBytes = 0;
    const std::string expected = value.toString(options);
    CHECK(expected.find("\\u00e9") != std::string::npos);

    options.maxOutputBytes = expected.size();
    CHECK_EQ(value.toString(options), expected);
    CHECK_EQ(streamed(value, options), expected);

    options.maxOutputBytes = expected.size() - 1U;
    bool threw = false;
    try {
        (void)value.toString(options);
    } catch (const std::length_error&) {
        threw = true;
    }
    CHECK(threw);

    std::ostringstream out;
    value.write(out, options);
    CHECK(out.fail());
    CHECK(out.str().empty());
}

TEST(serialize_options_empty_containers_stay_inline) {
    pjson value;
    value["array"].resetTo(pjson::jsonArray);
    value["object"].resetTo(pjson::jsonObject);

    pjson::SerializeOptions options = pjson::SerializeOptions::prettyPrinted();
    options.indentWidth = 1;
    CHECK_EQ(value.toString(options), std::string("{\n \"array\": [],\n \"object\": {}\n}"));
}

TEST(serialize_options_key_order_applies_at_every_depth) {
    pjson value;
    value["a"]["a"] = static_cast<int64_t>(1);
    value["a"]["z"] = static_cast<int64_t>(2);
    value["z"] = static_cast<int64_t>(3);

    pjson::SerializeOptions ascending;
    CHECK_EQ(value.toString(ascending), std::string("{\"a\":{\"a\":1,\"z\":2},\"z\":3}"));

    pjson::SerializeOptions descending;
    descending.keyOrder = pjson::SerializeOptions::DescendingKeys;
    CHECK_EQ(value.toString(descending), std::string("{\"z\":3,\"a\":{\"z\":2,\"a\":1}}"));
    CHECK_EQ(streamed(value, descending), value.toString(descending));
}

TEST(serialize_options_ascii_only_values_and_keys) {
    const std::string unicode = std::string("\xC2\x80") + "\xC3\xA9" + "\xE2\x82\xAC" +
                                "\xF0\x9F\x98\x80" + "\xF4\x8F\xBF\xBF";
    pjson value;
    value[unicode] = unicode;

    pjson::SerializeOptions options;
    options.escapeNonAscii = true;
    const std::string expected = "{\"\\u0080\\u00e9\\u20ac\\ud83d\\ude00\\udbff\\udfff\":"
                                 "\"\\u0080\\u00e9\\u20ac\\ud83d\\ude00\\udbff\\udfff\"}";
    const std::string text = value.toString(options);
    CHECK_EQ(text, expected);
    CHECK(isAscii(text));
    CHECK_EQ(streamed(value, options), text);

    pjson_test::Parsed reparsed = pjson_test::parse(text);
    CHECK(reparsed != nullptr);
    if (reparsed)
        CHECK(*reparsed == value);
}

TEST(serialize_options_invalid_utf8_to_string_throws_and_write_sets_failbit) {
    const char raw[] = {static_cast<char>(0xFF), static_cast<char>(0xE2), static_cast<char>(0x82),
                        static_cast<char>(0xED), static_cast<char>(0xA0), static_cast<char>(0x80)};
    pjson value;
    value = std::string(raw, sizeof(raw));

    pjson::SerializeOptions options;
    options.escapeNonAscii = true;

    bool defaultThrew = false;
    try {
        (void)value.toString();
    } catch (const std::invalid_argument&) {
        defaultThrew = true;
    }
    CHECK(defaultThrew);

    bool threw = false;
    try {
        (void)value.toString(options);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    std::ostringstream out;
    value.write(out, options);
    CHECK(out.fail());

    bool threwStreamFailure = false;
    std::ostringstream throwingOut;
    throwingOut.exceptions(std::ios::failbit);
    try {
        value.write(throwingOut, options);
    } catch (const std::ios_base::failure&) {
        threwStreamFailure = true;
    }
    CHECK(threwStreamFailure);

    pjson invalidKey;
    invalidKey[std::string(raw, sizeof(raw))] = int64_t(1);
    bool keyThrew = false;
    try {
        (void)invalidKey.toString();
    } catch (const std::invalid_argument&) {
        keyThrew = true;
    }
    CHECK(keyThrew);

    std::ostringstream keyOut;
    invalidKey.write(keyOut);
    CHECK(keyOut.fail());
}

TEST(serialize_options_preserve_ascii_and_required_escapes) {
    pjson value;
    value = std::string("ASCII \" \\ \b \f \n \r \t ") + static_cast<char>(0x01) +
            static_cast<char>(0x7F);
    pjson::SerializeOptions options;
    options.escapeNonAscii = true;
    CHECK_EQ(value.toString(options), std::string("\"ASCII \\\" \\\\ \\b \\f \\n \\r \\t \\u0001") +
                                          static_cast<char>(0x7F) + "\"");
}

//===----------------------------------------------------------------------===//
// Non-vivifying indexed lookup and strict typed extraction
//===----------------------------------------------------------------------===//

TEST(value_find_index_is_non_vivifying_and_mutable) {
    pjson value;
    value = std::vector<int64_t>({10, 20, 30});

    CHECK(value.hasIndex(0));
    CHECK(value.hasIndex(2));
    CHECK(value.hasIndex(-1));
    CHECK(value.hasIndex(-3));
    CHECK(!value.hasIndex(3));
    CHECK(!value.hasIndex(-4));
    CHECK(!value.hasIndex(INT_MIN));

    pjson* element = value.find(-2);
    CHECK(element != nullptr);
    if (element)
        *element = static_cast<int64_t>(99);
    CHECK_EQ(value.size(), size_t(3));
    const pjson* updated = value.find(1);
    CHECK(updated != nullptr);
    if (updated)
        CHECK_EQ(mustGetInt(*updated), int64_t(99));
}

TEST(value_find_index_const_and_wrong_types_do_not_mutate) {
    pjson array;
    array = std::vector<int64_t>({1});
    const pjson& constArray = array;
    static_assert(std::is_same<decltype(constArray.find(0)), const pjson*>::value,
                  "const indexed lookup must return const pjson*");
    CHECK(constArray.find(0) != nullptr);
    CHECK(constArray.find(1) == nullptr);
    const size_t zero = 0;
    const size_t one = 1;
    static_assert(std::is_same<decltype(constArray.findIndex(zero)), const pjson*>::value,
                  "const size_t lookup must return const pjson*");
    CHECK(constArray.findIndex(zero) != nullptr);
    CHECK(constArray.findIndex(one) == nullptr);
    CHECK(constArray.findIndex(std::numeric_limits<size_t>::max()) == nullptr);

    pjson empty;
    empty.resetTo(pjson::jsonArray);
    CHECK(empty.find(-1) == nullptr);
    CHECK_EQ(empty.size(), size_t(0));

    pjson scalar;
    scalar = static_cast<int64_t>(7);
    CHECK(scalar.find(0) == nullptr);
    CHECK(!scalar.hasIndex(0));
    CHECK(scalar.isInt());
    CHECK_EQ(mustGetInt(scalar), int64_t(7));
}

TEST(value_tryget_node_strict_matrix_and_untouched_failures) {
    pjson value;
    int64_t integer = 91;
    double floating = 9.5;
    bool boolean = true;
    std::string string = "sentinel";
    pjson::StringView view;

    value = static_cast<int64_t>(42);
    CHECK(value.tryGet(integer));
    CHECK_EQ(integer, int64_t(42));
    CHECK(value.tryGet(floating));
    CHECK_EQ(floating, 42.0);
    CHECK(!value.tryGet(boolean));
    CHECK_EQ(boolean, true);
    CHECK(!value.tryGet(string));
    CHECK_EQ(string, std::string("sentinel"));
    CHECK(!value.tryGet(view));
    CHECK(view.data() == nullptr);

    value = double(3.5);
    integer = 91;
    CHECK(!value.tryGet(integer));
    CHECK_EQ(integer, int64_t(91));
    CHECK(value.tryGet(floating));
    CHECK_EQ(floating, 3.5);

    value = false;
    CHECK(value.tryGet(boolean));
    CHECK_EQ(boolean, false);
}

TEST(value_tryget_string_view_is_copy_free_and_handles_nul) {
    const char raw[] = {'a', '\0', 'b'};
    pjson value;
    value = std::string(raw, sizeof(raw));

    pjson::StringView view;
    CHECK(value.tryGet(view));
    CHECK_EQ(view.size(), size_t(3));
    CHECK(!view.empty());
    CHECK(view.data() != nullptr);
    CHECK_EQ(view.data()[0], 'a');
    CHECK_EQ(view.data()[1], '\0');
    CHECK_EQ(view.data()[2], 'b');

    std::string copy;
    CHECK(value.tryGet(copy));
    CHECK_EQ(copy.size(), size_t(3));
    CHECK(view.data() != copy.data());

    value = std::string();
    CHECK(value.tryGet(view));
    CHECK(view.empty());
    CHECK_EQ(view.size(), size_t(0));
}

TEST(value_tryget_keyed_and_indexed_overloads) {
    pjson object;
    object["i"] = static_cast<int64_t>(7);
    object["d"] = double(2.5);
    object["b"] = true;
    object["s"] = std::string("value");

    int64_t integer = -1;
    double floating = -1.0;
    bool boolean = false;
    std::string string = "old";
    pjson::StringView view;
    CHECK(object.tryGet(std::string("i"), integer));
    CHECK_EQ(integer, int64_t(7));
    CHECK(object.tryGet("i", floating));
    CHECK_EQ(floating, 7.0);
    CHECK(object.tryGet("b", boolean));
    CHECK_EQ(boolean, true);
    CHECK(object.tryGet(std::string("s"), string));
    CHECK_EQ(string, std::string("value"));
    CHECK(object.tryGet("s", view));
    CHECK_EQ(std::string(view.data(), view.size()), std::string("value"));

    pjson array;
    array[0] = static_cast<int64_t>(11);
    array[1] = double(4.5);
    array[2] = false;
    array[3] = std::string("tail");
    CHECK(array.tryGet(0, integer));
    CHECK_EQ(integer, int64_t(11));
    CHECK(array.tryGet(0, floating));
    CHECK_EQ(floating, 11.0);
    CHECK(array.tryGet(-2, boolean));
    CHECK_EQ(boolean, false);
    CHECK(array.tryGet(-1, string));
    CHECK_EQ(string, std::string("tail"));
    CHECK(array.tryGet(3, view));
    CHECK_EQ(std::string(view.data(), view.size()), std::string("tail"));
}

TEST(value_tryget_child_failures_leave_outputs_untouched) {
    pjson object;
    object["number"] = static_cast<int64_t>(5);
    const size_t objectSize = object.size();
    const char* nullKey = nullptr;

    int64_t integer = 77;
    double floating = 8.5;
    bool boolean = true;
    std::string string = "keep";
    pjson::StringView view;
    pjson held;
    held = std::string("held");
    CHECK(held.tryGet(view));
    const char* viewData = view.data();

    CHECK(!object.tryGet("missing", integer));
    CHECK(!object.tryGet("number", boolean));
    CHECK(!object.tryGet(nullKey, floating));
    CHECK(!object.tryGet(nullKey, string));
    CHECK(!object.tryGet(nullKey, view));
    CHECK_EQ(integer, int64_t(77));
    CHECK_EQ(floating, 8.5);
    CHECK_EQ(boolean, true);
    CHECK_EQ(string, std::string("keep"));
    CHECK_EQ(view.data(), viewData);
    CHECK_EQ(object.size(), objectSize);

    pjson array;
    array[0] = static_cast<int64_t>(1);
    CHECK(!array.tryGet(1, integer));
    CHECK(!array.tryGet(-2, string));
    CHECK(!array.tryGet(0, view));
    CHECK_EQ(integer, int64_t(77));
    CHECK_EQ(string, std::string("keep"));
    CHECK_EQ(view.data(), viewData);
    CHECK_EQ(array.size(), size_t(1));
}

TEST(value_null_key_safe_lookup_apis) {
    pjson object;
    object["key"] = std::vector<int64_t>({1, 2});
    const char* nullKey = nullptr;
    int64_t integer = 12;

    CHECK(object.find(nullKey) == nullptr);
    CHECK(!object.hasKey(nullKey));
    CHECK(!object.tryGet(nullKey, integer));
    CHECK(!object.erase(nullKey));
    CHECK_EQ(integer, int64_t(12));
    CHECK_EQ(object.size(), size_t(1));
}
