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
// Schema validation: checking a document against a JSON-Schema-subset schema
// that is itself a pjson object. Covers each supported keyword, JSON-Pointer
// error paths, the collect-all-failures contract, and logical combinators.
//
#include "pjson.h"
#include "test_harness.h"
#include <string>
#include <vector>

using namespace ByteDance;

namespace {

    pjson::unique_ptr parseJson(const char* text) {
        return pjson::parse(std::string(text));
    }

    // Convenience: parse a schema and a document (both from JSON text) and return
    // whether the document validates, capturing errors.
    bool validates(const char* schemaText, const char* dataText,
                   std::vector<pjson::SchemaError>& errors) {
        pjson::unique_ptr schema = parseJson(schemaText);
        pjson::unique_ptr data = parseJson(dataText);
        if (!schema || !data)
            return false;
        return data->validate(*schema, errors);
    }

    bool hasMessageContaining(const std::vector<pjson::SchemaError>& errors,
                              const std::string& needle) {
        for (size_t i = 0; i < errors.size(); ++i) {
            if (errors[i].message.find(needle) != std::string::npos)
                return true;
        }
        return false;
    }

    bool validates(const char* schemaText, const char* dataText) {
        std::vector<pjson::SchemaError> errors;
        return validates(schemaText, dataText, errors);
    }

} // namespace

//===----------------------------------------------------------------------===//
// type
//===----------------------------------------------------------------------===//
TEST(schema_type_matches) {
    CHECK(validates(R"({"type":"string"})", R"("hi")"));
    CHECK(validates(R"({"type":"integer"})", "42"));
    CHECK(validates(R"({"type":"number"})", "42")); // integer is a number
    CHECK(validates(R"({"type":"number"})", "4.5"));
    CHECK(validates(R"({"type":"boolean"})", "true"));
    CHECK(validates(R"({"type":"null"})", "null"));
    CHECK(validates(R"({"type":"array"})", "[1,2]"));
    CHECK(validates(R"({"type":"object"})", R"({"k":1})"));
}

TEST(schema_type_mismatch_reports_path_and_message) {
    std::vector<pjson::SchemaError> errors;
    CHECK(!validates(R"({"type":"integer"})", R"("nope")", errors));
    CHECK_EQ(errors.size(), size_t(1));
    CHECK_EQ(errors[0].path, std::string("")); // root
    CHECK(errors[0].message.find("integer") != std::string::npos);
    CHECK(errors[0].message.find("string") != std::string::npos);
}

TEST(schema_type_integer_vs_number) {
    CHECK(!validates(R"({"type":"integer"})", "4.5")); // fractional not integer
    CHECK(validates(R"({"type":"integer"})", "4.0"));  // whole double is integer
}

TEST(schema_type_array_of_allowed) {
    CHECK(validates(R"({"type":["string","null"]})", R"("x")"));
    CHECK(validates(R"({"type":["string","null"]})", "null"));
    CHECK(!validates(R"({"type":["string","null"]})", "5"));
}

//===----------------------------------------------------------------------===//
// required / properties / additionalProperties
//===----------------------------------------------------------------------===//
TEST(schema_required_present) {
    CHECK(
        validates(R"({"type":"object","required":["name","age"]})", R"({"name":"Ada","age":36})"));
}

TEST(schema_required_missing) {
    std::vector<pjson::SchemaError> errors;
    CHECK(
        !validates(R"({"type":"object","required":["name","age"]})", R"({"name":"Ada"})", errors));
    CHECK_EQ(errors.size(), size_t(1));
    CHECK_EQ(errors[0].path, std::string(""));
    CHECK(errors[0].message.find("age") != std::string::npos);
}

TEST(schema_properties_recurse_with_path) {
    const char* schema =
        R"({"type":"object","properties":{
             "age":{"type":"integer"},
             "name":{"type":"string"}}})";
    std::vector<pjson::SchemaError> errors;
    CHECK(!validates(schema, R"({"age":"old","name":"Ada"})", errors));
    CHECK_EQ(errors.size(), size_t(1));
    CHECK_EQ(errors[0].path, std::string("/age")); // JSON-Pointer to the child
}

