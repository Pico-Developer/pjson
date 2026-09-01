// Copyright 2025 ByteDance Ltd. and/or its affiliates. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License");

#include "fuzz_util.h"

#include <cstddef>
#include <cstdint>
#include <string>

using ByteDance::pjson;

namespace {

    // Applies either RFC 6902 JSON Patch or RFC 7396 Merge Patch and checks the
    // non-throwing API contracts that are observable from fuzz-side callers.
    void exercisePatchVariant(const uint8_t* data, size_t size, const std::string& documentInput,
                              const std::string& patchInput, size_t variantOffset) {
        const pjson::ParseOptions options =
            pjson_fuzz::parseOptionsVariant(data, size, variantOffset);
        pjson::ParseError originalError;
        pjson::ParseError patchError;
        pjson original = pjson::parse(documentInput, originalError, options);
        pjson patch = pjson::parse(patchInput, patchError, options);
        if (!originalError.ok || !patchError.ok)
            return;

        const bool useJsonPatch = patch.isArray();
        pjson working = original;
        pjson::PatchError detailedError;
        const bool detailedOk = useJsonPatch ? working.applyPatch(patch, detailedError)
                                             : working.applyMergePatch(patch, detailedError);
        pjson_fuzz::require(detailedOk == detailedError.ok);

        pjson simple = original;
        const bool simpleOk =
            useJsonPatch ? simple.applyPatch(patch) : simple.applyMergePatch(patch);
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
        pjson::ParseOptions compactOptions = options;
        compactOptions.maxInputBytes = compact.size();
        pjson::ParseError reparsedError;
        pjson reparsed = pjson::parse(compact, reparsedError, compactOptions);
        pjson_fuzz::require(reparsedError.ok);
        pjson_fuzz::require(reparsed == working);

        const std::string pretty = working.toString(pjson::SerializeOptions::prettyPrinted());
        pjson::ParseOptions prettyOptions = options;
        prettyOptions.maxInputBytes = pretty.size();
        pjson::ParseError prettyError;
        pjson prettyParsed = pjson::parse(pretty, prettyError, prettyOptions);
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
