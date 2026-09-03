// Copyright 2025 ByteDance Ltd. and/or its affiliates. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License");

#include "fuzz_util.h"

#include <cstddef>
#include <cstdint>
#include <string>

using ByteDance::pjson;
using ByteDance::pJsonParser;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > pjson_fuzz::kMaxInputBytes)
        return 0;

    const std::string input(pjson_fuzz::bytes(data, size), size);
    std::string documentInput;
    std::string patchInput;
    pjson_fuzz::splitOnNewlineOrMidpoint(input, documentInput, patchInput);

    pJsonParser::Error documentError;
    pJsonParser::Error patchError;
    pjson document = pJsonParser().parse(documentInput, documentError);
    pjson patch = pJsonParser().parse(patchInput, patchError);
    if (!documentError.ok || !patchError.ok)
        return 0;

    for (size_t variant = 0; variant < 2; ++variant) {
        const pjson::PatchOptions options =
            pjson_fuzz::patchOptionsVariant(data, size, variant * size_t(4));
        pjson detailed = document;
        pjson::PatchError error;
        const bool detailedOk = detailed.applyMergePatch(patch, error, options);
        pjson_fuzz::require(detailedOk == error.ok);

        pjson simple = document;
        const bool simpleOk = simple.applyMergePatch(patch, options);
        pjson_fuzz::require(simpleOk == detailedOk);
        pjson_fuzz::require(simple == detailed);
        if (!detailedOk)
            pjson_fuzz::require(detailed == document);
        else {
            pJsonParser::Error roundTripError;
            pjson roundTrip = pJsonParser().parse(detailed.toString(), roundTripError);
            pjson_fuzz::require(roundTripError.ok);
            pjson_fuzz::require(roundTrip == detailed);
        }
    }
    return 0;
}
