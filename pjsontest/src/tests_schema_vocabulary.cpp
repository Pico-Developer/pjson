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
// JSON Schema vocabulary, exact multipleOf arithmetic, validation budgets,
// local references, and optional format-validation tests.
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

    // Parse-and-validate adapters keep the vocabulary tables focused on schema behavior.
    bool validates(const char* schemaText, const char* dataText,
                   std::vector<pjson::SchemaError>& errors,
                   const pjson::SchemaOptions& opts = pjson::SchemaOptions()) {
        pjson::unique_ptr schema = parseJson(schemaText);
        pjson::unique_ptr data = parseJson(dataText);
        if (!schema || !data)
            return false;
        return data->validate(*schema, errors, opts);
    }

    bool validates(const char* schemaText, const char* dataText,
                   const pjson::SchemaOptions& opts = pjson::SchemaOptions()) {
        std::vector<pjson::SchemaError> errors;
        return validates(schemaText, dataText, errors, opts);
    }

    // Error predicates assert semantic diagnostics without coupling tests to error ordering.
    bool hasErrorAt(const std::vector<pjson::SchemaError>& errors, const std::string& path) {
        for (const auto& err : errors) {
            if (err.path == path)
                return true;
        }
        return false;
    }

    bool hasMessageContaining(const std::vector<pjson::SchemaError>& errors,
                              const std::string& needle) {
        for (const auto& err : errors) {
            if (err.message.find(needle) != std::string::npos)
                return true;
        }
        return false;
    }

    bool hasErrorAtWithMessage(const std::vector<pjson::SchemaError>& errors,
                               const std::string& path, const std::string& needle) {
        for (const auto& err : errors) {
            if (err.path == path && err.message.find(needle) != std::string::npos)
                return true;
        }
        return false;
    }

    // Small option factories make each validation-budget test state only its changed knob.
    pjson::SchemaOptions optionsWithFormatValidation(bool enabled) {
        pjson::SchemaOptions opts;
        opts.validateFormats = enabled;
        return opts;
    }

    pjson::SchemaOptions optionsWithDepthBudget(size_t maxDepth) {
        pjson::SchemaOptions opts;
        opts.maxValidationDepth = maxDepth;
        return opts;
    }

    pjson::SchemaOptions optionsWithRefBudget(size_t maxRefs) {
        pjson::SchemaOptions opts;
        opts.maxRefResolutions = maxRefs;
        return opts;
    }

    pjson::SchemaOptions optionsWithWorkBudget(size_t maxWork) {
        pjson::SchemaOptions opts;
        opts.maxValidationWork = maxWork;
        return opts;
    }

    pjson::SchemaOptions optionsWithErrorBudget(size_t maxErrors) {
        pjson::SchemaOptions opts;
        opts.maxErrors = maxErrors;
        return opts;
    }

    // Recursive local-reference fixture shared by depth and reference-resolution budget tests.
    const char* kRecursiveNodeSchema = R"({
        "$defs": {
            "node": {
                "type": "object",
                "required": ["value", "next"],
                "properties": {
                    "value": { "type": "integer" },
                    "next": {
                        "anyOf": [
                            { "type": "null" },
                            { "$ref": "#/$defs/node" }
                        ]
                    }
                },
                "additionalProperties": false
            }
        },
        "$ref": "#/$defs/node"
    })";

    pjson makeNestedPropertySchema(size_t depth) {
        pjson schema;
        pjson* cursor = &schema;
        for (size_t i = 0; i < depth; ++i) {
            (*cursor)["type"] = "object";
            cursor = &((*cursor)["properties"]["x"]);
        }
        (*cursor)["type"] = "integer";
        return schema;
    }

    pjson makeNestedPropertyInstance(size_t depth) {
        pjson instance;
        pjson* cursor = &instance;
        for (size_t i = 0; i < depth; ++i)
            cursor = &((*cursor)["x"]);
        *cursor = int64_t(1);
        return instance;
    }

    pjson makeReferenceChainSchema(size_t references) {
        pjson schema;
        schema["$ref"] = "#/$defs/s0";
        for (size_t i = 0; i < references; ++i) {
            const std::string name = "s" + std::to_string(i);
            if (i + 1U == references) {
                schema["$defs"][name] = true;
            } else {
                schema["$defs"][name]["$ref"] = "#/$defs/s" + std::to_string(i + 1U);
            }
        }
        return schema;
    }

    pjson makeBranchingWorkSchema(size_t levels) {
        pjson schema;
        schema["$ref"] = "#/$defs/level0";
        for (size_t i = 0; i < levels; ++i) {
            const std::string name = "level" + std::to_string(i);
            if (i + 1U == levels) {
                schema["$defs"][name] = true;
            } else {
                const std::string next = "#/$defs/level" + std::to_string(i + 1U);
                schema["$defs"][name]["allOf"][0]["$ref"] = next;
                schema["$defs"][name]["allOf"][1]["$ref"] = next;
            }
        }
        return schema;
    }

    pjson makeDeepValue(size_t depth, int64_t leaf) {
        pjson value;
        pjson* cursor = &value;
        for (size_t i = 0; i < depth; ++i)
            cursor = &((*cursor)["x"]);
        *cursor = leaf;
        return value;
    }

} // namespace