TEST(schema_additional_properties_false) {
    const char* schema =
        R"({"type":"object","properties":{"a":{"type":"integer"}},
            "additionalProperties":false})";
    CHECK(validates(schema, R"({"a":1})"));
    std::vector<pjson::SchemaError> errors;
    CHECK(!validates(schema, R"({"a":1,"b":2})", errors));
    CHECK_EQ(errors.size(), size_t(1));
    CHECK_EQ(errors[0].path, std::string("/b"));
}

TEST(schema_min_max_properties) {
    CHECK(!validates(R"({"minProperties":2})", R"({"a":1})"));
    CHECK(validates(R"({"minProperties":2})", R"({"a":1,"b":2})"));
    CHECK(!validates(R"({"maxProperties":1})", R"({"a":1,"b":2})"));
}

//===----------------------------------------------------------------------===//
// items / array constraints
//===----------------------------------------------------------------------===//
TEST(schema_items_applies_to_each_element) {
    CHECK(validates(R"({"type":"array","items":{"type":"integer"}})", "[1,2,3]"));
    std::vector<pjson::SchemaError> errors;
    CHECK(!validates(R"({"type":"array","items":{"type":"integer"}})", R"([1,"two",3])", errors));
    CHECK_EQ(errors.size(), size_t(1));
    CHECK_EQ(errors[0].path, std::string("/1")); // index of the bad element
}

TEST(schema_min_max_items) {
    CHECK(!validates(R"({"minItems":2})", "[1]"));
    CHECK(validates(R"({"minItems":2})", "[1,2]"));
    CHECK(!validates(R"({"maxItems":2})", "[1,2,3]"));
}

TEST(schema_unique_items) {
    CHECK(validates(R"({"uniqueItems":true})", "[1,2,3]"));
    CHECK(!validates(R"({"uniqueItems":true})", "[1,2,2]"));
    CHECK(!validates(R"({"uniqueItems":true})", R"([{"a":1},{"a":1}])")); // deep dup
}

//===----------------------------------------------------------------------===//
// numeric constraints
//===----------------------------------------------------------------------===//
TEST(schema_minimum_maximum) {
    CHECK(validates(R"({"minimum":0,"maximum":100})", "50"));
    CHECK(!validates(R"({"minimum":18})", "5"));
    CHECK(!validates(R"({"maximum":10})", "11"));
    CHECK(validates(R"({"minimum":18})", "18")); // inclusive
}

TEST(schema_exclusive_bounds) {
    CHECK(!validates(R"({"exclusiveMinimum":0})", "0"));
    CHECK(validates(R"({"exclusiveMinimum":0})", "1"));
    CHECK(!validates(R"({"exclusiveMaximum":10})", "10"));
}

TEST(schema_multiple_of) {
    CHECK(validates(R"({"multipleOf":5})", "15"));
    CHECK(!validates(R"({"multipleOf":5})", "13"));
    CHECK(validates(R"({"multipleOf":0.5})", "2.5"));
}

//===----------------------------------------------------------------------===//
// string constraints
//===----------------------------------------------------------------------===//
TEST(schema_length) {
    CHECK(!validates(R"({"minLength":3})", R"("ab")"));
    CHECK(validates(R"({"minLength":3})", R"("abc")"));
    CHECK(!validates(R"({"maxLength":3})", R"("abcd")"));
}

TEST(schema_pattern) {
    CHECK(validates(R"({"pattern":"^[a-z]+$"})", R"("hello")"));
    CHECK(!validates(R"({"pattern":"^[a-z]+$"})", R"("Hello1")"));
}

