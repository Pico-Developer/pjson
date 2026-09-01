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
// PJSON-SCHEMA-000..003: strict fail-closed subset mode plus the Draft 2020-12
// applicator keywords added to the validator (if/then/else, prefixItems,
// contains/minContains/maxContains, dependentSchemas).
//
#include "pjson.h"
#include "test_harness.h"
#include "test_util.h"

#include <string>
#include <vector>

using namespace ByteDance;

namespace {

    bool validates(const char* schemaText, const char* dataText,
                   const pjson_test::SchemaOptions& opts = pjson_test::SchemaOptions()) {
        pjson_test::Parsed schema = pjson_test::parse(schemaText);
        pjson_test::Parsed data = pjson_test::parse(dataText);
        if (!schema || !data)
            return false;
        return pjson_test::schemaValidate(*data, *schema, opts);
    }

} // namespace

//===----------------------------------------------------------------------===//
// if/then/else selects the correct branch (A.5 regression: previously ignored).
//===----------------------------------------------------------------------===//
TEST(schema_if_then_else) {
    const char* schema =
        "{\"if\":{\"properties\":{\"kind\":{\"const\":\"a\"}},\"required\":[\"kind\"]},"
        "\"then\":{\"required\":[\"a_field\"]},"
        "\"else\":{\"required\":[\"other_field\"]}}";

    // kind == "a" but missing a_field -> then-branch fails.
    CHECK(!validates(schema, "{\"kind\":\"a\"}"));
    // kind == "a" with a_field -> passes.
    CHECK(validates(schema, "{\"kind\":\"a\",\"a_field\":1}"));
    // kind != "a" -> else-branch requires other_field.
    CHECK(!validates(schema, "{\"kind\":\"b\"}"));
    CHECK(validates(schema, "{\"kind\":\"b\",\"other_field\":1}"));
}

//===----------------------------------------------------------------------===//
// prefixItems constrains leading positions; items constrains the rest.
//===----------------------------------------------------------------------===//
TEST(schema_prefix_items_and_items) {
    const char* schema = "{\"prefixItems\":[{\"type\":\"string\"},{\"type\":\"integer\"}],"
                         "\"items\":{\"type\":\"boolean\"}}";

    CHECK(validates(schema, "[\"x\", 1]"));
    CHECK(validates(schema, "[\"x\", 1, true, false]"));
    // Third element must be boolean.
    CHECK(!validates(schema, "[\"x\", 1, 2]"));
    // First element must be a string.
    CHECK(!validates(schema, "[3, 1]"));
}

//===----------------------------------------------------------------------===//
// contains / minContains / maxContains.
//===----------------------------------------------------------------------===//
TEST(schema_contains_bounds) {
    const char* schema =
        "{\"contains\":{\"type\":\"integer\"},\"minContains\":2,\"maxContains\":3}";

    CHECK(!validates(schema, "[\"a\", \"b\"]")); // 0 integers, below min
    CHECK(!validates(schema, "[1, \"a\"]"));     // 1 integer, below min
    CHECK(validates(schema, "[1, 2, \"a\"]"));   // 2 integers, ok
    CHECK(validates(schema, "[1, 2, 3]"));       // 3 integers, ok
    CHECK(!validates(schema, "[1, 2, 3, 4]"));   // 4 integers, above max
}

//===----------------------------------------------------------------------===//
// dependentSchemas applies a subschema when a triggering property is present.
//===----------------------------------------------------------------------===//
TEST(schema_dependent_schemas) {
    const char* schema = "{\"dependentSchemas\":{\"card\":{\"required\":[\"billing_address\"]}}}";

    CHECK(validates(schema, "{}")); // no trigger
    CHECK(validates(schema, "{\"card\":1,\"billing_address\":\"x\"}"));
    CHECK(!validates(schema, "{\"card\":1}")); // trigger without dependency
}

//===----------------------------------------------------------------------===//
// PJSON-SCHEMA-000: strict subset mode fails closed on an unsupported standard
// keyword, while permissive (default) mode ignores it.
//===----------------------------------------------------------------------===//
TEST(schema_strict_mode_fails_on_unsupported_standard_keyword) {
    // unevaluatedProperties is a standard 2020-12 keyword pjson does not enforce.
    const char* schema = "{\"type\":\"object\",\"unevaluatedProperties\":false}";
    const char* data = "{\"extra\":1}";

    // Permissive default: the unsupported keyword is ignored, so this passes.
    CHECK(validates(schema, data));

    // Strict: the unsupported standard keyword makes validation fail closed.
    pjson_test::SchemaOptions strict = pjson_test::SchemaOptions::strict();
    CHECK(!validates(schema, data, strict));
}

//===----------------------------------------------------------------------===//
// Strict mode still permits unknown non-standard extension keywords.
//===----------------------------------------------------------------------===//
TEST(schema_strict_mode_allows_extension_keywords) {
    const char* schema = "{\"type\":\"string\",\"x-vendor-hint\":\"anything\"}";
    pjson_test::SchemaOptions strict = pjson_test::SchemaOptions::strict();
    CHECK(validates(schema, "\"hello\"", strict));
    CHECK(!validates(schema, "42", strict)); // the supported keyword still applies
}
