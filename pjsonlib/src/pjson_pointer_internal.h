// SPDX-FileCopyrightText: 2026 ByteDance Ltd. and/or its affiliates
// SPDX-License-Identifier: Apache-2.0
#ifndef PRAVEENJSON_POINTER_INTERNAL_H
#define PRAVEENJSON_POINTER_INTERNAL_H

#include "pjson.h"

#include <cstddef>
#include <string>
#include <vector>

namespace ByteDance {
    namespace pjson_pointer_detail {
        enum PointerIndexResult { PointerIndexOk, PointerIndexInvalid, PointerIndexOverflow };

        void resetPointerError(pjson::PointerError& aError);
        bool failPointer(pjson::PointerError& aError, pjson::PointerError::Code aCode,
                         const std::string& aPointer, size_t aTokenIndex, const std::string& aToken,
                         const char* aMessage);
        PointerIndexResult parsePointerIndex(const std::string& aToken, size_t& aIndex);
        bool decodePointer(const std::string& aPointer, std::vector<std::string>& aTokens,
                           pjson::PointerError& aError);
        const pjson* resolvePointerTokens(const pjson& aRoot,
                                          const std::vector<std::string>& aTokens, size_t aCount,
                                          const std::string& aPointer, pjson::PointerError& aError);
    } // namespace pjson_pointer_detail
} // namespace ByteDance

#endif // PRAVEENJSON_POINTER_INTERNAL_H