TEST(schema_pattern_redos_safety_policy) {
    pjson::unique_ptr schema = parseJson(R"({"pattern":"^(a+)+$","minLength":10})");
    pjson::unique_ptr value = parseJson(R"("aaaa")");
    CHECK(schema != nullptr);
    CHECK(value != nullptr);
    std::vector<pjson::SchemaError> errors;
    CHECK(!value->validate(*schema, errors));
    CHECK_EQ(errors.size(), size_t(2)); // policy failure + minLength (collect all)
    CHECK(errors[0].message.find("minLength") != std::string::npos ||
          errors[1].message.find("minLength") != std::string::npos);
    CHECK(errors[0].message.find("safety policy") != std::string::npos ||
          errors[1].message.find("safety policy") != std::string::npos);

    pjson::unique_ptr alternation = parseJson(R"({"pattern":"^(a|aa)+$"})");
    errors.clear();
    CHECK(!value->validate(*alternation, errors));
    CHECK(errors[0].message.find("safety policy") != std::string::npos);

    pjson::unique_ptr hugeRepeat = parseJson(R"({"pattern":"^a{1000000}$"})");
    errors.clear();
    CHECK(!value->validate(*hugeRepeat, errors));
    CHECK(errors[0].message.find("safety policy") != std::string::npos);
}

TEST(schema_pattern_size_limits_and_trusted_opt_in) {
    pjson schema;
    schema["pattern"] = std::string(257, 'a');
    pjson value;
    value = "a";
    std::vector<pjson::SchemaError> errors;
    CHECK(!value.validate(schema, errors));
    CHECK(errors[0].message.find("pattern exceeds") != std::string::npos);

    schema["pattern"] = "a";
    value = std::string(4097, 'a');
    errors.clear();
    CHECK(!value.validate(schema, errors));
    CHECK(errors[0].message.find("string exceeds") != std::string::npos);

    // Trusted applications may explicitly restore unrestricted behavior.
    pjson::SchemaOptions trusted = pjson::SchemaOptions::trustedRegex();
    errors.clear();
    CHECK(value.validate(schema, errors, trusted));
    CHECK(errors.empty());
}

//===----------------------------------------------------------------------===//
// const / enum
//===----------------------------------------------------------------------===//
TEST(schema_const) {
    CHECK(validates(R"({"const":42})", "42"));
    CHECK(validates(R"({"const":42})", "42.0")); // numeric equality
    CHECK(!validates(R"({"const":42})", "43"));
    CHECK(validates(R"({"const":{"a":[1,2]}})", R"({"a":[1,2]})")); // deep
}

TEST(schema_enum) {
    CHECK(validates(R"({"enum":["red","green","blue"]})", R"("green")"));
    CHECK(!validates(R"({"enum":["red","green","blue"]})", R"("purple")"));
    CHECK(validates(R"({"enum":[1,2,3]})", "2"));
}

//===----------------------------------------------------------------------===//
// logical combinators
//===----------------------------------------------------------------------===//
TEST(schema_allof) {
    const char* schema = R"({"allOf":[{"type":"integer"},{"minimum":10}]})";
    CHECK(validates(schema, "15"));
    CHECK(!validates(schema, "5"));      // fails minimum
    CHECK(!validates(schema, R"("x")")); // fails type
}

TEST(schema_anyof) {
    const char* schema = R"({"anyOf":[{"type":"string"},{"type":"integer"}]})";
    CHECK(validates(schema, R"("x")"));
    CHECK(validates(schema, "5"));
    CHECK(!validates(schema, "true"));
}

TEST(schema_oneof) {
    // Exactly one branch must match.
    const char* schema = R"({"oneOf":[{"type":"integer"},{"minimum":100}]})";
    CHECK(validates(schema, "5"));     // integer only (5 < 100)
    CHECK(!validates(schema, "150"));  // matches both integer and minimum -> fails
    CHECK(validates(schema, "150.5")); // matches only minimum
}

TEST(schema_not) {
    CHECK(validates(R"({"not":{"type":"string"}})", "5"));
    CHECK(!validates(R"({"not":{"type":"string"}})", R"("x")"));
}