//===----------------------------------------------------------------------===//
// Local references and validation budgets
//===----------------------------------------------------------------------===//

TEST(schema_vocab_ref_local_defs_and_definitions) {
    const char* defsSchema = R"({
        "$defs": {
            "positiveInt": { "type": "integer", "minimum": 1 }
        },
        "type": "object",
        "required": ["id"],
        "properties": {
            "id": { "$ref": "#/$defs/positiveInt" }
        },
        "additionalProperties": false
    })";
    CHECK(validates(defsSchema, R"({"id":7})"));
    std::vector<pjson::SchemaError> errors;
    CHECK(!validates(defsSchema, R"({"id":0})", errors));
    CHECK(hasErrorAtWithMessage(errors, "/id", "minimum"));

    const char* definitionsSchema = R"({
        "definitions": {
            "nonEmptyString": { "type": "string", "minLength": 1 }
        },
        "type": "object",
        "properties": {
            "name": { "$ref": "#/definitions/nonEmptyString" }
        }
    })";
    CHECK(validates(definitionsSchema, R"({"name":"Ada"})"));
    errors.clear();
    CHECK(!validates(definitionsSchema, R"({"name":""})", errors));
    CHECK(hasErrorAtWithMessage(errors, "/name", "minLength"));
}

TEST(schema_vocab_ref_unresolved_malformed_and_nonlocal) {
    std::vector<pjson::SchemaError> errors;

    CHECK(!validates(R"({"$ref":"#/$defs/missing"})", "1", errors));
    CHECK(hasErrorAt(errors, std::string("")));
    CHECK(hasMessageContaining(errors, "unresolved"));

    errors.clear();
    CHECK(!validates(R"({"$ref":"#/$defs/bad~2token"})", "1", errors));
    CHECK(hasErrorAt(errors, std::string("")));
    CHECK(hasMessageContaining(errors, "malformed"));

    errors.clear();
    CHECK(!validates(R"({"$ref":"https://example.com/schema.json#/$defs/x"})", "1", errors));
    CHECK(hasErrorAt(errors, std::string("")));
    CHECK(hasMessageContaining(errors, "non-local"));
}

TEST(schema_vocab_ref_cycle_is_reported) {
    std::vector<pjson::SchemaError> errors;
    CHECK(!validates(R"({"$ref":"#"})", R"({"anything":1})", errors));
    CHECK(hasErrorAt(errors, std::string("")));
    CHECK(hasMessageContaining(errors, "cycle"));
}

TEST(schema_vocab_ref_validation_depth_budget) {
    const char* data = R"({
        "value": 1,
        "next": {
            "value": 2,
            "next": {
                "value": 3,
                "next": null
            }
        }
    })";

    pjson::SchemaOptions shallow = optionsWithDepthBudget(2);
    std::vector<pjson::SchemaError> errors;
    CHECK(!validates(kRecursiveNodeSchema, data, errors, shallow));
    CHECK(hasMessageContaining(errors, "depth"));
}

TEST(schema_vocab_ref_resolution_budget) {
    const char* data = R"({
        "value": 1,
        "next": {
            "value": 2,
            "next": {
                "value": 3,
                "next": null
            }
        }
    })";

    pjson::SchemaOptions limited = optionsWithRefBudget(2);
    std::vector<pjson::SchemaError> errors;
    CHECK(!validates(kRecursiveNodeSchema, data, errors, limited));
    CHECK(hasMessageContaining(errors, "ref"));
    CHECK(hasMessageContaining(errors, "budget"));
}

TEST(schema_vocab_ref_chain_still_obeys_depth_budget) {
    pjson::SchemaOptions opts = optionsWithDepthBudget(4);
    opts.maxRefResolutions = 16;
    const pjson schema = makeReferenceChainSchema(4);
    pjson instance;
    instance = int64_t(1);

    std::vector<pjson::SchemaError> errors;
    CHECK(!instance.validate(schema, errors, opts));
    CHECK(hasMessageContaining(errors, "depth"));
    CHECK(hasMessageContaining(errors, "budget"));
}

TEST(schema_vocab_ref_zero_depth_uses_hard_ceiling) {
    pjson::SchemaOptions opts = optionsWithDepthBudget(0);
    CHECK_EQ(pjson::SchemaOptions().maxValidationDepth, size_t(64));
    const pjson schema = makeNestedPropertySchema(64);
    const pjson instance = makeNestedPropertyInstance(64);

    std::vector<pjson::SchemaError> errors;
    CHECK(!instance.validate(schema, errors, opts));
    CHECK(hasMessageContaining(errors, "depth"));
}

TEST(schema_vocab_ref_requested_depth_is_clamped_to_hard_ceiling) {
    pjson::SchemaOptions opts = optionsWithDepthBudget(2048);
    const pjson withinLimitSchema = makeNestedPropertySchema(63);
    const pjson withinLimitInstance = makeNestedPropertyInstance(63);
    const pjson schema = makeNestedPropertySchema(64);
    const pjson instance = makeNestedPropertyInstance(64);

    CHECK(withinLimitInstance.validate(withinLimitSchema, opts));
    std::vector<pjson::SchemaError> errors;
    CHECK(!instance.validate(schema, errors, opts));
    CHECK(hasMessageContaining(errors, "depth"));
}

TEST(schema_vocab_ref_zero_resolution_budget_allows_hard_ceiling) {
    pjson::SchemaOptions opts = optionsWithRefBudget(0);
    const pjson schema = makeBranchingWorkSchema(10);
    pjson instance;
    instance = int64_t(1);

    CHECK(instance.validate(schema, opts));
}

TEST(schema_vocab_ref_zero_resolution_budget_uses_hard_ceiling) {
    pjson::SchemaOptions opts = optionsWithRefBudget(0);
    const pjson schema = makeBranchingWorkSchema(11);
    pjson instance;
    instance = int64_t(1);

    std::vector<pjson::SchemaError> errors;
    CHECK(!instance.validate(schema, errors, opts));
    CHECK(hasMessageContaining(errors, "ref"));
    CHECK(hasMessageContaining(errors, "budget"));
}

TEST(schema_vocab_ref_ignores_sibling_keywords_draft07) {
    const char* schema = R"({
        "$defs": {
            "asString": { "type": "string", "minLength": 2 }
        },
        "$ref": "#/$defs/asString",
        "type": "integer",
        "minLength": 99
    })";

    CHECK(validates(schema, R"("ok")"));
    std::vector<pjson::SchemaError> errors;
    CHECK(!validates(schema, "5", errors));
    CHECK(hasMessageContaining(errors, "string"));
    CHECK(!hasMessageContaining(errors, "expected type integer"));
    CHECK(!hasMessageContaining(errors, "99"));
}

//===----------------------------------------------------------------------===//
// Object applicators and dependency keywords
//===----------------------------------------------------------------------===//

TEST(schema_vocab_pattern_properties_basic_matching) {
    const char* schema = R"({
        "type": "object",
        "patternProperties": {
            "^S_[A-Z]+$": { "type": "integer" }
        }
    })";

    CHECK(validates(schema, R"({"S_COUNT":1,"plain":"x"})"));
    std::vector<pjson::SchemaError> errors;
    CHECK(!validates(schema, R"({"S_COUNT":"bad"})", errors));
    CHECK(hasErrorAt(errors, "/S_COUNT"));
}

