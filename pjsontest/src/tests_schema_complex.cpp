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
#include <algorithm>
#include <string>
#include <vector>

using namespace ByteDance;

namespace {

    pjson::unique_ptr parseJson(const char* text) {
        return pjson::parse(std::string(text));
    }

    // Returns true if some collected error has exactly this path.
    bool hasErrorAt(const std::vector<pjson::SchemaError>& errs, const std::string& path) {
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
    pjson::unique_ptr schema = parseJson(kPersonSchema);
    CHECK(schema != nullptr);
    pjson::unique_ptr data = parseJson(R"({
        "id": 42,
        "name": "Ada Lovelace",
        "email": "ada@example.com",
        "age": 36,
        "roles": ["admin", "user"],
        "address": { "city": "London", "zip": "12345" }
    })");
    CHECK(data != nullptr);
    std::vector<pjson::SchemaError> errors;
    CHECK(data->validate(*schema, errors));
    CHECK_EQ(errors.size(), size_t(0));
}

//===----------------------------------------------------------------------===//
// Every violation across the tree is collected in a single pass.
//===----------------------------------------------------------------------===//
TEST(complex_schema_collects_all_violations) {
    pjson::unique_ptr schema = parseJson(kPersonSchema);
    pjson::unique_ptr data = parseJson(R"({
        "id": 0,
        "name": "",
        "email": "no-at-sign",
        "age": 200,
        "roles": [],
        "address": { "zip": "abc" },
        "extra": true
    })");
    CHECK(data != nullptr);
    std::vector<pjson::SchemaError> errors;
    CHECK(!data->validate(*schema, errors));

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
    pjson::unique_ptr s = parseJson(schema);
    pjson::unique_ptr d = parseJson(R"({ "matrix": [[1,2],[3,"bad"],[5]] })");
    std::vector<pjson::SchemaError> errors;
    CHECK(!d->validate(*s, errors));
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
    pjson::unique_ptr good = parseJson("{\"a\":5}");
    pjson::unique_ptr schemaValue = parseJson(schema);
    CHECK(good->validate(*schemaValue));
    std::vector<pjson::SchemaError> errors;
    pjson::unique_ptr bad = parseJson("{\"a\":\"x\"}");
    CHECK(!bad->validate(*schemaValue, errors));
    CHECK(hasErrorAt(errors, "/a"));
}

TEST(complex_schema_anyof_branches) {
    const char* schema = R"({
        "anyOf": [
            { "required": ["a"] },
            { "required": ["b"] }
        ]
    })";
    pjson::unique_ptr schemaValue = parseJson(schema);
    CHECK(parseJson("{\"a\":1}")->validate(*schemaValue));
    CHECK(parseJson("{\"b\":1}")->validate(*schemaValue));
    CHECK(!parseJson("{\"c\":1}")->validate(*schemaValue));
}

TEST(complex_schema_oneof_exactly_one) {
    // A value that satisfies two branches must FAIL oneOf.
    const char* schema = R"({ "oneOf": [ { "type": "number" }, { "type": "integer" } ] })";
    pjson::unique_ptr schemaValue = parseJson(schema);
    CHECK(parseJson("2.5")->validate(*schemaValue)); // number only
    CHECK(!parseJson("5")->validate(*schemaValue));  // both number and integer
}

TEST(complex_schema_not_nested) {
    const char* schema = R"({ "not": { "required": ["forbidden"] } })";
    pjson::unique_ptr schemaValue = parseJson(schema);
    CHECK(parseJson("{\"ok\":1}")->validate(*schemaValue));
    CHECK(!parseJson("{\"forbidden\":1}")->validate(*schemaValue));
}

TEST(complex_schema_combinator_inside_properties) {
    const char* schema = R"({
        "properties": {
            "val": { "anyOf": [ { "type": "string" }, { "type": "integer" } ] }
        }
    })";
    pjson::unique_ptr schemaValue = parseJson(schema);
    CHECK(parseJson("{\"val\":\"x\"}")->validate(*schemaValue));
    CHECK(parseJson("{\"val\":7}")->validate(*schemaValue));
    std::vector<pjson::SchemaError> errors;
    CHECK(!parseJson("{\"val\":true}")->validate(*schemaValue, errors));
    CHECK(hasErrorAt(errors, "/val"));
}

//===----------------------------------------------------------------------===//
// Boolean sub-schemas.
//===----------------------------------------------------------------------===//
TEST(complex_schema_items_false_rejects_nonempty) {
    pjson::unique_ptr schemaValue = parseJson(R"({"items":false})");
    CHECK(parseJson("[]")->validate(*schemaValue));
    std::vector<pjson::SchemaError> errors;
    CHECK(!parseJson("[1]")->validate(*schemaValue, errors));
    CHECK_EQ(errors[0].path, std::string("/0"));
}

TEST(complex_schema_property_true_false) {
    const char* schema = R"({ "properties": { "yes": true, "no": false } })";
    pjson::unique_ptr schemaValue = parseJson(schema);
    CHECK(parseJson("{\"yes\":123}")->validate(*schemaValue)); // true accepts
    CHECK(!parseJson("{\"no\":1}")->validate(*schemaValue));   // false rejects presence
}