TEST(schema_boolean_schemas) {
    CHECK(validates("true", R"({"anything":1})"));
    CHECK(!validates("false", "1"));
}

//===----------------------------------------------------------------------===//
// collect-all: multiple independent failures reported together
//===----------------------------------------------------------------------===//
TEST(schema_collects_all_failures) {
    const char* schema =
        R"({"type":"object",
            "required":["name","age","email"],
            "properties":{
              "age":{"type":"integer","minimum":0},
              "name":{"type":"string"}}})";
    // age is a negative string (2 problems), name is a number (1), email missing (1).
    std::vector<pjson::SchemaError> errors;
    CHECK(!validates(schema, R"({"age":"x","name":5})", errors));
    // Expect: missing email, /age type, /name type. (age minimum can't run on a
    // non-number.) At least three distinct failures collected.
    CHECK(errors.size() >= size_t(3));
}

TEST(schema_valid_document_has_no_errors) {
    const char* schema =
        R"({"type":"object",
            "required":["name","age"],
            "properties":{
              "name":{"type":"string","minLength":1},
              "age":{"type":"integer","minimum":0},
              "tags":{"type":"array","items":{"type":"string"}}},
            "additionalProperties":false})";
    std::vector<pjson::SchemaError> errors;
    CHECK(validates(schema, R"({"name":"Ada","age":36,"tags":["x","y"]})", errors));
    CHECK_EQ(errors.size(), size_t(0));
}

//===----------------------------------------------------------------------===//
// validate() built with the programmatic API (schema is a pjson object)
//===----------------------------------------------------------------------===//
TEST(schema_built_programmatically) {
    pjson schema;
    schema["type"] = "object";
    schema["required"][0] = "name";
    schema["required"][1] = "age";
    schema["properties"]["name"]["type"] = "string";
    schema["properties"]["age"]["type"] = "integer";
    schema["properties"]["age"]["minimum"] = int64_t(0);

    pjson::unique_ptr data = parseJson(R"({"name":"Ada","age":36})");
    CHECK(data->validate(schema));

    pjson::unique_ptr bad = parseJson(R"({"name":"Ada","age":-1})");
    std::vector<pjson::SchemaError> errors;
    CHECK(!bad->validate(schema, errors));
    CHECK_EQ(errors.size(), size_t(1));
    CHECK_EQ(errors[0].path, std::string("/age"));
}

//===----------------------------------------------------------------------===//
// JSON-Pointer escaping for keys containing '/' or '~'
//===----------------------------------------------------------------------===//
TEST(schema_pointer_escaping) {
    const char* schema = R"({"type":"object","properties":{"a/b":{"type":"integer"}}})";
    std::vector<pjson::SchemaError> errors;
    CHECK(!validates(schema, R"({"a/b":"x"})", errors));
    CHECK_EQ(errors.size(), size_t(1));
    CHECK_EQ(errors[0].path, std::string("/a~1b")); // '/' escaped as ~1
}

TEST(schema_const_exact_mixed_numeric_equality_beyond_2pow53) {
    pjson schema;
    schema["const"] = int64_t(9007199254740993LL);

    pjson exact;
    exact = int64_t(9007199254740993LL);
    CHECK(exact.validate(schema));

    pjson rounded;
    rounded = double(9007199254740992.0);
    CHECK(!rounded.validate(schema));
}

TEST(schema_enum_exact_mixed_numeric_equality_beyond_2pow53) {
    pjson schema;
    schema["enum"][0] = int64_t(9007199254740993LL);
    schema["enum"][1] = int64_t(7);

    pjson exact;
    exact = int64_t(9007199254740993LL);
    CHECK(exact.validate(schema));

    pjson rounded;
    rounded = double(9007199254740992.0);
    CHECK(!rounded.validate(schema));
}

