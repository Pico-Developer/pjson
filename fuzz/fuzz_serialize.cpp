// Copyright 2025 ByteDance Ltd. and/or its affiliates. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License");

#include "fuzz_util.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>

using ByteDance::pjson;

namespace {
    void exercise(const pjson& value, const uint8_t* data, size_t size, size_t offset) {
        pjson::SerializeOptions options;
        options.pretty = (pjson_fuzz::pickByte(data, size, offset, 0) & 1U) != 0;
        options.indentWidth = pjson_fuzz::pickByte(data, size, offset + 1U, 2) % 9U;
        options.indentCharacter =
            (pjson_fuzz::pickByte(data, size, offset + 2U, 0) & 1U) != 0 ? '\t' : ' ';
        options.escapeNonAscii = (pjson_fuzz::pickByte(data, size, offset + 3U, 0) & 1U) != 0;
        options.keyOrder = (pjson_fuzz::pickByte(data, size, offset + 4U, 0) & 1U) != 0
                               ? pjson::SerializeOptions::DescendingKeys
                               : pjson::SerializeOptions::AscendingKeys;
        options.nonFinite = static_cast<pjson::SerializeOptions::NonFinitePolicy>(
            pjson_fuzz::pickByte(data, size, offset + 5U, 0) % 3U);
        const size_t limits[] = {0U, 1U, 32U, 4096U, pjson_fuzz::kMaxInputBytes};
        options.maxOutputBytes = limits[pjson_fuzz::pickByte(data, size, offset + 6U, 0) % 5U];

        std::string output = "preserved";
        pjson::SerializeError error;
        const bool ok = value.toString(output, error, options);
        pjson_fuzz::require(ok == (error.code == pjson::SerializeError::None));
        if (!ok) {
            pjson_fuzz::require(output == "preserved");
            return;
        }

        std::ostringstream stream;
        pjson::SerializeError streamError;
        pjson_fuzz::require(value.write(stream, streamError, options));
        pjson_fuzz::require(streamError.code == pjson::SerializeError::None);
        pjson_fuzz::require(stream.str() == output);
        pjson::ParseError parseError;
        pjson roundTrip = pjson::parse(output, parseError);
        pjson_fuzz::require(parseError.ok);
        if (value.isDouble()) {
            double number = 0.0;
            value.tryGet(number);
            if (number != number || number == std::numeric_limits<double>::infinity() ||
                number == -std::numeric_limits<double>::infinity())
                return;
        }
        pjson_fuzz::require(roundTrip == value);
    }
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > pjson_fuzz::kMaxInputBytes)
        return 0;
    pjson::ParseError parseError;
    pjson parsed = pjson::parse(pjson_fuzz::bytes(data, size), size, parseError);
    if (parseError.ok)
        exercise(parsed, data, size, 0);

    pjson arbitraryString;
    arbitraryString = std::string(pjson_fuzz::bytes(data, size), size);
    exercise(arbitraryString, data, size, 8);

    const double nonFiniteValues[] = {std::numeric_limits<double>::infinity(),
                                      -std::numeric_limits<double>::infinity(),
                                      std::numeric_limits<double>::quiet_NaN()};
    for (size_t i = 0; i < size_t(3); ++i) {
        pjson nonFinite;
        nonFinite = nonFiniteValues[i];
        exercise(nonFinite, data, size, size_t(16) + i * size_t(8));
    }
    return 0;
}
