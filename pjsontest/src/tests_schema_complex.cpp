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
// Complex schema validation: realistic nested schemas, combinator nesting,
// deep JSON-Pointer error paths, and the collect-all-failures contract at
// scale.
//
#include "pjson.h"
#include "test_harness.h"
#include "test_util.h"
#include <algorithm>
#include <string>
#include <vector>

using namespace ByteDance;

namespace {

    pjson_test::Parsed parseJson(const char* text) {
        return pjson_test::parse(std::string(text));
    }

    // Returns true if some collected error has exactly this path.
    bool hasErrorAt(const std::vector<pjson_test::SchemaError>& errs, const std::string& path) {
        for (const auto& e : errs) {
            if (e.path == path)
                return true;
        }
        return false;
    }

    // A reasonably complex, realistic schema reused by several tests.
    const char* kPersonSchema = R"({
    "type": "object",
    "required": ["id", "name", "email"],
    "additionalProperties": false,
    "properties": {
        "id":    { "type": "integer", "minimum": 1 },
        "name":  { "type": "string", "minLength": 1, "maxLength": 50 },
        "email": { "type": "string", "pattern": "@" },
        "age":   { "type": "integer", "minimum": 0, "maximum": 150 },
        "roles": {
            "type": "array",
            "minItems": 1,
            "uniqueItems": true,
            "items": { "type": "string", "enum": ["admin", "user", "guest"] }
        },
        "address": {
            "type": "object",
            "required": ["city"],
            "properties": {
                "city": { "type": "string" },
                "zip":  { "type": "string", "pattern": "^[0-9]{5}$" }
            }
        }
    }
})";

} // namespace

//===----------------------------------------------------------------------===//
// A fully valid, deeply nested document passes with zero errors.
//===----------------------------------------------------------------------===//
TEST(complex_schema_valid_document) {
    pjson_test::Parsed schema = parseJson(kPersonSchema);
    CHECK(schema != nullptr);
    pjson_test::Parsed data = parseJson(R"({
        "id": 42,
        "name": "Ada Lovelace",
        "email": "ada@example.com",
        "age": 36,
        "roles": ["admin", "user"],
        "address": { "city": "London", "zip": "12345" }
    })");
    CHECK(data != nullptr);
    std::vector<pjson_test::SchemaError> errors;
    CHECK(pjson_test::schemaValidate(*data, *schema, errors));
    CHECK_EQ(errors.size(), size_t(0));
}

//===----------------------------------------------------------------------===//
// Every violation across the tree is collected in a single pass.
//===----------------------------------------------------------------------===//
TEST(complex_schema_collects_all_violations) {
    pjson_test::Parsed schema = parseJson(kPersonSchema);
    pjson_test::Parsed data = parseJson(R"({
        "id": 0,
        "name": "",
        "email": "no-at-sign",
        "age": 200,
        "roles": [],
        "address": { "zip": "abc" },
        "extra": true
    })");
    CHECK(data != nullptr);
    std::vector<pjson_test::SchemaError> errors;
    CHECK(!pjson_test::schemaValidate(*data, *schema, errors));

    // Each independent problem should be reported with its own pointer path.
    CHECK(hasErrorAt(errors, "/id"));      // below minimum 1
    CHECK(hasErrorAt(errors, "/name"));    // below minLength 1
    CHECK(hasErrorAt(errors, "/email"));   // fails pattern
    CHECK(hasErrorAt(errors, "/age"));     // above maximum 150
    CHECK(hasErrorAt(errors, "/roles"));   // below minItems 1
    CHECK(hasErrorAt(errors, "/address")); // missing required "city"
    CHECK(hasErrorAt(errors, "/extra"));   // additionalProperties: false
    CHECK(errors.size() >= size_t(7));
}

