// Copyright 2025 ByteDance Ltd. and/or its affiliates. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License");

#include "fuzz_util.h"

#include <cstddef>
#include <cstdint>
#include <string>

using ByteDance::pjson;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > pjson_fuzz::kMaxInputBytes)
        return 0;

    const std::string input(pjson_fuzz::bytes(data, size), size);
    std::string documentInput;
    std::string pointer;
    pjson_fuzz::splitOnNewlineOrMidpoint(input, documentInput, pointer);
    pjson::ParseError parseError;
    pjson document = pjson::parse(documentInput, parseError);
    if (!parseError.ok)
        return 0;

    const std::string before = document.toString();
    pjson::PointerError mutableError;
    pjson* mutableResult = document.findPointer(pointer, mutableError);
    const pjson& constDocument = document;
    pjson::PointerError constError;
    const pjson* constResult = constDocument.findPointer(pointer, constError);
    pjson_fuzz::require((mutableResult != nullptr) == (constResult != nullptr));
    pjson_fuzz::require(mutableError.ok == constError.ok);
    pjson_fuzz::require(mutableError.code == constError.code);
    pjson_fuzz::require(mutableError.tokenIndex == constError.tokenIndex);
    pjson_fuzz::require(mutableError.token == constError.token);
    pjson_fuzz::require(document.toString() == before);
    if (mutableResult != nullptr)
        pjson_fuzz::require(*mutableResult == *constResult);

    if (pointer.find('\0') == std::string::npos) {
        const pjson* cStringResult = constDocument.findPointer(pointer.c_str());
        pjson_fuzz::require((cStringResult != nullptr) == (constResult != nullptr));
    }
    return 0;
}
