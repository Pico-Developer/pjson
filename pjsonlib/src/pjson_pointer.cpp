// SPDX-FileCopyrightText: 2026 ByteDance Ltd. and/or its affiliates
// SPDX-License-Identifier: Apache-2.0
//
// JSON Pointer decoding and non-vivifying traversal.

#include "pjson_internal.h"
#include "pjson_pointer_internal.h"

#include <limits>
#include <new>
#include <utility>

namespace ByteDance {
    pjson::PointerError::PointerError()
            : ok(true)
            , code(Ok)
            , tokenIndex(0) {}

    namespace pjson_pointer_detail {
        void resetPointerError(pjson::PointerError& error) {
            error.ok = true;
            error.code = pjson::PointerError::Ok;
            error.pointer.clear();
            error.tokenIndex = 0;
            error.token.clear();
            error.message.clear();
        }

        bool failPointer(pjson::PointerError& error, pjson::PointerError::Code code,
                         const std::string& pointer, size_t tokenIndex, const std::string& token,
                         const char* message) {
            error.ok = false;
            error.code = code;
            error.pointer = pointer;
            error.tokenIndex = tokenIndex;
            error.token = token;
            error.message = message;
            return false;
        }

        PointerIndexResult parsePointerIndex(const std::string& token, size_t& index) {
            if (token.empty() || (token.size() > 1 && token[0] == '0'))
                return PointerIndexInvalid;

            size_t value = 0;
            for (size_t i = 0; i < token.size(); ++i) {
                const unsigned char ch = static_cast<unsigned char>(token[i]);
                if (ch < static_cast<unsigned char>('0') || ch > static_cast<unsigned char>('9'))
                    return PointerIndexInvalid;
                const size_t digit = static_cast<size_t>(ch - static_cast<unsigned char>('0'));
                if (value > (std::numeric_limits<size_t>::max() - digit) / size_t(10))
                    return PointerIndexOverflow;
                value = value * size_t(10) + digit;
            }
            index = value;
            return PointerIndexOk;
        }

        bool decodePointer(const std::string& pointer, std::vector<std::string>& tokens,
                           pjson::PointerError& error) {
            resetPointerError(error);
            tokens.clear();
            if (pointer.empty())
                return true;
            if (pointer[0] != '/')
                return failPointer(error, pjson::PointerError::InvalidSyntax, pointer, 0,
                                   std::string(), "JSON Pointer must be empty or begin with '/'");

            size_t tokenIndex = 0;
            size_t tokenStart = 1;
            while (true) {
                const size_t slash = pointer.find('/', tokenStart);
                const size_t tokenEnd = slash == std::string::npos ? pointer.size() : slash;
                std::string decoded;
                decoded.reserve(tokenEnd - tokenStart);
                for (size_t i = tokenStart; i < tokenEnd; ++i) {
                    const char ch = pointer[i];
                    if (ch != '~') {
                        decoded += ch;
                        continue;
                    }
                    if (i + 1 >= tokenEnd || (pointer[i + 1] != '0' && pointer[i + 1] != '1')) {
                        return failPointer(error, pjson::PointerError::InvalidEscape, pointer,
                                           tokenIndex,
                                           pointer.substr(tokenStart, tokenEnd - tokenStart),
                                           "JSON Pointer token contains an invalid '~' escape");
                    }
                    decoded += pointer[i + 1] == '0' ? '~' : '/';
                    ++i;
                }
                tokens.push_back(std::move(decoded));
                if (slash == std::string::npos)
                    break;
                tokenStart = slash + 1;
                ++tokenIndex;
            }
            return true;
        }

