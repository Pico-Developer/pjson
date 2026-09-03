// Copyright 2025 ByteDance Ltd. and/or its affiliates. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License");

#include "fuzz_util.h"

#include <cstddef>
#include <cstdint>
#include <string>

using ByteDance::pjson;
using ByteDance::pJsonParser;

namespace {

    // DOM parsing and serialization invariants.

    // Exercises one parser configuration and checks successful
    // values across both compact and pretty serialization modes.
    void exerciseParser(const uint8_t* data, size_t size, size_t variantOffset) {
        const pJsonParser::Options options =
            pjson_fuzz::parseOptionsVariant(data, size, variantOffset);
        pJsonParser::Error error;
        pjson value = pJsonParser(options).parse(pjson_fuzz::bytes(data, size), size, error);
        if (!error.ok)
            return;

        // Compact output must be a stable, value-preserving representation.
        const std::string compact = value.toString();
        pJsonParser::Options compactOptions = options;
        compactOptions.maxInputBytes = compact.size();
        pJsonParser::Error compactError;
        pjson reparsed = pJsonParser(compactOptions).parse(compact, compactError);
        pjson_fuzz::require(compactError.ok);
        pjson_fuzz::require(reparsed == value);
        pjson_fuzz::require(reparsed.toString() == compact);

        // Pretty printing may change whitespace, but never the represented JSON value.
        const std::string pretty = value.toString(pjson::SerializeOptions::prettyPrinted());
        pJsonParser::Options prettyOptions = options;
        prettyOptions.maxInputBytes = pretty.size();
        pJsonParser::Error prettyError;
        pjson prettyParsed = pJsonParser(prettyOptions).parse(pretty, prettyError);
        pjson_fuzz::require(prettyError.ok);
        pjson_fuzz::require(prettyParsed == value);
    }

} // namespace

// libFuzzer entry point.

// Bounds each test case and exercises the same bytes under several
// duplicate-policy/resource-budget variants.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > pjson_fuzz::kMaxInputBytes)
        return 0;
    exerciseParser(data, size, 0U);
    exerciseParser(data, size, 4U);
    exerciseParser(data, size, 8U);
    return 0;
}
