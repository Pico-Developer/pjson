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

#include <map>
#include <stdexcept>
#include <string>
#include <thread>
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

    struct ResolverFixture {
        std::map<std::string, pjson> documents;
        size_t calls;
        ResolverFixture()
                : calls(0) {}
    };

    struct CountingAllocator : pjson::Allocator {
        size_t allocations;
        size_t deallocations;
        CountingAllocator()
                : allocations(0)
                , deallocations(0) {}
        void* allocate(size_t size, size_t, AllocationKind) override {
            ++allocations;
            return ::operator new(size);
        }
        void deallocate(void* pointer, size_t, size_t, AllocationKind) noexcept override {
            ++deallocations;
            ::operator delete(pointer);
        }
    };

    bool resolveFixture(const std::string& uri, pjson& output, void* context) {
        ResolverFixture& fixture = *static_cast<ResolverFixture*>(context);
        ++fixture.calls;
        std::map<std::string, pjson>::const_iterator found = fixture.documents.find(uri);
        if (found == fixture.documents.end())
            return false;
        output.copyFrom(found->second);
        return true;
    }

    bool throwingResolver(const std::string&, pjson&, void*) {
        throw std::runtime_error("resolver failure");
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
    // contentSchema is a standard 2020-12 keyword pjson does not enforce.
    const char* schema = "{\"type\":\"object\",\"contentSchema\":false}";
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

//===----------------------------------------------------------------------===//
// PJSON-SCHEMA-001: explicit dialect and vocabulary contract.
//===----------------------------------------------------------------------===//
TEST(schema_documented_subset_dialect_is_explicit_and_reusable) {
    pjson schema;
    schema["$schema"] = pJsonSchemaValidator::documentedSubsetDialectUri();
    schema["type"] = "integer";

    pJsonSchemaValidator validator(schema);
    CHECK(validator.isSchemaValid());
    CHECK(validator.schemaErrors().empty());
    CHECK_EQ(validator.dialect(), std::string(pJsonSchemaValidator::documentedSubsetDialectUri()));

    pjson integerValue;
    integerValue = int64_t(7);
    pjson stringValue;
    stringValue = "seven";
    CHECK(validator.validate(integerValue));
    CHECK(!validator.validate(stringValue));
}

TEST(schema_unsupported_declared_dialect_fails_compilation) {
    pjson schema;
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    schema["type"] = "integer";
    pJsonSchemaValidator validator(schema);

    CHECK(!validator.isSchemaValid());
    CHECK_EQ(validator.schemaErrors().size(), size_t(1));
    CHECK_EQ(validator.schemaErrors()[0].category, pJsonSchemaValidator::Error::SchemaCompilation);
    CHECK_EQ(validator.schemaErrors()[0].code, pJsonSchemaValidator::Error::UnsupportedDialect);
    CHECK_EQ(validator.schemaErrors()[0].schemaLocation, std::string("/$schema"));
    CHECK_EQ(validator.schemaErrors()[0].keyword, std::string("$schema"));

    pjson value;
    value = int64_t(7);
    std::vector<pJsonSchemaValidator::Error> errors;
    CHECK(!validator.validate(value, errors));
    CHECK_EQ(errors.size(), size_t(1));
    CHECK_EQ(errors[0].category, pJsonSchemaValidator::Error::SchemaCompilation);
}

TEST(schema_draft2020_preset_accepts_official_dialect) {
    pjson schema = pjson::parse(
        R"({"$schema":"https://json-schema.org/draft/2020-12/schema","type":"integer"})");
    pJsonSchemaValidator validator(schema, pJsonSchemaValidator::Options::draft2020());
    CHECK(validator.isSchemaValid());
    CHECK_EQ(validator.dialect(), std::string(pJsonSchemaValidator::draft2020DialectUri()));
    pjson integerValue;
    integerValue = int64_t(7);
    pjson stringValue;
    stringValue = "seven";
    CHECK(validator.validate(integerValue));
    CHECK(!validator.validate(stringValue));
}

TEST(schema_draft2020_preset_rejects_meta_schema_violation) {
    pjson schema = pjson::parse(
        R"({"$schema":"https://json-schema.org/draft/2020-12/schema","$defs":{"bad":{"type":1}}})");
    pJsonSchemaValidator validator(schema, pJsonSchemaValidator::Options::draft2020());
    CHECK(!validator.isSchemaValid());
    CHECK(!validator.schemaErrors().empty());
    CHECK_EQ(validator.schemaErrors()[0].category, pJsonSchemaValidator::Error::SchemaCompilation);
    CHECK(validator.schemaErrors()[0].schemaLocation.find("/$defs/bad/type") != std::string::npos);
}

TEST(schema_custom_dialect_controls_validation_vocabulary) {
    const std::string dialect = "https://example.test/meta/no-validation";
    ResolverFixture fixture;
    fixture.documents[dialect] = pjson::parse(
        R"({"$id":"https://example.test/meta/no-validation","$vocabulary":{"https://json-schema.org/draft/2020-12/vocab/core":true,"https://json-schema.org/draft/2020-12/vocab/applicator":true}})");
    pjson schema = pjson::parse(
        R"({"$schema":"https://example.test/meta/no-validation","properties":{"blocked":false,"number":{"minimum":10}}})");
    pJsonSchemaValidator::Options options = pJsonSchemaValidator::Options::draft2020();
    options.resolver = resolveFixture;
    options.resolverContext = &fixture;
    pJsonSchemaValidator validator(schema, options);
    CHECK(validator.isSchemaValid());
    CHECK_EQ(fixture.calls, size_t(1));
    CHECK(!validator.validate(pjson::parse(R"({"blocked":1})")));
    CHECK(validator.validate(pjson::parse(R"({"number":1})")));
}

TEST(schema_custom_dialect_is_charged_once_as_a_resolved_document) {
    const std::string dialect = "https://example.test/meta/small";
    ResolverFixture fixture;
    fixture.documents[dialect] = pjson::parse(
        R"({"$id":"https://example.test/meta/small","$vocabulary":{"https://json-schema.org/draft/2020-12/vocab/core":true,"https://json-schema.org/draft/2020-12/vocab/validation":true},"type":["object","boolean"]})");
    pjson schema =
        pjson::parse(R"({"$schema":"https://example.test/meta/small","type":"integer"})");
    pJsonSchemaValidator::Options options = pJsonSchemaValidator::Options::draft2020();
    options.resolver = resolveFixture;
    options.resolverContext = &fixture;
    options.maxResolvedDocuments = 1;
    pJsonSchemaValidator validator(schema, options);
    CHECK(validator.isSchemaValid());
    CHECK_EQ(fixture.calls, size_t(1));
    pjson value;
    value = int64_t(3);
    CHECK(validator.validate(value));
}

TEST(schema_invalid_root_dialect_does_not_invoke_resolver) {
    ResolverFixture fixture;
    pjson schema =
        pjson::parse(R"({"$schema":"urn:unsupported","$ref":"https://example.test/remote.json"})");
    pJsonSchemaValidator::Options options;
    options.resolver = resolveFixture;
    options.resolverContext = &fixture;
    pJsonSchemaValidator validator(schema, options);
    CHECK(!validator.isSchemaValid());
    CHECK_EQ(fixture.calls, size_t(0));
}

TEST(schema_unsupported_default_dialect_fails_when_schema_omits_schema_keyword) {
    pjson schema;
    schema["type"] = "integer";
    pJsonSchemaValidator::Options options;
    options.defaultDialectUri = "urn:example:unsupported-dialect";
    pJsonSchemaValidator validator(schema, options);
    CHECK(!validator.isSchemaValid());
    CHECK_EQ(validator.dialect(), std::string("urn:example:unsupported-dialect"));
}

TEST(schema_vocabulary_contract_accepts_supported_and_optional_unknown) {
    pjson schema;
    schema["$schema"] = pJsonSchemaValidator::documentedSubsetDialectUri();
    schema["$vocabulary"][pJsonSchemaValidator::documentedSubsetVocabularyUri()] = true;
    schema["$vocabulary"]["urn:example:optional-annotations"] = false;
    schema["type"] = "string";

    pJsonSchemaValidator validator(schema);
    CHECK(validator.isSchemaValid());
    pjson value;
    value = "ok";
    CHECK(validator.validate(value));
}

TEST(schema_vocabulary_contract_rejects_unknown_required_vocabulary) {
    pjson schema;
    schema["$schema"] = pJsonSchemaValidator::documentedSubsetDialectUri();
    schema["$vocabulary"]["urn:example:required-but-unsupported"] = true;
    pJsonSchemaValidator validator(schema);

    CHECK(!validator.isSchemaValid());
    CHECK_EQ(validator.schemaErrors().size(), size_t(1));
    CHECK(validator.schemaErrors()[0].message.find("unsupported required") != std::string::npos);
}

TEST(schema_vocabulary_contract_accepts_supported_required_vocabulary) {
    pjson schema;
    schema["$vocabulary"][pJsonSchemaValidator::documentedSubsetVocabularyUri()] = true;
    schema["minimum"] = int64_t(10);
    pJsonSchemaValidator validator(schema);
    CHECK(validator.isSchemaValid());

    pjson below;
    below = int64_t(9);
    CHECK(!validator.validate(below));
}

TEST(schema_empty_default_dialect_selects_documented_subset) {
    pjson schema;
    pJsonSchemaValidator::Options options;
    options.defaultDialectUri.clear();
    pJsonSchemaValidator validator(schema, options);
    CHECK(validator.isSchemaValid());
    CHECK_EQ(validator.dialect(), std::string(pJsonSchemaValidator::documentedSubsetDialectUri()));
}

TEST(schema_dialect_and_vocabulary_shapes_are_compilation_errors) {
    pjson badDialect;
    badDialect["$schema"] = int64_t(202012);
    pJsonSchemaValidator dialectValidator(badDialect);
    CHECK(!dialectValidator.isSchemaValid());

    pjson badVocabulary;
    badVocabulary["$vocabulary"] = "not-an-object";
    pJsonSchemaValidator vocabularyValidator(badVocabulary);
    CHECK(!vocabularyValidator.isSchemaValid());

    pjson badEntry;
    badEntry["$vocabulary"]["urn:example:vocabulary"] = "required";
    pJsonSchemaValidator entryValidator(badEntry);
    CHECK(!entryValidator.isSchemaValid());

    pjson nonSchema;
    nonSchema = int64_t(7);
    pJsonSchemaValidator::Options strict = pJsonSchemaValidator::Options::strict();
    pJsonSchemaValidator nonSchemaValidator(nonSchema, strict);
    CHECK(!nonSchemaValidator.isSchemaValid());
}

TEST(schema_reference_and_anchor_shapes_fail_validation_safely) {
    pjson value;
    for (const char* schemaText : {R"({"$ref":1})", R"({"$dynamicRef":false})", R"({"$id":[]})",
                                   R"({"$anchor":"bad/name"})", R"({"$dynamicAnchor":""})"}) {
        pjson schema = pjson::parse(schemaText);
        pJsonSchemaValidator validator(schema);
        std::vector<pJsonSchemaValidator::Error> errors;
        CHECK(!validator.validate(value, errors));
        CHECK(!errors.empty());
    }

    pjson nonSchemaTarget = pjson::parse(R"({"$ref":"#/$defs/value","$defs":{"value":7}})");
    pJsonSchemaValidator::Options strict = pJsonSchemaValidator::Options::strict();
    pJsonSchemaValidator targetValidator(nonSchemaTarget, strict);
    CHECK(!targetValidator.isSchemaValid());
}

TEST(schema_strict_mode_rejects_every_supported_keyword_shape) {
    struct ShapeCase {
        const char* schema;
        const char* location;
        const char* keyword;
        pJsonSchemaValidator::Error::Code code;
    };
    const ShapeCase cases[] = {
        {R"({"type":"unknown"})", "/type", "type", pJsonSchemaValidator::Error::InvalidSchema},
        {R"({"type":["string","string"]})", "/type", "type",
         pJsonSchemaValidator::Error::InvalidSchema},
        {R"({"enum":[]})", "/enum", "enum", pJsonSchemaValidator::Error::InvalidSchema},
        {R"({"enum":[1,1.0]})", "/enum", "enum", pJsonSchemaValidator::Error::InvalidSchema},
        {R"({"pattern":3})", "/pattern", "pattern", pJsonSchemaValidator::Error::InvalidSchema},
        {R"({"pattern":"["})", "/pattern", "pattern", pJsonSchemaValidator::Error::RegexFailure},
        {R"({"format":false})", "/format", "format", pJsonSchemaValidator::Error::InvalidSchema},
        {R"({"uniqueItems":1})", "/uniqueItems", "uniqueItems",
         pJsonSchemaValidator::Error::InvalidSchema},
        {R"({"minimum":"0"})", "/minimum", "minimum", pJsonSchemaValidator::Error::InvalidSchema},
        {R"({"multipleOf":0})", "/multipleOf", "multipleOf",
         pJsonSchemaValidator::Error::InvalidSchema},
        {R"({"minLength":-1})", "/minLength", "minLength",
         pJsonSchemaValidator::Error::InvalidSchema},
        {R"({"maxItems":1.5})", "/maxItems", "maxItems",
         pJsonSchemaValidator::Error::InvalidSchema},
        {R"({"additionalProperties":7})", "/additionalProperties", "additionalProperties",
         pJsonSchemaValidator::Error::InvalidSchema},
        {R"({"items":"schema"})", "/items", "items", pJsonSchemaValidator::Error::InvalidSchema},
        {R"({"allOf":[]})", "/allOf", "allOf", pJsonSchemaValidator::Error::InvalidSchema},
        {R"({"anyOf":[true,7]})", "/anyOf/1", "anyOf", pJsonSchemaValidator::Error::InvalidSchema},
        {R"({"prefixItems":false})", "/prefixItems", "prefixItems",
         pJsonSchemaValidator::Error::InvalidSchema},
        {R"({"prefixItems":[]})", "/prefixItems", "prefixItems",
         pJsonSchemaValidator::Error::InvalidSchema},
        {R"({"properties":[]})", "/properties", "properties",
         pJsonSchemaValidator::Error::InvalidSchema},
        {R"({"properties":{"x":7}})", "/properties/x", "properties",
         pJsonSchemaValidator::Error::InvalidSchema},
        {R"({"required":["x","x"]})", "/required", "required",
         pJsonSchemaValidator::Error::InvalidSchema},
        {R"({"dependentRequired":{"x":["y","y"]}})", "/dependentRequired/x", "dependentRequired",
         pJsonSchemaValidator::Error::InvalidSchema},
        {R"({"dependencies":{"x":7}})", "/dependencies/x", "dependencies",
         pJsonSchemaValidator::Error::InvalidSchema},
        {R"({"examples":false})", "/examples", "examples",
         pJsonSchemaValidator::Error::InvalidSchema},
        {R"({"contentSchema":{}})", "/contentSchema", "contentSchema",
         pJsonSchemaValidator::Error::UnsupportedKeyword},
    };

    const pJsonSchemaValidator::Options strict = pJsonSchemaValidator::Options::strict();
    for (const ShapeCase& test : cases) {
        const pjson schema = pjson::parse(test.schema);
        const pJsonSchemaValidator validator(schema, strict);
        CHECK(!validator.isSchemaValid());
        CHECK(!validator.schemaErrors().empty());
        if (!validator.schemaErrors().empty()) {
            CHECK_EQ(validator.schemaErrors()[0].code, test.code);
            CHECK_EQ(validator.schemaErrors()[0].schemaLocation, std::string(test.location));
            CHECK_EQ(validator.schemaErrors()[0].keyword, std::string(test.keyword));
        }
    }
}

TEST(schema_permissive_mode_still_ignores_malformed_keyword_shapes) {
    pjson schema = pjson::parse(
        R"({"type":7,"required":false,"properties":[],"allOf":false,"minimum":"zero"})");
    pJsonSchemaValidator validator(schema);
    CHECK(validator.isSchemaValid());
    pjson value;
    value = "accepted";
    CHECK(validator.validate(value));
}

TEST(schema_ids_inside_instance_valued_keywords_are_not_indexed) {
    pjson schema = pjson::parse(
        R"({"const":{"$id":"https://example.test/not-a-schema","value":1},"$defs":{"actual":{"$id":"https://example.test/not-a-schema","type":"integer"}}})");
    pJsonSchemaValidator validator(schema);
    pjson equalValue = pjson::parse(R"({"$id":"https://example.test/not-a-schema","value":1})");
    CHECK(validator.validate(equalValue));
}

TEST(schema_compilation_depth_is_bounded_for_programmatic_schemas) {
    pjson schema;
    pjson* cursor = &schema;
    for (size_t i = 0; i < 1000; ++i)
        cursor = &((*cursor)["allOf"][0]);
    pJsonSchemaValidator validator(schema);
    CHECK(!validator.isSchemaValid());
    CHECK(!validator.schemaErrors().empty());
    CHECK(validator.schemaErrors()[0].message.find("compilation depth") != std::string::npos);
}

TEST(schema_duplicate_resource_ids_and_anchors_are_rejected) {
    pjson duplicateId = pjson::parse(
        R"({"$id":"https://example.test/root","$defs":{"a":{"$id":"child"},"b":{"$id":"child"}}})");
    pJsonSchemaValidator idValidator(duplicateId);
    CHECK(!idValidator.isSchemaValid());

    pjson duplicateAnchor =
        pjson::parse(R"({"$defs":{"a":{"$anchor":"same"},"b":{"$anchor":"same"}}})");
    pJsonSchemaValidator anchorValidator(duplicateAnchor);
    CHECK(!anchorValidator.isSchemaValid());

    pjson malformedAnchor = pjson::parse(R"({"$defs":{"a":{"$anchor":7}}})");
    pJsonSchemaValidator malformedAnchorValidator(malformedAnchor);
    CHECK(!malformedAnchorValidator.isSchemaValid());
}

//===----------------------------------------------------------------------===//
// PJSON-SCHEMA-004: URI resources, anchors, dynamic references, and explicit
// resolver callbacks. pjson never performs implicit I/O.
//===----------------------------------------------------------------------===//
TEST(schema_anchor_and_nested_id_resolution) {
    CHECK(validates(
        R"({"$ref":"#integer","$defs":{"value":{"$anchor":"integer","type":"integer"}}})", "7"));
    CHECK(!validates(
        R"({"$ref":"#integer","$defs":{"value":{"$anchor":"integer","type":"integer"}}})",
        R"("seven")"));

    const char* nested =
        R"({"$id":"https://example.test/root.json","$ref":"nested.json#value","$defs":{"nested":{"$id":"nested.json","$defs":{"v":{"$anchor":"value","type":"string"}}}}})";
    CHECK(validates(nested, R"("ok")"));
    CHECK(!validates(nested, "9"));

    const char* encodedPointer =
        R"({"$ref":"#%2F$defs%2Fvalue","$defs":{"value":{"type":"boolean"}}})";
    CHECK(validates(encodedPointer, "true"));
    CHECK(!validates(encodedPointer, "0"));
}

TEST(schema_external_resolver_and_fragment) {
    ResolverFixture fixture;
    fixture.documents["https://example.test/remote.json"] = pjson::parse(
        R"({"$id":"https://example.test/remote.json","$defs":{"value":{"type":"integer"}}})");

    pjson schema = pjson::parse(R"({"$ref":"https://example.test/remote.json#/$defs/value"})");
    pJsonSchemaValidator::Options options;
    options.resolver = resolveFixture;
    options.resolverContext = &fixture;
    pJsonSchemaValidator validator(schema, options);
    CHECK_EQ(fixture.calls, size_t(1));
    CHECK(validator.options().resolver == nullptr);
    CHECK(validator.options().resolverContext == nullptr);

    pjson valid;
    valid = int64_t(5);
    pjson invalid;
    invalid = "five";
    CHECK(validator.validate(valid));
    std::vector<pJsonSchemaValidator::Error> errors;
    CHECK(!validator.validate(invalid, errors));
    CHECK_EQ(errors.size(), size_t(1));
    CHECK_EQ(errors[0].code, pJsonSchemaValidator::Error::TypeMismatch);
    CHECK_EQ(errors[0].instanceLocation, std::string());
    CHECK_EQ(errors[0].schemaLocation,
             std::string("https://example.test/remote.json#/$defs/value/type"));
    CHECK_EQ(errors[0].keyword, std::string("type"));
    CHECK_EQ(fixture.calls, size_t(1)); // resolved once during construction
}

TEST(schema_retrieval_uri_resolves_relative_root_reference) {
    ResolverFixture fixture;
    fixture.documents["https://example.test/schemas/remote.json"] =
        pjson::parse(R"({"type":"integer"})");
    pjson schema = pjson::parse(R"({"$ref":"remote.json"})");
    pJsonSchemaValidator::Options options;
    options.retrievalUri = "https://example.test/schemas/root.json";
    options.resolver = resolveFixture;
    options.resolverContext = &fixture;
    pJsonSchemaValidator validator(schema, options);
    CHECK(validator.isSchemaValid());
    CHECK_EQ(fixture.calls, size_t(1));
    pjson integerValue;
    integerValue = int64_t(1);
    CHECK(validator.validate(integerValue));

    pjson noBaseSchema = pjson::parse(R"({"$ref":"remote.json"})");
    pJsonSchemaValidator noBase(noBaseSchema, options);
    // `options` supplies retrievalUri here, so this remains valid.
    CHECK(noBase.isSchemaValid());
    pJsonSchemaValidator::Options missingBaseOptions;
    missingBaseOptions.resolver = resolveFixture;
    missingBaseOptions.resolverContext = &fixture;
    pJsonSchemaValidator missingBase(noBaseSchema, missingBaseOptions);
    CHECK(!missingBase.isSchemaValid());
}

TEST(schema_reference_resolution_normalizes_dot_segments) {
    ResolverFixture fixture;
    fixture.documents["https://example.test/schemas/remote.json"] =
        pjson::parse(R"({"type":"integer"})");
    pjson schema = pjson::parse(R"({"$ref":"./defs/../remote.json"})");
    pJsonSchemaValidator::Options options;
    options.retrievalUri = "https://example.test/schemas/root.json";
    options.resolver = resolveFixture;
    options.resolverContext = &fixture;
    pJsonSchemaValidator validator(schema, options);
    CHECK(validator.isSchemaValid());
    CHECK_EQ(fixture.calls, size_t(1));
    pjson value;
    value = int64_t(1);
    CHECK(validator.validate(value));
}

TEST(schema_retrieval_uri_applies_relative_root_id_once) {
    pjson schema = pjson::parse(
        R"({"$id":"sub/root.json","$ref":"#value","$defs":{"v":{"$anchor":"value","type":"string"}}})");
    pJsonSchemaValidator::Options options;
    options.retrievalUri = "https://example.test/schemas/source.json";
    pJsonSchemaValidator validator(schema, options);
    CHECK(validator.isSchemaValid());
    pjson value;
    value = "ok";
    CHECK(validator.validate(value));
}

TEST(schema_external_resolution_is_explicit_and_budgeted) {
    pjson schema = pjson::parse(R"({"$ref":"https://example.test/remote.json"})");
    pJsonSchemaValidator noResolver(schema);
    pjson value;
    std::vector<pJsonSchemaValidator::Error> errors;
    CHECK(!noResolver.isSchemaValid());
    CHECK(!noResolver.validate(value, errors));
    CHECK(!errors.empty());
    CHECK(errors[0].message.find("no resolver") != std::string::npos);

    ResolverFixture fixture;
    fixture.documents["https://example.test/remote.json"] = pjson::parse(R"({"type":"null"})");
    pJsonSchemaValidator::Options options;
    options.resolver = resolveFixture;
    options.resolverContext = &fixture;
    options.maxResolvedBytes = 1;
    pJsonSchemaValidator limited(schema, options);
    errors.clear();
    CHECK(!limited.validate(value, errors));
    CHECK(!errors.empty());
    CHECK(errors[0].message.find("resolved-byte budget") != std::string::npos);

    options.maxResolvedBytes = size_t(16) * 1024 * 1024;
    options.maxResolvedDocuments = 1;
    pjson twoDocuments = pjson::parse(
        R"({"allOf":[{"$ref":"https://example.test/one.json"},{"$ref":"https://example.test/two.json"}]})");
    fixture.documents["https://example.test/one.json"] = pjson::parse("true");
    fixture.documents["https://example.test/two.json"] = pjson::parse("true");
    pJsonSchemaValidator documentLimited(twoDocuments, options);
    CHECK(!documentLimited.isSchemaValid());
    CHECK(documentLimited.schemaErrors()[0].message.find("resolved-document budget") !=
          std::string::npos);
}

TEST(schema_external_resolver_exception_becomes_compilation_error) {
    pjson schema = pjson::parse(R"({"$ref":"https://example.test/remote.json"})");
    pJsonSchemaValidator::Options options;
    options.resolver = throwingResolver;
    bool caught = false;
    try {
        pJsonSchemaValidator validator(schema, options);
        CHECK(!validator.isSchemaValid());
    } catch (...) {
        caught = true;
    }
    CHECK(!caught);
}

TEST(schema_external_resource_with_unsupported_dialect_fails_compilation) {
    ResolverFixture fixture;
    fixture.documents["https://example.test/remote.json"] = pjson::parse(
        R"({"$schema":"https://json-schema.org/draft/2020-12/schema","type":"integer"})");
    pjson schema = pjson::parse(R"({"$ref":"https://example.test/remote.json"})");
    pJsonSchemaValidator::Options options;
    options.resolver = resolveFixture;
    options.resolverContext = &fixture;
    pJsonSchemaValidator validator(schema, options);
    CHECK(!validator.isSchemaValid());
    CHECK(!validator.schemaErrors().empty());
    CHECK_EQ(validator.schemaErrors()[0].schemaLocation,
             std::string("https://example.test/remote.json#/$schema"));

    fixture.documents["https://example.test/remote.json"] =
        pjson::parse(R"({"$vocabulary":{"urn:example:required":true}})");
    pJsonSchemaValidator vocabularyValidator(schema, options);
    CHECK(!vocabularyValidator.isSchemaValid());
}

TEST(schema_validator_owns_schema_beyond_caller_allocator_lifetime) {
    pJsonSchemaValidator* validator = nullptr;
    {
        CountingAllocator allocator;
        pjson::ParseError error;
        pjson schema = pjson::parse(R"({"type":"integer"})", error, allocator);
        CHECK(error.ok);
        validator = new pJsonSchemaValidator(schema);
        CHECK(&validator->schema().getAllocator() != &allocator);
    }
    pjson value;
    value = int64_t(4);
    CHECK(validator->validate(value));
    delete validator;
}

TEST(schema_compiled_validator_supports_concurrent_read_only_validation) {
    pjson schema = pjson::parse(
        R"({"type":"object","properties":{"value":{"type":"integer"}},"required":["value"],"unevaluatedProperties":false})");
    const pJsonSchemaValidator validator(schema, pJsonSchemaValidator::Options::modernSubset());
    bool results[8] = {false, false, false, false, false, false, false, false};
    std::vector<std::thread> threads;
    for (size_t i = 0; i < 8; ++i) {
        threads.push_back(std::thread([&validator, &results, i]() {
            pjson instance;
            instance["value"] = int64_t(i);
            std::vector<pJsonSchemaValidator::Error> errors;
            results[i] = validator.validate(instance, errors) && errors.empty();
        }));
    }
    for (size_t i = 0; i < threads.size(); ++i)
        threads[i].join();
    for (size_t i = 0; i < 8; ++i)
        CHECK(results[i]);
}

TEST(schema_dynamic_ref_uses_outer_dynamic_anchor) {
    const char* schema =
        R"({"$id":"https://example.test/strict-tree","$dynamicAnchor":"node","type":"object","properties":{"value":{"type":"integer"},"child":{"$dynamicRef":"#node"}},"required":["value"],"additionalProperties":false})";
    CHECK(validates(schema, R"({"value":1,"child":{"value":2}})"));
    CHECK(!validates(schema, R"({"value":1,"child":{"value":"bad"}})"));
}

TEST(schema_unevaluated_properties_collects_successful_applicator_annotations) {
    const char* schema =
        R"({"allOf":[{"properties":{"a":{"type":"integer"}}}],"anyOf":[{"properties":{"b":{"type":"string"}}},{"properties":{"c":true}}],"unevaluatedProperties":false})";
    CHECK(validates(schema, R"({"a":1,"b":"ok","c":true})"));
    CHECK(!validates(schema, R"({"a":1,"b":"ok","extra":true})"));
}

TEST(schema_unevaluated_items_collects_prefix_contains_and_conditionals) {
    const char* schema =
        R"({"prefixItems":[{"type":"string"}],"contains":{"type":"integer"},"unevaluatedItems":false})";
    CHECK(validates(schema, R"(["head",1,2])"));
    CHECK(!validates(schema, R"(["head",1,true])"));

    const char* conditional = R"({"if":{"prefixItems":[{"const":"a"}]},"unevaluatedItems":false})";
    CHECK(validates(conditional, R"(["a"])"));
    CHECK(!validates(conditional, R"(["b"])"));
}

TEST(schema_unevaluated_keywords_ignore_non_container_instances) {
    CHECK(validates(R"({"unevaluatedProperties":false})", "7"));
    CHECK(validates(R"({"unevaluatedItems":false})", R"("value")"));
}