//===----------------------------------------------------------------------===//
// Constraints on the wrong node type are simply skipped (not errors).
//===----------------------------------------------------------------------===//
TEST(complex_schema_irrelevant_constraints_ignored) {
    // minItems on a number, minLength on an array, etc. do not fire.
    CHECK(parseJson("5")->validate(*parseJson(R"({"minItems":3})")));
    CHECK(parseJson("[1]")->validate(*parseJson(R"({"minLength":3})")));
    CHECK(parseJson("\"hi\"")->validate(*parseJson(R"({"minimum":100})")));
    CHECK(parseJson("5")->validate(
        *parseJson(R"({"required":["a"]})"))); // required only checks objects
}

//===----------------------------------------------------------------------===//
// uniqueItems with deep (structural) comparison.
//===----------------------------------------------------------------------===//
TEST(complex_schema_unique_items_deep) {
    pjson::unique_ptr schemaValue = parseJson(R"({"uniqueItems":true})");
    CHECK(parseJson("[[1,2],[1,3]]")->validate(*schemaValue));
    CHECK(!parseJson("[[1,2],[1,2]]")->validate(*schemaValue));
    CHECK(!parseJson(R"([{"a":1},{"a":1}])")->validate(*schemaValue));
    CHECK(parseJson(R"([{"a":1},{"a":2}])")->validate(*schemaValue));
}

//===----------------------------------------------------------------------===//
// enum / const with structured (array / object) values.
//===----------------------------------------------------------------------===//
TEST(complex_schema_enum_structured) {
    const char* schema = R"({ "enum": [ {"a":1}, [1,2,3], "text" ] })";
    pjson::unique_ptr schemaValue = parseJson(schema);
    CHECK(parseJson(R"({"a":1})")->validate(*schemaValue));
    CHECK(parseJson("[1,2,3]")->validate(*schemaValue));
    CHECK(parseJson("\"text\"")->validate(*schemaValue));
    CHECK(!parseJson("[1,2]")->validate(*schemaValue));
    CHECK(!parseJson(R"({"a":2})")->validate(*schemaValue));
}

TEST(complex_schema_const_structured) {
    const char* schema = R"({ "const": { "nested": [1, {"x": true}] } })";
    pjson::unique_ptr schemaValue = parseJson(schema);
    CHECK(parseJson(R"({"nested":[1,{"x":true}]})")->validate(*schemaValue));
    CHECK(!parseJson(R"({"nested":[1,{"x":false}]})")->validate(*schemaValue));
}

//===----------------------------------------------------------------------===//
// multipleOf with fractional divisors.
//===----------------------------------------------------------------------===//
TEST(complex_schema_multiple_of_fractions) {
    CHECK(parseJson("0.3")->validate(*parseJson(R"({"multipleOf":0.1})")));
    CHECK(parseJson("15")->validate(*parseJson(R"({"multipleOf":5})")));
    CHECK(!parseJson("14")->validate(*parseJson(R"({"multipleOf":5})")));
    // Divisor of zero is guarded (treated as no constraint).
    CHECK(parseJson("5")->validate(*parseJson(R"({"multipleOf":0})")));
}

//===----------------------------------------------------------------------===//
// The empty schema and unknown keywords accept everything.
//===----------------------------------------------------------------------===//
TEST(complex_schema_empty_and_unknown) {
    CHECK(parseJson("5")->validate(*parseJson("{}")));
    CHECK(parseJson("[1,2,3]")->validate(*parseJson("{}")));
    CHECK(parseJson(R"({"a":1})")
              ->validate(*parseJson(R"({"title":"ignored","description":"also ignored"})")));
    // A schema-valued additionalProperties constraint applies to every key
    // not matched by properties or patternProperties.
    CHECK(!parseJson(R"({"x":"str"})")
               ->validate(*parseJson(R"({"additionalProperties":{"type":"integer"}})")));
    CHECK(parseJson(R"({"x":7})")
              ->validate(*parseJson(R"({"additionalProperties":{"type":"integer"}})")));
}

//===----------------------------------------------------------------------===//
// A schema built programmatically behaves identically to a parsed one.
//===----------------------------------------------------------------------===//
TEST(complex_schema_built_vs_parsed_equivalent) {
    pjson::unique_ptr parsed = parseJson(kPersonSchema);

    // Validate the same doc against both and compare pass/fail + error count.
    pjson::unique_ptr data = parseJson(R"({ "id": 1, "name": "X", "email": "x@y" })");
    std::vector<pjson::SchemaError> e1;
    bool ok1 = data->validate(*parsed, e1);
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
    pjson::unique_ptr schemaValue = parseJson(schema);
    CHECK(parseJson(R"({"id":5})")->validate(*schemaValue));
    CHECK(parseJson(R"({"id":"abc"})")->validate(*schemaValue));
    std::vector<pjson::SchemaError> errors;
    CHECK(!parseJson(R"({"id":true})")->validate(*schemaValue, errors));
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
    pjson::unique_ptr schema = parseJson(R"({ "type": "array", "items": { "type": "integer" } })");
    std::vector<pjson::SchemaError> errors;
    CHECK(!data.validate(*schema, errors));
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
    CHECK(dataDistinct.validate(schema));

    pjson dataEqual;
    dataEqual[0] = int64_t(9007199254740992LL);
    dataEqual[1] = double(9007199254740992.0);
    CHECK(!dataEqual.validate(schema));
}