        const pjson* resolvePointerTokens(const pjson& root, const std::vector<std::string>& tokens,
                                          size_t count, const std::string& pointer,
                                          pjson::PointerError& error) {
            const pjson* current = &root;
            for (size_t i = 0; i < count; ++i) {
                const std::string& token = tokens[i];
                if (current->isObject()) {
                    const pjson* child = current->find(token);
                    if (child == nullptr) {
                        failPointer(error, pjson::PointerError::MissingTarget, pointer, i, token,
                                    "JSON Pointer object member does not exist");
                        return nullptr;
                    }
                    current = child;
                    continue;
                }
                if (current->isArray()) {
                    if (token == "-") {
                        failPointer(error, pjson::PointerError::AppendTokenNotAllowed, pointer, i,
                                    token, "the '-' token is only valid for JSON Patch add");
                        return nullptr;
                    }
                    size_t index = 0;
                    const PointerIndexResult result = parsePointerIndex(token, index);
                    if (result == PointerIndexInvalid) {
                        failPointer(error, pjson::PointerError::InvalidArrayIndex, pointer, i,
                                    token, "JSON Pointer array index is not canonical decimal");
                        return nullptr;
                    }
                    if (result == PointerIndexOverflow || index >= current->size()) {
                        failPointer(error, pjson::PointerError::ArrayIndexOutOfRange, pointer, i,
                                    token, "JSON Pointer array index is out of range");
                        return nullptr;
                    }
                    current = current->findIndex(index);
                    continue;
                }
                failPointer(error, pjson::PointerError::ExpectedContainer, pointer, i, token,
                            "JSON Pointer traversal reached a non-container value");
                return nullptr;
            }
            return current;
        }
    } // namespace pjson_pointer_detail

    std::string pjson::escapePointerToken(const std::string& token) {
        std::string escaped;
        escaped.reserve(token.size());
        for (size_t i = 0; i < token.size(); ++i) {
            if (token[i] == '~')
                escaped += "~0";
            else if (token[i] == '/')
                escaped += "~1";
            else
                escaped += token[i];
        }
        return escaped;
    }

    const pjson* pjson::findPointer(const std::string& pointer, PointerError& error) const {
        using namespace pjson_pointer_detail;
        try {
            std::vector<std::string> tokens;
            if (!decodePointer(pointer, tokens, error))
                return nullptr;
            return resolvePointerTokens(*this, tokens, tokens.size(), pointer, error);
        } catch (const std::bad_alloc&) {
            try {
                failPointer(error, PointerError::AllocationFailure, std::string(), 0, std::string(),
                            "JSON Pointer ran out of memory");
            } catch (...) {
                error.ok = false;
                error.code = PointerError::AllocationFailure;
            }
            return nullptr;
        } catch (...) {
            try {
                failPointer(error, PointerError::InternalError, std::string(), 0, std::string(),
                            "JSON Pointer failed with an internal exception");
            } catch (...) {
                error.ok = false;
                error.code = PointerError::InternalError;
            }
            return nullptr;
        }
    }

    pjson* pjson::findPointer(const std::string& pointer, PointerError& error) {
        return const_cast<pjson*>(static_cast<const pjson*>(this)->findPointer(pointer, error));
    }

    const pjson* pjson::findPointer(const std::string& pointer) const {
        PointerError error;
        return findPointer(pointer, error);
    }

    pjson* pjson::findPointer(const std::string& pointer) {
        return const_cast<pjson*>(static_cast<const pjson*>(this)->findPointer(pointer));
    }

    const pjson* pjson::findPointer(const char* pointer, PointerError& error) const {
        using namespace pjson_pointer_detail;
        try {
            if (pointer != nullptr)
                return findPointer(std::string(pointer), error);
            resetPointerError(error);
            failPointer(error, PointerError::InvalidSyntax, std::string(), 0, std::string(),
                        "JSON Pointer input is null");
            return nullptr;
        } catch (const std::bad_alloc&) {
            error.ok = false;
            error.code = PointerError::AllocationFailure;
            return nullptr;
        } catch (...) {
            error.ok = false;
            error.code = PointerError::InternalError;
            return nullptr;
        }
    }

    pjson* pjson::findPointer(const char* pointer, PointerError& error) {
        return const_cast<pjson*>(static_cast<const pjson*>(this)->findPointer(pointer, error));
    }

    const pjson* pjson::findPointer(const char* pointer) const {
        PointerError error;
        return findPointer(pointer, error);
    }

    pjson* pjson::findPointer(const char* pointer) {
        return const_cast<pjson*>(static_cast<const pjson*>(this)->findPointer(pointer));
    }
} // namespace ByteDance