TEST(schema_vocab_pattern_properties_and_properties_both_apply) {
    const char* schema = R"({
        "type": "object",
        "properties": {
            "S_NAME": { "type": "integer" }
        },
        "patternProperties": {
            "^S_": { "type": "string" }
        }
    })";

    std::vector<pjson::SchemaError> errors;
    CHECK(!validates(schema, R"({"S_NAME":5})", errors));
    CHECK(hasErrorAtWithMessage(errors, "/S_NAME", "string"));
}

TEST(schema_vocab_property_names_reports_property_path) {
    const char* schema = R"({
        "type": "object",
        "propertyNames": {
            "pattern": "^[A-Z_]+$"
        }
    })";

    CHECK(validates(schema, R"({"OK":1,"ALSO_OK":2})"));
    std::vector<pjson::SchemaError> errors;
    CHECK(!validates(schema, R"({"bad/key":1})", errors));
    CHECK(hasErrorAt(errors, "/bad~1key"));
    CHECK(hasMessageContaining(errors, "pattern"));
}

TEST(schema_vocab_dependent_required) {
    const char* schema = R"({
        "type": "object",
        "dependentRequired": {
            "credit_card": ["billing_address", "name"]
        }
    })";

    CHECK(validates(schema, R"({"credit_card":"1234","billing_address":"x","name":"Ada"})"));
    std::vector<pjson::SchemaError> errors;
    CHECK(!validates(schema, R"({"credit_card":"1234"})", errors));
    CHECK(hasErrorAt(errors, std::string("")));
    CHECK(hasMessageContaining(errors, "billing_address"));
    CHECK(hasMessageContaining(errors, "name"));
}

