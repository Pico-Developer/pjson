// Copyright 2025 ByteDance Ltd. and/or its affiliates. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License");

#include "fuzz_util.h"

#include <cstddef>
#include <cstdint>
#include <string>

using ByteDance::pjson;
using ByteDance::pJsonParser;

namespace {

    // Applies RFC 6902 JSON Patch and checks the non-throwing API contracts
    // that are observable from fuzz-side callers.
    void exercisePatchVariant(const uint8_t* data, size_t size, const std::string& documentInput,
                              const std::string& patchInput, size_t variantOffset) {
        const pJsonParser::Options options =
            pjson_fuzz::parseOptionsVariant(data, size, variantOffset);
        pJsonParser::Error originalError;
        pJsonParser::Error patchError;
        pjson original = pJsonParser(options).parse(documentInput, originalError);
        pjson patch = pJsonParser(options).parse(patchInput, patchError);
        if (!originalError.ok || !patchError.ok)
            return;

        if (!patch.isArray())
            return;
        pjson working = original;
        pjson::PatchError detailedError;
        const pjson::PatchOptions patchOptions =
            pjson_fuzz::patchOptionsVariant(data, size, variantOffset);
        const bool detailedOk = working.applyPatch(patch, detailedError, patchOptions);
        pjson_fuzz::require(detailedOk == detailedError.ok);

        pjson simple = original;
        const bool simpleOk = simple.applyPatch(patch, patchOptions);
        pjson_fuzz::require(simpleOk == detailedOk);

        if (!detailedOk) {
            // Failure must leave the document unchanged because patch application is atomic.
            pjson_fuzz::require(working == original);
            pjson_fuzz::require(simple == original);
            return;
        }

        // Successful mutation must serialize and reparse stably.
        pjson_fuzz::require(working == simple);
        const std::string compact = working.toString();
        pJsonParser::Options compactOptions = options;
        compactOptions.maxInputBytes = compact.size();
        pJsonParser::Error reparsedError;
        pjson reparsed = pJsonParser(compactOptions).parse(compact, reparsedError);
        pjson_fuzz::require(reparsedError.ok);
        pjson_fuzz::require(reparsed == working);

        const std::string pretty = working.toString(pjson::SerializeOptions::prettyPrinted());
        pJsonParser::Options prettyOptions = options;
        prettyOptions.maxInputBytes = pretty.size();
        pJsonParser::Error prettyError;
        pjson prettyParsed = pJsonParser(prettyOptions).parse(pretty, prettyError);
        pjson_fuzz::require(prettyError.ok);
        pjson_fuzz::require(prettyParsed == working);
    }

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > pjson_fuzz::kMaxInputBytes)
        return 0;

    const std::string input(pjson_fuzz::bytes(data, size), size);
    std::string documentInput;
    std::string patchInput;
    pjson_fuzz::splitOnNewlineOrMidpoint(input, documentInput, patchInput);

    exercisePatchVariant(data, size, documentInput, patchInput, 0U);
    exercisePatchVariant(data, size, documentInput, patchInput, 4U);
    return 0;
}
