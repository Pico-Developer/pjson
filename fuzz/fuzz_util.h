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

#ifndef PJSON_FUZZ_UTIL_H
#define PJSON_FUZZ_UTIL_H

#include "pjson.h"
#include "pjson_parser.h"
#include "pjson_schema.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace pjson_fuzz {

    // Shared resource limits and invariant checks.

    // Caps raw fuzzer inputs so each target stays fast and allocation-bounded.
    const size_t kMaxInputBytes = 64U * 1024U;

    // Converts a violated cross-API property into a fuzzer-detectable crash.
    inline void require(bool condition) {
        if (!condition)
            std::abort();
    }

    // Parser configuration.

    // Returns a stable byte even when the input is shorter than the requested index.
    inline uint8_t pickByte(const uint8_t* data, size_t size, size_t index, uint8_t fallback) {
        return index < size ? data[index] : fallback;
    }

    // Builds a parser configuration while varying duplicate-key
    // policy and resource budgets across inputs.
    inline ByteDance::pJsonParser::Options parseOptionsVariant(const uint8_t* data, size_t size,
                                                               size_t offset = 0) {
        ByteDance::pJsonParser::Options options;
        switch (pickByte(data, size, offset, 0) % 3U) {
            case 0:
                options.duplicateKeys = ByteDance::pJsonParser::Options::RejectDuplicateKeys;
                break;
            case 1:
                options.duplicateKeys = ByteDance::pJsonParser::Options::KeepFirstDuplicate;
                break;
            default:
                options.duplicateKeys = ByteDance::pJsonParser::Options::KeepLastDuplicate;
                break;
        }

        static const int kDepthBudgets[] = {8, 32, 128, 512};
        static const size_t kNodeBudgets[] = {64U, 1024U, 8192U, 65536U};
        static const size_t kInputBudgets[] = {64U, 1024U, 16384U, kMaxInputBytes};

        options.maxDepth = kDepthBudgets[pickByte(data, size, offset + 1U, 1) % 4U];
        options.maxNodes = kNodeBudgets[pickByte(data, size, offset + 2U, 2) % 4U];
        options.maxInputBytes = kInputBudgets[pickByte(data, size, offset + 3U, 3) % 4U];
        return options;
    }

    // Schema validation gets its own bounded knobs so one input can drive both
    // parser and validator resource limits.
    inline ByteDance::pJsonSchemaValidator::Options
    boundedSchemaOptions(const uint8_t* data, size_t size, size_t offset = 0) {
        ByteDance::pJsonSchemaValidator::Options options;
        static const size_t kPatternBudgets[] = {32U, 64U, 256U, 1024U};
        static const size_t kSubjectBudgets[] = {128U, 512U, 4096U, 16384U};
        static const size_t kValidationDepths[] = {16U, 64U, 256U, 1024U};
        static const size_t kRefBudgets[] = {16U, 64U, 256U, 1024U};
        static const size_t kWorkBudgets[] = {256U, 4096U, 65536U, 1000000U};
        static const size_t kErrorBudgets[] = {1U, 8U, 32U, 100U};

        options.maxRegexPatternBytes = kPatternBudgets[pickByte(data, size, offset, 0) % 4U];
        options.maxRegexSubjectBytes = kSubjectBudgets[pickByte(data, size, offset + 1U, 1) % 4U];
        options.maxValidationDepth = kValidationDepths[pickByte(data, size, offset + 2U, 2) % 4U];
        options.maxRefResolutions = kRefBudgets[pickByte(data, size, offset + 3U, 3) % 4U];
        options.maxValidationWork = kWorkBudgets[pickByte(data, size, offset + 4U, 4) % 4U];
        options.maxErrors = kErrorBudgets[pickByte(data, size, offset + 5U, 5) % 4U];
        options.validateFormats = (pickByte(data, size, offset + 6U, 6) & 1U) != 0;
        return options;
    }

    inline ByteDance::pjson::PatchOptions patchOptionsVariant(const uint8_t* data, size_t size,
                                                              size_t offset = 0) {
        ByteDance::pjson::PatchOptions options;
        static const size_t kOperationBudgets[] = {1U, 8U, 64U, 10000U};
        static const size_t kNodeBudgets[] = {8U, 128U, 4096U, 1000000U};
        static const size_t kByteBudgets[] = {64U, 4096U, 65536U, 64U * 1024U * 1024U};
        static const size_t kWorkBudgets[] = {16U, 512U, 16384U, 1000000U};
        options.maxOperations = kOperationBudgets[pickByte(data, size, offset, 0) % 4U];
        options.maxClonedNodes = kNodeBudgets[pickByte(data, size, offset + 1U, 1) % 4U];
        options.maxClonedBytes = kByteBudgets[pickByte(data, size, offset + 2U, 2) % 4U];
        options.maxWork = kWorkBudgets[pickByte(data, size, offset + 3U, 3) % 4U];
        return options;
    }

    // Raw input adaptation.

    // Returns a non-null character pointer for empty input and preserves all other bytes.
    inline const char* bytes(const uint8_t* data, size_t size) {
        return size == 0 ? "" : reinterpret_cast<const char*>(data);
    }

    // Splits "left\nright" style inputs without requiring a checked-in framing token.
    inline void splitOnNewlineOrMidpoint(const std::string& input, std::string& first,
                                         std::string& second) {
        size_t split = input.find('\n');
        size_t secondStart = split;
        if (split == std::string::npos) {
            split = input.size() / 2U;
            secondStart = split;
        } else {
            secondStart = split + 1U;
        }
        first.assign(input.data(), split);
        second.assign(input.data() + secondStart, input.size() - secondStart);
    }

} // namespace pjson_fuzz

#endif // PJSON_FUZZ_UTIL_H