TEST(schema_vocab_dependencies_array_form) {
    const char* schema = R"({
        "type": "object",
        "dependencies": {
            "credit_card": ["billing_address"]
        }
    })";

    CHECK(validates(schema, R"({"credit_card":"1234","billing_address":"x"})"));
    std::vector<pjson::SchemaError> errors;
    CHECK(!validates(schema, R"({"credit_card":"1234"})", errors));
    CHECK(hasErrorAt(errors, std::string("")));
    CHECK(hasMessageContaining(errors, "billing_address"));
}

TEST(schema_vocab_dependencies_schema_form) {
    const char* schema = R"({
        "type": "object",
        "dependencies": {
            "credit_card": {
                "required": ["billing_address"],
                "properties": {
                    "billing_address": { "type": "string", "minLength": 5 }
                }
            }
        }
    })";

    CHECK(validates(schema, R"({"credit_card":"1234","billing_address":"123 Main"})"));

    std::vector<pjson::SchemaError> errors;
    CHECK(!validates(schema, R"({"credit_card":"1234"})", errors));
    CHECK(hasErrorAt(errors, std::string("")));
    CHECK(hasMessageContaining(errors, "billing_address"));

    errors.clear();
    CHECK(!validates(schema, R"({"credit_card":"1234","billing_address":"x"})", errors));
    CHECK(hasErrorAtWithMessage(errors, "/billing_address", "minLength"));
}