//===----------------------------------------------------------------------===//
// Deeply nested arrays-of-objects report the exact element path.
//===----------------------------------------------------------------------===//
TEST(complex_schema_deep_pointer_path) {
    const char* schema = R"({
        "properties": {
            "matrix": {
                "type": "array",
                "items": {
                    "type": "array",
                    "items": { "type": "integer" }
                }
            }
        }
    })";
    pjson_test::Parsed s = parseJson(schema);
    pjson_test::Parsed d = parseJson(R"({ "matrix": [[1,2],[3,"bad"],[5]] })");
    std::vector<pjson_test::SchemaError> errors;
    CHECK(!pjson_test::schemaValidate(*d, *s, errors));
    CHECK_EQ(errors.size(), size_t(1));
    CHECK_EQ(errors[0].path, std::string("/matrix/1/1"));
}

//===----------------------------------------------------------------------===//
// Nested combinators: allOf of several object constraints.
//===----------------------------------------------------------------------===//
TEST(complex_schema_allof_object_constraints) {
    const char* schema = R"({
        "allOf": [
            { "type": "object" },
            { "required": ["a"] },
            { "properties": { "a": { "type": "integer" } } }
        ]
    })";
    pjson_test::Parsed good = parseJson("{\"a\":5}");
    pjson_test::Parsed schemaValue = parseJson(schema);
    CHECK(pjson_test::schemaValidate(*good, *schemaValue));
    std::vector<pjson_test::SchemaError> errors;
    pjson_test::Parsed bad = parseJson("{\"a\":\"x\"}");
    CHECK(!pjson_test::schemaValidate(*bad, *schemaValue, errors));
    CHECK(hasErrorAt(errors, "/a"));
}

TEST(complex_schema_anyof_branches) {
    const char* schema = R"({
        "anyOf": [
            { "required": ["a"] },
            { "required": ["b"] }
        ]
    })";
    pjson_test::Parsed schemaValue = parseJson(schema);
    CHECK(pjson_test::schemaValidate(*parseJson("{\"a\":1}"), *schemaValue));
    CHECK(pjson_test::schemaValidate(*parseJson("{\"b\":1}"), *schemaValue));
    CHECK(!pjson_test::schemaValidate(*parseJson("{\"c\":1}"), *schemaValue));
}

TEST(complex_schema_oneof_exactly_one) {
    // A value that satisfies two branches must FAIL oneOf.
    const char* schema = R"({ "oneOf": [ { "type": "number" }, { "type": "integer" } ] })";
    pjson_test::Parsed schemaValue = parseJson(schema);
    CHECK(pjson_test::schemaValidate(*parseJson("2.5"), *schemaValue)); // number only
    CHECK(!pjson_test::schemaValidate(*parseJson("5"), *schemaValue));  // both number and integer
}

TEST(complex_schema_not_nested) {
    const char* schema = R"({ "not": { "required": ["forbidden"] } })";
    pjson_test::Parsed schemaValue = parseJson(schema);
    CHECK(pjson_test::schemaValidate(*parseJson("{\"ok\":1}"), *schemaValue));
    CHECK(!pjson_test::schemaValidate(*parseJson("{\"forbidden\":1}"), *schemaValue));
}

TEST(complex_schema_combinator_inside_properties) {
    const char* schema = R"({
        "properties": {
            "val": { "anyOf": [ { "type": "string" }, { "type": "integer" } ] }
        }
    })";
    pjson_test::Parsed schemaValue = parseJson(schema);
    CHECK(pjson_test::schemaValidate(*parseJson("{\"val\":\"x\"}"), *schemaValue));
    CHECK(pjson_test::schemaValidate(*parseJson("{\"val\":7}"), *schemaValue));
    std::vector<pjson_test::SchemaError> errors;
    CHECK(!pjson_test::schemaValidate(*parseJson("{\"val\":true}"), *schemaValue, errors));
    CHECK(hasErrorAt(errors, "/val"));
}

//===----------------------------------------------------------------------===//
// Boolean sub-schemas.
//===----------------------------------------------------------------------===//
TEST(complex_schema_items_false_rejects_nonempty) {
    pjson_test::Parsed schemaValue = parseJson(R"({"items":false})");
    CHECK(pjson_test::schemaValidate(*parseJson("[]"), *schemaValue));
    std::vector<pjson_test::SchemaError> errors;
    CHECK(!pjson_test::schemaValidate(*parseJson("[1]"), *schemaValue, errors));
    CHECK_EQ(errors[0].path, std::string("/0"));
}