TEST(schema_unique_items_exact_mixed_numeric_equality_beyond_2pow53) {
    pjson schema;
    schema["uniqueItems"] = true;

    pjson distinct;
    distinct[0] = int64_t(9007199254740993LL);
    distinct[1] = double(9007199254740992.0);
    CHECK(distinct.validate(schema));

    pjson duplicate;
    duplicate[0] = int64_t(9007199254740992LL);
    duplicate[1] = double(9007199254740992.0);
    CHECK(!duplicate.validate(schema));
}

TEST(schema_exact_numeric_bounds_beyond_2pow53) {
    pjson minimumSchema;
    minimumSchema["minimum"] = int64_t(9007199254740993LL);

    pjson below;
    below = int64_t(9007199254740992LL);
    CHECK(!below.validate(minimumSchema));
    pjson belowDouble;
    belowDouble = double(9007199254740992.0);
    CHECK(!belowDouble.validate(minimumSchema));

    pjson at;
    at = int64_t(9007199254740993LL);
    CHECK(at.validate(minimumSchema));

    pjson exclusiveMaximumSchema;
    exclusiveMaximumSchema["exclusiveMaximum"] = int64_t(9007199254740993LL);
    CHECK(at.validate(minimumSchema));
    CHECK(!at.validate(exclusiveMaximumSchema));

    pjson maximumDoubleSchema;
    maximumDoubleSchema["maximum"] = double(9007199254740992.0);
    CHECK(!at.validate(maximumDoubleSchema));
}

TEST(schema_length_counts_unicode_code_points) {
    CHECK(validates(R"({"minLength":1,"maxLength":1})", "\"\xC3\xA9\""));
    CHECK(validates(R"({"minLength":1,"maxLength":1})", "\"\xF0\x9F\x98\x80\""));
    CHECK(validates(R"({"minLength":2,"maxLength":2})", "\"\xC3\xA9\xE2\x82\xAC\""));
    CHECK(!validates(R"({"maxLength":1})", "\"\xC3\xA9\xE2\x82\xAC\""));
    CHECK(!validates(R"({"maxLength":1})", "\"e\xCC\x81\""));
}

TEST(schema_integral_keyword_shapes) {
    CHECK(validates(R"({"minLength":1.5})", R"("")"));
    CHECK(validates(R"({"maxLength":2.5})", R"("abcd")"));
    CHECK(validates(R"({"minItems":1.5})", "[]"));
    CHECK(validates(R"({"maxItems":0.5})", "[1,2,3]"));
    CHECK(validates(R"({"minProperties":1.25})", R"({})"));
    CHECK(validates(R"({"maxProperties":0.5})", R"({"a":1})"));
    CHECK(validates(R"({"minLength":-1})", R"("")"));
    CHECK(validates(R"({"maxItems":"1"})", "[1,2]"));

    CHECK(!validates(R"({"minLength":2.0})", R"("a")"));
    CHECK(!validates(R"({"maxItems":1.0})", "[1,2]"));
    CHECK(!validates(R"({"maxProperties":0.0})", R"({"a":1})"));
}

TEST(schema_malformed_not_shape_is_ignored) {
    CHECK(validates(R"({"not":5})", "1"));
    CHECK(validates(R"({"not":"schema"})", R"({"value":true})"));
}

TEST(schema_error_constructors_and_collector_append) {
    pjson::SchemaError empty;
    CHECK_EQ(empty.path, std::string());
    CHECK_EQ(empty.message, std::string());

    pjson::SchemaError concrete("/age", "expected integer");
    CHECK_EQ(concrete.path, std::string("/age"));
    CHECK_EQ(concrete.message, std::string("expected integer"));

    std::vector<pjson::SchemaError> errors;
    errors.push_back(pjson::SchemaError("/seed", "existing"));
    CHECK(!validates(R"({"type":"object","required":["name"]})", R"({})", errors));
    CHECK_EQ(errors[0].path, std::string("/seed"));
    CHECK_EQ(errors[0].message, std::string("existing"));
    CHECK(errors.size() >= size_t(2));
    CHECK(hasMessageContaining(errors, "missing required property"));
}