TEST(schema_vocab_additional_properties_schema_applies_only_to_unmatched_keys) {
    const char* schema = R"({
        "type": "object",
        "properties": {
            "declared": { "type": "string" }
        },
        "additionalProperties": { "type": "integer" }
    })";

    CHECK(validates(schema, R"({"declared":"ok","extra":2})"));

    std::vector<pjson::SchemaError> errors;
    CHECK(!validates(schema, R"({"declared":"ok","extra":"bad"})", errors));
    CHECK(hasErrorAtWithMessage(errors, "/extra", "integer"));
    CHECK(!hasErrorAt(errors, "/declared"));
}

TEST(schema_vocab_object_keyword_interactions) {
    const char* schema = R"({
        "type": "object",
        "properties": {
            "fixed": { "type": "string" }
        },
        "patternProperties": {
            "^dyn_": { "type": "integer" }
        },
        "additionalProperties": false
    })";

    CHECK(validates(schema, R"({"fixed":"ok","dyn_count":3})"));

    std::vector<pjson::SchemaError> errors;
    CHECK(!validates(schema, R"({"fixed":"ok","dyn_count":"bad","extra":1})", errors));
    CHECK(hasErrorAtWithMessage(errors, "/dyn_count", "integer"));
    CHECK(hasErrorAtWithMessage(errors, "/extra", "additional property"));
    CHECK(!hasErrorAt(errors, "/fixed"));
}

//===----------------------------------------------------------------------===//
// Optional string-format validation
//===----------------------------------------------------------------------===//

TEST(schema_vocab_format_date) {
    const char* schema = R"({"type":"string","format":"date"})";
    CHECK(validates(schema, R"("2025-01-02")"));
    CHECK(!validates(schema, R"("2025-13-02")"));
    CHECK(!validates(schema, R"("2025-1-02")"));
}

TEST(schema_vocab_format_time) {
    const char* schema = R"({"type":"string","format":"time"})";
    CHECK(validates(schema, R"("23:59:59Z")"));
    CHECK(validates(schema, R"("12:34:56+05:30")"));
    CHECK(!validates(schema, R"("24:00:00Z")"));
    CHECK(!validates(schema, R"("12:34:56")"));
}

TEST(schema_vocab_format_date_time) {
    const char* schema = R"({"type":"string","format":"date-time"})";
    CHECK(validates(schema, R"("2025-01-02T03:04:05Z")"));
    CHECK(validates(schema, R"("2025-01-02T03:04:05.123+02:30")"));
    CHECK(!validates(schema, R"("2025-01-02 03:04:05Z")"));
    CHECK(!validates(schema, R"("2025-13-02T03:04:05Z")"));
}

TEST(schema_vocab_format_ipv4) {
    const char* schema = R"({"type":"string","format":"ipv4"})";
    CHECK(validates(schema, R"("192.168.0.1")"));
    CHECK(!validates(schema, R"("256.1.2.3")"));
    CHECK(!validates(schema, R"("1.2.3")"));
}

TEST(schema_vocab_format_ipv6) {
    const char* schema = R"({"type":"string","format":"ipv6"})";
    CHECK(validates(schema, R"("2001:db8::1")"));
    CHECK(validates(schema, R"("2001:0db8:85a3:0000:0000:8a2e:0370:7334")"));
    CHECK(!validates(schema, R"("2001:::1")"));
    CHECK(!validates(schema, R"("gggg::1")"));
}

TEST(schema_vocab_format_ipv6_rejects_ipv4_prefix_before_compression) {
    std::vector<pjson::SchemaError> errors;
    CHECK(!validates(R"({"format":"ipv6"})", R"("192.0.2.128::")", errors));
    CHECK(hasMessageContaining(errors, "format") || hasMessageContaining(errors, "ipv6"));
}

