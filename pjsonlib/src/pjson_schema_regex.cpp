// SPDX-FileCopyrightText: 2026 ByteDance Ltd. and/or its affiliates
// SPDX-License-Identifier: Apache-2.0
#include "pjson_schema_regex.h"

#include <srell/srell.hpp>

#include <new>

namespace ByteDance {
    namespace pjson_schema_detail {
        namespace {
            bool isAsciiLetter(char value) {
                return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
            }

            bool validEcmaSyntaxSubset(const std::string& pattern) {
                bool inClass = false;
                size_t groups = 0;
                for (size_t i = 0; i < pattern.size(); ++i) {
                    const char value = pattern[i];
                    if (value == '\\') {
                        ++i;
                        if (i >= pattern.size())
                            return false;
                        const char escaped = pattern[i];
                        if (escaped == 'c') {
                            ++i;
                            if (i >= pattern.size() || !isAsciiLetter(pattern[i]))
                                return false;
                        } else if (escaped == 'p' || escaped == 'P') {
                            ++i;
                            if (i >= pattern.size() || pattern[i] != '{')
                                return false;
                            const size_t close = pattern.find('}', i + 1);
                            if (close == std::string::npos || close == i + 1)
                                return false;
                            i = close;
                        } else if (isAsciiLetter(escaped) &&
                                   std::string("bBfnrtvxdDsSwWuk").find(escaped) ==
                                       std::string::npos) {
                            return false;
                        }
                        continue;
                    }
                    if (value == '[' && !inClass) {
                        inClass = true;
                        continue;
                    }
                    if (value == ']' && inClass) {
                        inClass = false;
                        continue;
                    }
                    if (inClass)
                        continue;
                    if (value == '(') {
                        ++groups;
                        if (i + 2 < pattern.size() && pattern[i + 1] == '?' &&
                            (pattern[i + 2] == 'P' || pattern[i + 2] == '#' ||
                             pattern[i + 2] == 'i' || pattern[i + 2] == 'm' ||
                             pattern[i + 2] == 's' || pattern[i + 2] == '-'))
                            return false;
                    } else if (value == ')') {
                        if (groups == 0)
                            return false;
                        --groups;
                    }
                }
                if (inClass || groups != 0)
                    return false;
                return true;
            }

            bool hasUnsupportedGlobalModifiers(const std::string& pattern) {
                for (size_t i = 0; i + 3 < pattern.size(); ++i) {
                    if (pattern[i] == '(' && pattern[i + 1] == '?' &&
                        (pattern[i + 2] == 'i' || pattern[i + 2] == 'm' || pattern[i + 2] == 's' ||
                         pattern[i + 2] == '-'))
                        return true;
                }
                return false;
            }
        } // namespace

        struct EcmaRegex::Impl {
            srell::u8regex expression;
        };

        EcmaRegex::EcmaRegex()
                : _impl(new Impl()) {}

        EcmaRegex::~EcmaRegex() {
            delete _impl;
        }

        bool EcmaRegex::compile(const std::string& pattern) {
            if (!validEcmaSyntaxSubset(pattern) || hasUnsupportedGlobalModifiers(pattern))
                return false;
            try {
                _impl->expression.assign(pattern);
                return true;
            } catch (const srell::regex_error&) {
                return false;
            }
        }

        EcmaRegex::Result EcmaRegex::search(const std::string& subject) const {
            try {
                return srell::regex_search(subject, _impl->expression) ? Match : NoMatch;
            } catch (const srell::regex_error& error) {
                return error.code() == srell::regex_constants::error_complexity ||
                               error.code() == srell::regex_constants::error_stack
                           ? WorkLimit
                           : Invalid;
            }
        }

        bool validEcmaRegex(const std::string& pattern) {
            EcmaRegex expression;
            return expression.compile(pattern);
        }
    } // namespace pjson_schema_detail
} // namespace ByteDance
