//
// Copyright 2025 ByteDance Ltd. and/or its affiliates. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License");
//
#include "pjson_schema_util.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <vector>

namespace ByteDance {
    namespace pjson_schema_detail {
        std::string strOf(const pjson& value) {
            std::string result;
            value.tryGet(result);
            return result;
        }

        bool boolOf(const pjson& value) {
            bool result = false;
            value.tryGet(result);
            return result;
        }

        std::string numberText(const pjson& value) {
            int64_t i = 0;
            if (value.isInt() && value.tryGet(i))
                return std::to_string(i);
            uint64_t u = 0;
            if (value.isUInt() && value.tryGet(u))
                return std::to_string(u);
            pjson::SerializeOptions options;
            options.nonFinite = pjson::SerializeOptions::NonFiniteToString;
            return value.toString(options);
        }

        double numberAsDouble(const pjson& value) {
            double result = 0.0;
            value.tryGet(result);
            return result;
        }

        int utf8Len(const char* source, size_t position, size_t end) {
            const unsigned char c0 = static_cast<unsigned char>(source[position]);
            int count;
            uint32_t codePoint;
            uint32_t minimum;
            if (c0 < 0x80)
                return 1;
            if ((c0 & 0xE0) == 0xC0) {
                count = 2;
                codePoint = c0 & 0x1F;
                minimum = 0x80;
            } else if ((c0 & 0xF0) == 0xE0) {
                count = 3;
                codePoint = c0 & 0x0F;
                minimum = 0x800;
            } else if ((c0 & 0xF8) == 0xF0) {
                count = 4;
                codePoint = c0 & 0x07;
                minimum = 0x10000;
            } else {
                return 0;
            }
            if (position + static_cast<size_t>(count) > end)
                return 0;
            for (int i = 1; i < count; ++i) {
                const unsigned char continuation =
                    static_cast<unsigned char>(source[position + static_cast<size_t>(i)]);
                if ((continuation & 0xC0) != 0x80)
                    return 0;
                codePoint = (codePoint << 6) | (continuation & 0x3F);
            }
            if (codePoint < minimum || codePoint > 0x10FFFF ||
                (codePoint >= 0xD800 && codePoint <= 0xDFFF))
                return 0;
            return count;
        }

        std::string typeName(const pjson& node) {
            if (node.isNull())
                return "null";
            if (node.isString())
                return "string";
            if (node.isInteger())
                return "integer";
            if (node.isDouble())
                return "number";
            if (node.isBool())
                return "boolean";
            if (node.isArray())
                return "array";
            if (node.isObject())
                return "object";
            return "unknown";
        }

        bool typeMatches(const pjson& node, const std::string& type) {
            if (type == "null")
                return node.isNull();
            if (type == "string")
                return node.isString();
            if (type == "boolean")
                return node.isBool();
            if (type == "array")
                return node.isArray();
            if (type == "object")
                return node.isObject();
            if (type == "number")
                return node.isNumber();
            if (type == "integer") {
                if (node.isInteger())
                    return true;
                double number = 0.0;
                return node.isDouble() && node.tryGet(number) && std::isfinite(number) &&
                       std::floor(number) == number;
            }
            return false;
        }