TEST(schema_vocab_format_uuid) {
    const char* schema = R"({"type":"string","format":"uuid"})";
    CHECK(validates(schema, R"("123e4567-e89b-12d3-a456-426614174000")"));
    CHECK(!validates(schema, R"("123e4567e89b12d3a456426614174000")"));
    CHECK(!validates(schema, R"("123e4567-e89b-12d3-a456-42661417400z")"));
}

TEST(schema_vocab_format_unknown_is_ignored_and_disable_option_skips_known_formats) {
    CHECK(validates(R"({"type":"string","format":"unknown-future-format"})", R"("anything")"));

    pjson::SchemaOptions disabled = optionsWithFormatValidation(false);
    CHECK(validates(R"({"type":"string","format":"date"})", R"("not-a-date")", disabled));

    pjson::SchemaOptions enabled = optionsWithFormatValidation(true);
    std::vector<pjson::SchemaError> errors;
    CHECK(!validates(R"({"type":"string","format":"date"})", R"("not-a-date")", errors, enabled));
    CHECK(hasMessageContaining(errors, "format"));
}

//===----------------------------------------------------------------------===//
// Exact multipleOf arithmetic across integer and decimal scales
//===----------------------------------------------------------------------===//

TEST(schema_multiple_of_precision_integer_paths) {
    CHECK(validates(R"({"multipleOf":10})", "9007199254740990"));
    CHECK(!validates(R"({"multipleOf":10})", "9007199254740991"));
    CHECK(validates(R"({"multipleOf":3})", "0"));
}

TEST(schema_multiple_of_precision_decimal_exact_cases) {
    CHECK(validates(R"({"multipleOf":0.1})", "0.3"));
    CHECK(validates(R"({"multipleOf":0.01})", "12.34"));
    CHECK(validates(R"({"multipleOf":0.00000001})", "0.00000012"));
    CHECK(!validates(R"({"multipleOf":0.01})", "12.345"));
}

TEST(schema_multiple_of_precision_decimal_traps) {
    std::vector<pjson::SchemaError> errors;
    CHECK(!validates(R"({"multipleOf":0.1})", "0.30000000000000004", errors));
    CHECK(hasMessageContaining(errors, "multiple"));

    errors.clear();
    CHECK(!validates(R"({"multipleOf":0.0000000001})", "0.0000000003000000001", errors));
    CHECK(hasMessageContaining(errors, "multiple"));
}

TEST(schema_multiple_of_precision_large_and_tiny_scales) {
    CHECK(validates(R"({"multipleOf":0.01})", "1000000000000.01"));
    CHECK(validates(R"({"multipleOf":0.0000000001})", "0.0000000003"));
    CHECK(!validates(R"({"multipleOf":0.0000000001})", "0.00000000035"));
}

TEST(schema_multiple_of_non_positive_schema_values_are_ignored) {
    CHECK(validates(R"({"multipleOf":0})", "5"));
    CHECK(validates(R"({"multipleOf":-2})", "5"));
}

TEST(schema_multiple_of_precision_overflow_case_from_official_suite) {
    std::vector<pjson::SchemaError> errors;
    CHECK(!validates(R"({"type":"integer","multipleOf":0.123456789})", "1e308", errors));
    CHECK(hasMessageContaining(errors, "multiple"));
}

TEST(schema_multiple_of_precision_exact_mixed_numeric_const_enum) {
    pjson constSchema;
    constSchema["const"] = int64_t(9007199254740993LL);

    pjson exactInt;
    exactInt = int64_t(9007199254740993LL);
    CHECK(exactInt.validate(constSchema));

    pjson roundedDouble;
    roundedDouble = double(9007199254740992.0);
    CHECK(!roundedDouble.validate(constSchema));

    pjson enumSchema;
    enumSchema["enum"][0] = int64_t(9007199254740993LL);
    enumSchema["enum"][1] = int64_t(5);
    CHECK(exactInt.validate(enumSchema));
    CHECK(!roundedDouble.validate(enumSchema));
}