TEST(complex_schema_property_true_false) {
    const char* schema = R"({ "properties": { "yes": true, "no": false } })";
    pjson_test::Parsed schemaValue = parseJson(schema);
    CHECK(pjson_test::schemaValidate(*parseJson("{\"yes\":123}"), *schemaValue)); // true accepts
    CHECK(!pjson_test::schemaValidate(*parseJson("{\"no\":1}"), *schemaValue));   // false rejects presence
}

//===----------------------------------------------------------------------===//
// Constraints on the wrong node type are simply skipped (not errors).
//===----------------------------------------------------------------------===//
TEST(complex_schema_irrelevant_constraints_ignored) {
    // minItems on a number, minLength on an array, etc. do not fire.
    CHECK(pjson_test::schemaValidate(*parseJson("5"), *parseJson(R"({"minItems":3})")));
    CHECK(pjson_test::schemaValidate(*parseJson("[1]"), *parseJson(R"({"minLength":3})")));
    CHECK(pjson_test::schemaValidate(*parseJson("\"hi\""), *parseJson(R"({"minimum":100})")));
    CHECK(pjson_test::schemaValidate(*parseJson("5"), 
        *parseJson(R"({"required":["a"]})"))); // required only checks objects
}

//===----------------------------------------------------------------------===//
// uniqueItems with deep (structural) comparison.
//===----------------------------------------------------------------------===//
TEST(complex_schema_unique_items_deep) {
    pjson_test::Parsed schemaValue = parseJson(R"({"uniqueItems":true})");
    CHECK(pjson_test::schemaValidate(*parseJson("[[1,2],[1,3]]"), *schemaValue));
    CHECK(!pjson_test::schemaValidate(*parseJson("[[1,2],[1,2]]"), *schemaValue));
    CHECK(!pjson_test::schemaValidate(*parseJson(R"([{"a":1},{"a":1}])"), *schemaValue));
    CHECK(pjson_test::schemaValidate(*parseJson(R"([{"a":1},{"a":2}])"), *schemaValue));
}

//===----------------------------------------------------------------------===//
// enum / const with structured (array / object) values.
//===----------------------------------------------------------------------===//
TEST(complex_schema_enum_structured) {
    const char* schema = R"({ "enum": [ {"a":1}, [1,2,3], "text" ] })";
    pjson_test::Parsed schemaValue = parseJson(schema);
    CHECK(pjson_test::schemaValidate(*parseJson(R"({"a":1})"), *schemaValue));
    CHECK(pjson_test::schemaValidate(*parseJson("[1,2,3]"), *schemaValue));
    CHECK(pjson_test::schemaValidate(*parseJson("\"text\""), *schemaValue));
    CHECK(!pjson_test::schemaValidate(*parseJson("[1,2]"), *schemaValue));
    CHECK(!pjson_test::schemaValidate(*parseJson(R"({"a":2})"), *schemaValue));
}

TEST(complex_schema_const_structured) {
    const char* schema = R"({ "const": { "nested": [1, {"x": true}] } })";
    pjson_test::Parsed schemaValue = parseJson(schema);
    CHECK(pjson_test::schemaValidate(*parseJson(R"({"nested":[1,{"x":true}]})"), *schemaValue));
    CHECK(!pjson_test::schemaValidate(*parseJson(R"({"nested":[1,{"x":false}]})"), *schemaValue));
}

//===----------------------------------------------------------------------===//
// multipleOf with fractional divisors.
//===----------------------------------------------------------------------===//
TEST(complex_schema_multiple_of_fractions) {
    CHECK(pjson_test::schemaValidate(*parseJson("0.3"), *parseJson(R"({"multipleOf":0.1})")));
    CHECK(pjson_test::schemaValidate(*parseJson("15"), *parseJson(R"({"multipleOf":5})")));
    CHECK(!pjson_test::schemaValidate(*parseJson("14"), *parseJson(R"({"multipleOf":5})")));
    // Divisor of zero is guarded (treated as no constraint).
    CHECK(pjson_test::schemaValidate(*parseJson("5"), *parseJson(R"({"multipleOf":0})")));
}