        bool isSafeRegex(const std::string& pattern) {
            bool escaped = false;
            bool inClass = false;
            int groups = 0;
            int quantifiers = 0;
            struct Group {
                bool hasQuantifier;
                bool hasAlternation;
            };
            std::vector<Group> stack;
            for (size_t i = 0; i < pattern.size(); ++i) {
                const char c = pattern[i];
                if (escaped) {
                    if (c >= '1' && c <= '9')
                        return false;
                    escaped = false;
                    continue;
                }
                if (c == '\\') {
                    escaped = true;
                    continue;
                }
                if (c == '[') {
                    inClass = true;
                    continue;
                }
                if (c == ']' && inClass) {
                    inClass = false;
                    continue;
                }
                if (inClass)
                    continue;
                if (c == '(') {
                    if (++groups > 16)
                        return false;
                    stack.push_back(Group{false, false});
                } else if (c == '|') {
                    return false;
                } else if (c == '*' || c == '+' || c == '?' || c == '{') {
                    if (++quantifiers > 1)
                        return false;
                    if (c == '{') {
                        size_t j = i + 1;
                        size_t first = 0;
                        size_t second = 0;
                        bool haveFirst = false;
                        bool haveSecond = false;
                        while (j < pattern.size() && pattern[j] >= '0' && pattern[j] <= '9') {
                            haveFirst = true;
                            if (first > 1000)
                                return false;
                            first = first * 10 + static_cast<size_t>(pattern[j] - '0');
                            ++j;
                        }
                        if (j < pattern.size() && pattern[j] == ',') {
                            ++j;
                            while (j < pattern.size() && pattern[j] >= '0' && pattern[j] <= '9') {
                                haveSecond = true;
                                if (second > 1000)
                                    return false;
                                second = second * 10 + static_cast<size_t>(pattern[j] - '0');
                                ++j;
                            }
                        }
                        if ((haveFirst && first > 1000) || (haveSecond && second > 1000))
                            return false;
                    }
                    if (!stack.empty())
                        stack.back().hasQuantifier = true;
                } else if (c == ')' && !stack.empty()) {
                    const Group closed = stack.back();
                    stack.pop_back();
                    const size_t next = i + 1;
                    const bool quantified =
                        next < pattern.size() && (pattern[next] == '*' || pattern[next] == '+' ||
                                                  pattern[next] == '?' || pattern[next] == '{');
                    if (quantified && (closed.hasQuantifier || closed.hasAlternation))
                        return false;
                    if (!stack.empty())
                        stack.back().hasQuantifier =
                            stack.back().hasQuantifier || quantified || closed.hasQuantifier;
                }
            }
            return true;
        }

        namespace {
            struct ExactDecimal {
                uint64_t coefficient;
                int exponent10;
            };

            uint64_t magnitudeOf(int64_t value) {
                return value < 0 ? uint64_t(-(value + 1)) + uint64_t(1) : uint64_t(value);
            }

            bool decimalFromText(const std::string& text, ExactDecimal& result) {
                size_t position = 0;
                if (position < text.size() && (text[position] == '+' || text[position] == '-'))
                    ++position;
                uint64_t coefficient = 0;
                int fractionDigits = 0;
                bool seenDigit = false;
                bool afterPoint = false;
                while (position < text.size() && text[position] != 'e' && text[position] != 'E') {
                    const char ch = text[position++];
                    if (ch == '.' && !afterPoint) {
                        afterPoint = true;
                        continue;
                    }
                    if (ch < '0' || ch > '9')
                        return false;
                    const uint64_t digit = static_cast<uint64_t>(ch - '0');
                    if (coefficient > (std::numeric_limits<uint64_t>::max() - digit) / uint64_t(10))
                        return false;
                    coefficient = coefficient * uint64_t(10) + digit;
                    if (afterPoint)
                        ++fractionDigits;
                    seenDigit = true;
                }
                int explicitExponent = 0;
                if (position < text.size()) {
                    ++position;
                    bool negative = false;
                    if (position < text.size() &&
                        (text[position] == '+' || text[position] == '-')) {
                        negative = text[position] == '-';
                        ++position;
                    }
                    if (position == text.size())
                        return false;
                    while (position < text.size()) {
                        const char ch = text[position++];
                        if (ch < '0' || ch > '9' || explicitExponent > 10000)
                            return false;
                        explicitExponent = explicitExponent * 10 + (ch - '0');
                    }
                    if (negative)
                        explicitExponent = -explicitExponent;
                }
                if (!seenDigit)
                    return false;
                if (coefficient == 0) {
                    result = ExactDecimal{0, 0};
                    return true;
                }
                int exponent = explicitExponent - fractionDigits;
                while (coefficient % uint64_t(10) == 0) {
                    coefficient /= uint64_t(10);
                    ++exponent;
                }
                result = ExactDecimal{coefficient, exponent};
                return true;
            }