TEST(schema_validation_work_budget) {
    const char* schema = R"({
        "type": "object",
        "properties": {
            "items": {
                "type": "array",
                "items": { "type": "integer" }
            }
        }
    })";
    const char* data = R"({"items":[1,2,3,4,5,6,7,8,9,10]})";

    pjson::SchemaOptions constrained = optionsWithWorkBudget(1);
    std::vector<pjson::SchemaError> errors;
    CHECK(!validates(schema, data, errors, constrained));
    CHECK(hasMessageContaining(errors, "work") || hasMessageContaining(errors, "budget"));
}

TEST(schema_validation_work_budget_charges_deep_equality_unicode_and_additional_properties) {
    pjson::SchemaOptions tiny = optionsWithWorkBudget(16);
    std::vector<pjson::SchemaError> errors;

    const pjson deep = makeDeepValue(32, int64_t(1));
    pjson constSchema;
    constSchema["const"] = deep;
    CHECK(!deep.validate(constSchema, errors, tiny));
    CHECK(hasMessageContaining(errors, "work") || hasMessageContaining(errors, "budget"));

    errors.clear();
    pjson enumSchema;
    enumSchema["enum"][0] = deep;
    CHECK(!deep.validate(enumSchema, errors, tiny));
    CHECK(hasMessageContaining(errors, "work") || hasMessageContaining(errors, "budget"));

    errors.clear();
    pjson uniqueSchema;
    uniqueSchema["uniqueItems"] = true;
    pjson duplicateDeep;
    duplicateDeep[0] = deep;
    duplicateDeep[1] = deep;
    CHECK(!duplicateDeep.validate(uniqueSchema, errors, tiny));
    CHECK(hasMessageContaining(errors, "work") || hasMessageContaining(errors, "budget"));

    errors.clear();
    pjson unicodeSchema;
    unicodeSchema["minLength"] = int64_t(1);
    pjson unicodeValue;
    std::string unicode;
    for (size_t i = 0; i < 32; ++i)
        unicode += "\xF0\x9F\x98\x80";
    unicodeValue = unicode;
    CHECK(!unicodeValue.validate(unicodeSchema, errors, tiny));
    CHECK(hasMessageContaining(errors, "work") || hasMessageContaining(errors, "budget"));

    errors.clear();
    pjson additionalSchema;
    additionalSchema["additionalProperties"] = true;
    pjson manyProperties;
    for (size_t i = 0; i < 32; ++i)
        manyProperties["key" + std::to_string(i)] = static_cast<int64_t>(i);
    CHECK(!manyProperties.validate(additionalSchema, errors, tiny));
    CHECK(hasMessageContaining(errors, "work") || hasMessageContaining(errors, "budget"));
}

TEST(schema_additional_properties_large_object_stays_within_work_budget) {
    pjson schema;
    schema["additionalProperties"]["type"] = "integer";
    pjson instance;
    const size_t propertyCount = 2048;
    for (size_t i = 0; i < propertyCount; ++i)
        instance["key" + std::to_string(i)] = static_cast<int64_t>(i);

    pjson::SchemaOptions options = optionsWithWorkBudget(propertyCount * 4U + 16U);
    CHECK(instance.validate(schema, options));
}

TEST(schema_validation_zero_work_budget_uses_hard_ceiling) {
    pjson::SchemaOptions opts = optionsWithWorkBudget(0);
    opts.maxValidationDepth = 64;
    opts.maxRefResolutions = 2000000;
    const pjson schema = makeBranchingWorkSchema(20);
    pjson instance;
    instance = int64_t(1);

    std::vector<pjson::SchemaError> errors;
    CHECK(!instance.validate(schema, errors, opts));
    CHECK(hasMessageContaining(errors, "work") || hasMessageContaining(errors, "budget"));
}

