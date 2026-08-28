// Copyright 2025 ByteDance Ltd. and/or its affiliates. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License");

#include "fuzz_util.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using ByteDance::pjson;

// Schema-validation consistency.

// Splits a fuzz case into a schema and document, then compares validation overloads.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > pjson_fuzz::kMaxInputBytes)
        return 0;

    // Prefer `schema\ndocument` framing; unframed inputs are divided at their midpoint.
    const std::string input(pjson_fuzz::bytes(data, size), size);
    std::string schemaInput;
    std::string documentInput;
    pjson_fuzz::splitOnNewlineOrMidpoint(input, schemaInput, documentInput);

    // Only pairs that are both valid strict JSON values can exercise schema validation.
    const pjson::ParseOptions options = pjson_fuzz::parseOptionsVariant(data, size, 0U);
    pjson::unique_ptr schema = pjson::parse(schemaInput, options);
    pjson::unique_ptr document = pjson::parse(documentInput, options);
    if (!schema || !document)
        return 0;

    // Detailed and simple validation must agree, and errors exist exactly on failure.
    const pjson::SchemaOptions schemaOptions = pjson_fuzz::boundedSchemaOptions(data, size, 4U);
    std::vector<pjson::SchemaError> errors;
    const bool detailed = document->validate(*schema, errors, schemaOptions);
    const bool simple = document->validate(*schema, schemaOptions);
    pjson_fuzz::require(simple == detailed);
    pjson_fuzz::require(detailed == errors.empty());
    return 0;
}