            bool decimalFromNumber(const pjson& value, ExactDecimal& result) {
                if (value.isInteger()) {
                    uint64_t unsignedValue = 0;
                    int64_t signedValue = 0;
                    result.coefficient =
                        value.isUInt() ? (value.tryGet(unsignedValue), unsignedValue)
                                       : (value.tryGet(signedValue), magnitudeOf(signedValue));
                    result.exponent10 = 0;
                    while (result.coefficient != 0 && result.coefficient % uint64_t(10) == 0) {
                        result.coefficient /= uint64_t(10);
                        ++result.exponent10;
                    }
                    return true;
                }
                double number = 0.0;
                return value.isDouble() && value.tryGet(number) && std::isfinite(number) &&
                       decimalFromText(numberText(value), result);
            }
        } // namespace

        std::string formatNumber(const pjson& value) {
            return numberText(value);
        }

        bool schemaSize(const pjson& value, size_t& result, bool& aboveRange) {
            aboveRange = false;
            if (value.isUInt()) {
                uint64_t magnitude = 0;
                value.tryGet(magnitude);
                if (magnitude > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
                    aboveRange = true;
                    return true;
                }
                result = static_cast<size_t>(magnitude);
                return true;
            }
            if (value.isInt()) {
                int64_t integer = 0;
                value.tryGet(integer);
                if (integer < 0)
                    return false;
                const uint64_t magnitude = static_cast<uint64_t>(integer);
                if (magnitude > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
                    aboveRange = true;
                    return true;
                }
                result = static_cast<size_t>(magnitude);
                return true;
            }
            double number = 0.0;
            if (!value.isDouble() || !value.tryGet(number) || !std::isfinite(number) ||
                number < 0.0 || std::floor(number) != number)
                return false;
            const double exclusiveUpper = std::ldexp(1.0, std::numeric_limits<size_t>::digits);
            if (number >= exclusiveUpper) {
                aboveRange = true;
                return true;
            }
            result = static_cast<size_t>(number);
            return true;
        }

        bool isExactMultiple(const pjson& value, const pjson& divisor) {
            if (numberAsDouble(divisor) <= 0.0)
                return true;
            if (value.isInt() && divisor.isInt()) {
                int64_t valueInteger = 0, divisorInteger = 0;
                value.tryGet(valueInteger);
                divisor.tryGet(divisorInteger);
                return magnitudeOf(valueInteger) % magnitudeOf(divisorInteger) == 0;
            }
            ExactDecimal valueDecimal = {0, 0};
            ExactDecimal divisorDecimal = {0, 0};
            if (!decimalFromNumber(divisor, divisorDecimal) || divisorDecimal.coefficient == 0)
                return true;
            if (!decimalFromNumber(value, valueDecimal))
                return false;
            if (valueDecimal.coefficient == 0)
                return true;
            const int shift = valueDecimal.exponent10 - divisorDecimal.exponent10;
            if (shift >= 0) {
                uint64_t reduced = divisorDecimal.coefficient;
                int remainingTwos = shift;
                int remainingFives = shift;
                while (remainingTwos > 0 && reduced % uint64_t(2) == 0) {
                    reduced /= uint64_t(2);
                    --remainingTwos;
                }
                while (remainingFives > 0 && reduced % uint64_t(5) == 0) {
                    reduced /= uint64_t(5);
                    --remainingFives;
                }
                return valueDecimal.coefficient % reduced == 0;
            }
            if (valueDecimal.coefficient % divisorDecimal.coefficient != 0)
                return false;
            uint64_t quotient = valueDecimal.coefficient / divisorDecimal.coefficient;
            int decimalPlaces = -shift;
            while (decimalPlaces > 0 && quotient % uint64_t(10) == 0) {
                quotient /= uint64_t(10);
                --decimalPlaces;
            }
            return decimalPlaces == 0;
        }
    } // namespace pjson_schema_detail
} // namespace ByteDance