TEST(schema_validation_error_budget) {
    const char* schema = R"({
        "type":"object",
        "required":["a","b","c","d"],
        "properties":{
            "a":{"type":"integer"},
            "b":{"type":"integer"},
            "c":{"type":"integer"},
            "d":{"type":"integer"}
        }
    })";
    const char* data = R"({"a":"x","b":"y"})";

    pjson::SchemaOptions constrained = optionsWithErrorBudget(1);
    std::vector<pjson::SchemaError> errors;
    CHECK(!validates(schema, data, errors, constrained));
    CHECK(errors.size() <= size_t(1));
}

TEST(schema_anyof_scratch_errors_do_not_consume_public_error_budget) {
    pjson::SchemaOptions options = optionsWithErrorBudget(1);
    const char* matchingLast =
        R"({"anyOf":[{"type":"string"},{"minimum":10},{"const":7},{"type":"integer"}]})";
    const char* matchingFirst =
        R"({"anyOf":[{"type":"integer"},{"type":"string"},{"minimum":10},{"const":7}]})";

    std::vector<pjson::SchemaError> errors;
    CHECK(validates(matchingLast, "5", errors, options));
    CHECK(errors.empty());
    CHECK(validates(matchingFirst, "5", errors, options));
    CHECK(errors.empty());
}

TEST(schema_oneof_scratch_errors_do_not_consume_public_error_budget) {
    pjson::SchemaOptions options = optionsWithErrorBudget(1);
    const char* matchingLast =
        R"({"oneOf":[{"type":"string"},{"minimum":10},{"const":7},{"type":"integer"}]})";
    const char* matchingFirst =
        R"({"oneOf":[{"type":"integer"},{"type":"string"},{"minimum":10},{"const":7}]})";

    std::vector<pjson::SchemaError> errors;
    CHECK(validates(matchingLast, "5", errors, options));
    CHECK(errors.empty());
    CHECK(validates(matchingFirst, "5", errors, options));
    CHECK(errors.empty());
}

TEST(schema_not_scratch_error_leaves_budget_for_later_real_failure) {
    pjson::SchemaOptions options = optionsWithErrorBudget(1);
    const char* schema = R"({
        "allOf": [
            {"not": {"type": "string"}},
            {"type": "string"}
        ]
    })";

    std::vector<pjson::SchemaError> errors;
    CHECK(!validates(schema, "5", errors, options));
    CHECK_EQ(errors.size(), size_t(1));
    CHECK(hasMessageContaining(errors, "string"));
}

TEST(schema_speculative_branches_discard_large_hidden_error_sets) {
    const size_t requiredCount = 4096;
    pjson failingBranch;
    failingBranch["type"] = "object";
    for (size_t i = 0; i < requiredCount; ++i) {
        failingBranch["required"][static_cast<int>(i)] =
            std::string("missing-") + std::to_string(i);
    }

    pjson anyOfSchema;
    anyOfSchema["anyOf"][0] = failingBranch;
    anyOfSchema["anyOf"][1] = true;

    pjson instance;
    instance.resetTo(pjson::jsonObject);
    pjson::SchemaOptions options;
    options.maxErrors = 1;
    options.maxValidationWork = requiredCount * 4;
    std::vector<pjson::SchemaError> errors;

    CHECK(instance.validate(anyOfSchema, errors, options));
    CHECK(errors.empty());
}

TEST(schema_validation_zero_error_budget_uses_hard_ceiling) {
    const char* schema = R"({"type":"object","required":["a","b","c"]})";
    const char* data = R"({})";

    pjson::SchemaOptions opts = optionsWithErrorBudget(0);
    std::vector<pjson::SchemaError> errors;
    CHECK(!validates(schema, data, errors, opts));
    CHECK(!errors.empty());
    CHECK(errors.size() <= size_t(100));
}