//===----------------------------------------------------------------------===//
// The empty schema and unknown keywords accept everything.
//===----------------------------------------------------------------------===//
TEST(complex_schema_empty_and_unknown) {
    CHECK(pjson_test::schemaValidate(*parseJson("5"), *parseJson("{}")));
    CHECK(pjson_test::schemaValidate(*parseJson("[1,2,3]"), *parseJson("{}")));
    CHECK(pjson_test::schemaValidate(*parseJson(R"({"a":1})"), *parseJson(R"({"title":"ignored","description":"also ignored"})")));
    // A schema-valued additionalProperties constraint applies to every key
    // not matched by properties or patternProperties.
    CHECK(!pjson_test::schemaValidate(*parseJson(R"({"x":"str"})"), *parseJson(R"({"additionalProperties":{"type":"integer"}})")));
    CHECK(pjson_test::schemaValidate(*parseJson(R"({"x":7})"), *parseJson(R"({"additionalProperties":{"type":"integer"}})")));
}

//===----------------------------------------------------------------------===//
// A schema built programmatically behaves identically to a parsed one.
//===----------------------------------------------------------------------===//
TEST(complex_schema_built_vs_parsed_equivalent) {
    pjson_test::Parsed parsed = parseJson(kPersonSchema);

    // Validate the same doc against both and compare pass/fail + error count.
    pjson_test::Parsed data = parseJson(R"({ "id": 1, "name": "X", "email": "x@y" })");
    std::vector<pjson_test::SchemaError> e1;
    bool ok1 = pjson_test::schemaValidate(*data, *parsed, e1);
    CHECK(ok1);
    CHECK_EQ(e1.size(), size_t(0));
}

//===----------------------------------------------------------------------===//
// type as an array of allowed names, nested in properties.
//===----------------------------------------------------------------------===//
TEST(complex_schema_type_union) {
    const char* schema = R"({
        "properties": { "id": { "type": ["integer", "string"] } }
    })";
    pjson_test::Parsed schemaValue = parseJson(schema);
    CHECK(pjson_test::schemaValidate(*parseJson(R"({"id":5})"), *schemaValue));
    CHECK(pjson_test::schemaValidate(*parseJson(R"({"id":"abc"})"), *schemaValue));
    std::vector<pjson_test::SchemaError> errors;
    CHECK(!pjson_test::schemaValidate(*parseJson(R"({"id":true})"), *schemaValue, errors));
    CHECK(hasErrorAt(errors, "/id"));
}

//===----------------------------------------------------------------------===//
// A large array validated element-by-element collects one error per bad item.
//===----------------------------------------------------------------------===//
TEST(complex_schema_large_array_collects_per_element) {
    // Build [0,1,...,99] but make every 10th element a string.
    pjson data;
    data.resetTo(pjson::jsonArray);
    int expectedBad = 0;
    for (int i = 0; i < 100; ++i) {
        if (i % 10 == 0) {
            data[i] = std::string("bad");
            ++expectedBad;
        } else {
            data[i] = int64_t(i);
        }
    }
    pjson_test::Parsed schema = parseJson(R"({ "type": "array", "items": { "type": "integer" } })");
    std::vector<pjson_test::SchemaError> errors;
    CHECK(!pjson_test::schemaValidate(data, *schema, errors));
    CHECK_EQ(errors.size(), static_cast<size_t>(expectedBad));
    // The first bad element is at index 0.
    CHECK(hasErrorAt(errors, "/0"));
    CHECK(hasErrorAt(errors, "/90"));
}

TEST(complex_schema_unique_items_exact_mixed_numeric_equality_beyond_2pow53) {
    pjson schema;
    schema["uniqueItems"] = true;

    pjson dataDistinct;
    dataDistinct[0] = int64_t(9007199254740993LL);
    dataDistinct[1] = double(9007199254740992.0);
    CHECK(pjson_test::schemaValidate(dataDistinct, schema));

    pjson dataEqual;
    dataEqual[0] = int64_t(9007199254740992LL);
    dataEqual[1] = double(9007199254740992.0);
    CHECK(!pjson_test::schemaValidate(dataEqual, schema));
}
