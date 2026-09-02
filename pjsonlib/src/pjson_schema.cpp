//
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
//
//===----------------------------------------------------------------------===//
// pjson_schema.cpp — standalone JSON Schema validation.
//
// Implements ByteDance::pJsonSchemaValidator. This translation unit is a pure
// consumer of pjson's PUBLIC API (pjson.h): it never touches pjson's private
// storage and does not include pjson_internal.h. That keeps the schema module
// fully decoupled from the DOM layout, so future DOM changes cannot silently
// alter validation behavior.
//
// This is a documented JSON Schema subset, not a complete draft implementation.
//===----------------------------------------------------------------------===//
#include "pjson_schema.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <map>
#include <new>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace ByteDance;

namespace {

    typedef pJsonSchemaValidator::Error SchemaError;
    typedef pJsonSchemaValidator::Options Options;

    const char kDocumentedSubsetDialect[] = "urn:bytedance:pjson:schema:documented-subset:2";
    const char kDocumentedSubsetVocabulary[] =
        "urn:bytedance:pjson:schema:vocabulary:documented-subset:2";

    // Recursive validation still uses native recursion for applicator keywords.
    // Keep its logical depth below a conservative stack-safe ceiling even when a
    // caller requests a larger value.
    const size_t kSchemaValidationDepthHardLimit = 64;

    //===------------------------------------------------------------------===//
    // Public-API accessors
    //
    // These small helpers express everything the validator needs to read from a
    // pjson value using only the public interface.
    //===------------------------------------------------------------------===//

    // Copies a string value (schema keyword strings and instance strings are
    // small relative to the work already charged for visiting them).
    std::string strOf(const pjson& value) {
        std::string result;
        value.tryGet(result);
        return result;
    }

    // Reads a stored boolean, defaulting to false for non-booleans.
    bool boolOf(const pjson& value) {
        bool result = false;
        value.tryGet(result);
        return result;
    }

    // Canonical decimal/finite text for a numeric node, used both for diagnostics
    // and for exact multipleOf decimal parsing. Non-finite doubles are rendered
    // as sentinel strings so this never throws on a programmatically built value.
    std::string numberText(const pjson& value) {
        int64_t i = 0;
        if (value.isInt() && value.tryGet(i))
            return std::to_string(i);
        uint64_t u = 0;
        if (value.isUInt() && value.tryGet(u))
            return std::to_string(u);
        pjson::SerializeOptions opts;
        opts.nonFinite = pjson::SerializeOptions::NonFiniteToString;
        return value.toString(opts);
    }

    // Number as double via the public widening read (covers int/uint/double).
    double numberAsDouble(const pjson& value) {
        double d = 0.0;
        value.tryGet(d);
        return d;
    }

    // Reimplements UTF-8 code-point measurement locally so this TU needs no
    // pjson internals. Returns the byte length of the sequence at pos, or 0 for
    // an invalid/overlong/surrogate encoding.
    int utf8Len(const char* src, size_t pos, size_t end) {
        const unsigned char c0 = static_cast<unsigned char>(src[pos]);
        int n;
        uint32_t cp;
        uint32_t lo;
        if (c0 < 0x80)
            return 1;
        else if ((c0 & 0xE0) == 0xC0) {
            n = 2;
            cp = c0 & 0x1F;
            lo = 0x80;
        } else if ((c0 & 0xF0) == 0xE0) {
            n = 3;
            cp = c0 & 0x0F;
            lo = 0x800;
        } else if ((c0 & 0xF8) == 0xF0) {
            n = 4;
            cp = c0 & 0x07;
            lo = 0x10000;
        } else
            return 0;
        if (pos + static_cast<size_t>(n) > end)
            return 0;
        for (int k = 1; k < n; ++k) {
            const unsigned char ck = static_cast<unsigned char>(src[pos + k]);
            if ((ck & 0xC0) != 0x80)
                return 0;
            cp = (cp << 6) | (ck & 0x3F);
        }
        if (cp < lo || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return 0;
        return n;
    }

    // The JSON Schema type name for a value.
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

    // Implements the "type" keyword. "number" accepts integers too; "integer"
    // accepts a whole-valued double (e.g. 2.0) as JSON Schema does.
    bool typeMatches(const pjson& node, const std::string& typeText) {
        if (typeText == "null")
            return node.isNull();
        if (typeText == "string")
            return node.isString();
        if (typeText == "boolean")
            return node.isBool();
        if (typeText == "array")
            return node.isArray();
        if (typeText == "object")
            return node.isObject();
        if (typeText == "number")
            return node.isNumber();
        if (typeText == "integer") {
            if (node.isInteger())
                return true;
            if (node.isDouble()) {
                double d = 0.0;
                node.tryGet(d);
                return std::isfinite(d) && std::floor(d) == d;
            }
            return false;
        }
        return false; // unknown type name never matches
    }

    // Appends "/token" to a JSON Pointer path, escaping '~' and '/' per RFC 6901.
    std::string pointerAppend(const std::string& base, const std::string& token) {
        std::string escaped;
        escaped.reserve(token.size());
        for (char c : token) {
            if (c == '~')
                escaped += "~0";
            else if (c == '/')
                escaped += "~1";
            else
                escaped += c;
        }
        return base + "/" + escaped;
    }

    // Conservative single-pass screen for constructs that are especially prone to
    // catastrophic backtracking in std::regex. Fail-closed: unrestricted
    // ECMAScript regex remains available through Options::trustedRegex().
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
                    return false; // backreference
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
                Group g = {false, false};
                stack.push_back(g);
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
                Group closed = stack.back();
                stack.pop_back();
                size_t next = i + 1;
                bool quantified =
                    next < pattern.size() && (pattern[next] == '*' || pattern[next] == '+' ||
                                              pattern[next] == '?' || pattern[next] == '{');
                if (quantified && (closed.hasQuantifier || closed.hasAlternation))
                    return false;
                if (!stack.empty()) {
                    stack.back().hasQuantifier =
                        stack.back().hasQuantifier || quantified || closed.hasQuantifier;
                    stack.back().hasAlternation =
                        stack.back().hasAlternation || closed.hasAlternation;
                }
            }
        }
        return true;
    }

    //===------------------------------------------------------------------===//
    // Regex cache, run context, and diagnostic sink
    //===------------------------------------------------------------------===//

    // One compiled schema regex or a cached policy/syntax rejection.
    struct RegexCacheEntry {
        enum State { Uninitialized, Ready, PatternTooLarge, UnsafePattern, InvalidPattern };
        State state;
        std::regex expression;
        RegexCacheEntry()
                : state(Uninitialized) {}
    };

    struct SchemaResource {
        const pjson* root;
        std::string baseUri;
        SchemaResource()
                : root(nullptr) {}
        SchemaResource(const pjson* aRoot, const std::string& aBase)
                : root(aRoot)
                , baseUri(aBase) {}
    };

    struct SchemaTarget {
        const pjson* schema;
        const pjson* resourceRoot;
        std::string baseUri;
        SchemaTarget()
                : schema(nullptr)
                , resourceRoot(nullptr) {}
        SchemaTarget(const pjson* aSchema, const pjson* aResourceRoot, const std::string& aBase)
                : schema(aSchema)
                , resourceRoot(aResourceRoot)
                , baseUri(aBase) {}
    };

    struct ResolvedDocument {
        std::string requestedUri;
        pjson schema;
        explicit ResolvedDocument(const std::string& aUri)
                : requestedUri(aUri) {}
    };

    struct CompiledSchemaIndex {
        std::deque<ResolvedDocument> documents;
        std::map<std::string, SchemaResource> resources;
        std::map<std::string, SchemaTarget> anchors;
        std::map<std::string, SchemaTarget> dynamicAnchors;
        std::map<const pjson*, SchemaTarget> nodeTargets;
        std::set<std::string> pendingDocuments;
        std::set<std::string> failedDocuments;
        size_t resolvedBytes;
        size_t workUsed;

        CompiledSchemaIndex()
                : resolvedBytes(0)
                , workUsed(0) {}
    };

    struct SchemaAnnotations {
        std::set<std::string> properties;
        std::set<size_t> items;

        void merge(const SchemaAnnotations& other) {
            properties.insert(other.properties.begin(), other.properties.end());
            items.insert(other.items.begin(), other.items.end());
        }
    };

    // Mutable limits and recursion state shared by one validation run.
    struct ValidationCtx {
        const pjson& rootSchema;
        const Options& options;
        const CompiledSchemaIndex& compiled;
        std::vector<SchemaError>* publicErrors;
        size_t depth;
        size_t refResolutions;
        size_t workUsed;
        size_t errorsUsed;
        size_t publicErrorStart;
        bool aborted;
        std::vector<std::pair<const pjson*, const pjson*>> activeRefs;
        std::map<std::string, RegexCacheEntry> regexCache;
        // Resource bases encountered along the reference evaluation path,
        // outermost first. $dynamicRef searches these resources for a matching
        // dynamic anchor after its initial static resolution.
        std::vector<SchemaTarget> dynamicScope;

        ValidationCtx(const pjson& aRootSchema, const Options& aOptions,
                      const CompiledSchemaIndex& aCompiled, std::vector<SchemaError>* aPublicErrors)
                : rootSchema(aRootSchema)
                , options(aOptions)
                , compiled(aCompiled)
                , publicErrors(aPublicErrors)
                , depth(0)
                , refResolutions(0)
                , workUsed(0)
                , errorsUsed(0)
                , publicErrorStart(aPublicErrors == nullptr ? 0 : aPublicErrors->size())
                , aborted(false) {}
    };

    struct SchemaBudgetExceeded {};

    // Facade over a caller or speculative error vector enforcing one shared
    // per-validation diagnostic budget.
    struct ErrorSink {
        std::vector<SchemaError>& values;
        ValidationCtx& ctx;
        bool reported;
        size_t discardedFailures;

        ErrorSink(std::vector<SchemaError>& aValues, ValidationCtx& aCtx, bool aReported = true)
                : values(aValues)
                , ctx(aCtx)
                , reported(aReported)
                , discardedFailures(0) {}

        size_t size() const { return reported ? values.size() : discardedFailures; }

        void push_back(const SchemaError& error) {
            if (ctx.aborted)
                return;
            if (!reported) {
                (void)error;
                if (discardedFailures != std::numeric_limits<size_t>::max())
                    ++discardedFailures;
                return;
            }
            const size_t limit = ctx.options.maxErrors == 0 ? size_t(100) : ctx.options.maxErrors;
            if (ctx.errorsUsed >= limit) {
                ctx.aborted = true;
                if (ctx.publicErrors != nullptr &&
                    ctx.publicErrors->size() - ctx.publicErrorStart < limit) {
                    try {
                        ctx.publicErrors->push_back(
                            SchemaError(error.path, "schema validation error budget exceeded"));
                    } catch (...) {
                        ctx.publicErrors = nullptr;
                    }
                }
                throw SchemaBudgetExceeded();
            }
            values.push_back(error);
            ++ctx.errorsUsed;
        }
    };

    //===------------------------------------------------------------------===//
    // Exact numeric constraints and format validators
    //===------------------------------------------------------------------===//

    struct ExactDecimal {
        uint64_t coefficient;
        int exponent10;
    };

    uint64_t magnitudeOf(int64_t value) {
        return value < 0 ? uint64_t(-(value + 1)) + uint64_t(1) : uint64_t(value);
    }

    bool decimalFromText(const std::string& text, ExactDecimal& result) {
        size_t pos = 0;
        if (pos < text.size() && (text[pos] == '+' || text[pos] == '-'))
            ++pos;
        uint64_t coefficient = 0;
        int fractionDigits = 0;
        bool seenDigit = false;
        bool afterPoint = false;
        while (pos < text.size() && text[pos] != 'e' && text[pos] != 'E') {
            const char ch = text[pos++];
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
        if (pos < text.size()) {
            ++pos;
            bool negative = false;
            if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
                negative = text[pos] == '-';
                ++pos;
            }
            if (pos == text.size())
                return false;
            while (pos < text.size()) {
                const char ch = text[pos++];
                if (ch < '0' || ch > '9')
                    return false;
                if (explicitExponent > 10000)
                    return false;
                explicitExponent = explicitExponent * 10 + (ch - '0');
            }
            if (negative)
                explicitExponent = -explicitExponent;
        }
        if (!seenDigit)
            return false;
        if (coefficient == 0) {
            result.coefficient = 0;
            result.exponent10 = 0;
            return true;
        }
        int exponent = explicitExponent - fractionDigits;
        while (coefficient % uint64_t(10) == 0) {
            coefficient /= uint64_t(10);
            ++exponent;
        }
        result.coefficient = coefficient;
        result.exponent10 = exponent;
        return true;
    }

    bool decimalFromNumber(const pjson& value, ExactDecimal& result) {
        if (value.isInteger()) {
            uint64_t u = 0;
            int64_t i = 0;
            result.coefficient =
                value.isUInt() ? (value.tryGet(u), u) : (value.tryGet(i), magnitudeOf(i));
            result.exponent10 = 0;
            if (result.coefficient == 0)
                return true;
            while (result.coefficient % uint64_t(10) == 0) {
                result.coefficient /= uint64_t(10);
                ++result.exponent10;
            }
            return true;
        }
        double d = 0.0;
        if (!value.isDouble() || !value.tryGet(d) || !std::isfinite(d))
            return false;
        return decimalFromText(numberText(value), result);
    }

    std::string formatNumber(const pjson& value) {
        return numberText(value);
    }

    // Decodes nonnegative integral size keywords without truncation.
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
        if (!value.isDouble())
            return false;
        double number = 0.0;
        value.tryGet(number);
        if (!std::isfinite(number) || number < 0.0 || std::floor(number) != number)
            return false;
        const double exclusiveUpper = std::ldexp(1.0, std::numeric_limits<size_t>::digits);
        if (number >= exclusiveUpper) {
            aboveRange = true;
            return true;
        }
        result = static_cast<size_t>(number);
        return true;
    }

    // Implements multipleOf from integers or canonical decimal text.
    bool isExactMultiple(const pjson& value, const pjson& divisor) {
        if (numberAsDouble(divisor) <= 0.0)
            return true;
        if (value.isInt() && divisor.isInt()) {
            int64_t vi = 0, di = 0;
            value.tryGet(vi);
            divisor.tryGet(di);
            const uint64_t d = magnitudeOf(di);
            return magnitudeOf(vi) % d == 0;
        }
        ExactDecimal v = {0, 0};
        ExactDecimal d = {0, 0};
        if (!decimalFromNumber(divisor, d) || d.coefficient == 0)
            return true;
        if (!decimalFromNumber(value, v))
            return false;
        if (v.coefficient == 0)
            return true;
        const int shift = v.exponent10 - d.exponent10;
        if (shift >= 0) {
            uint64_t reduced = d.coefficient;
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
            return v.coefficient % reduced == 0;
        }
        if (v.coefficient % d.coefficient != 0)
            return false;
        uint64_t quotient = v.coefficient / d.coefficient;
        int decimalPlaces = -shift;
        while (decimalPlaces > 0 && quotient % uint64_t(10) == 0) {
            quotient /= uint64_t(10);
            --decimalPlaces;
        }
        return decimalPlaces == 0;
    }

    bool isAsciiDigit(char ch) {
        return ch >= '0' && ch <= '9';
    }
    bool isAsciiHex(char ch) {
        return isAsciiDigit(ch) || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
    }

    bool parseFixedDigits(const std::string& value, size_t offset, size_t count, int& result) {
        if (offset > value.size() || count > value.size() - offset)
            return false;
        result = 0;
        for (size_t i = 0; i < count; ++i) {
            if (!isAsciiDigit(value[offset + i]))
                return false;
            result = result * 10 + (value[offset + i] - '0');
        }
        return true;
    }

    bool isLeapYear(int year) {
        return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    }

    bool validDate(const std::string& value) {
        if (value.size() != 10 || value[4] != '-' || value[7] != '-')
            return false;
        int year = 0, month = 0, day = 0;
        if (!parseFixedDigits(value, 0, 4, year) || !parseFixedDigits(value, 5, 2, month) ||
            !parseFixedDigits(value, 8, 2, day) || month < 1 || month > 12 || day < 1)
            return false;
        static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        int maxDay = days[month - 1];
        if (month == 2 && isLeapYear(year))
            maxDay = 29;
        return day <= maxDay;
    }

    bool validTime(const std::string& value) {
        if (value.size() < 9 || value[2] != ':' || value[5] != ':')
            return false;
        int hour = 0, minute = 0, second = 0;
        if (!parseFixedDigits(value, 0, 2, hour) || !parseFixedDigits(value, 3, 2, minute) ||
            !parseFixedDigits(value, 6, 2, second) || hour > 23 || minute > 59 || second > 60)
            return false;
        size_t pos = 8;
        if (pos < value.size() && value[pos] == '.') {
            ++pos;
            const size_t fractionStart = pos;
            while (pos < value.size() && isAsciiDigit(value[pos]))
                ++pos;
            if (pos == fractionStart)
                return false;
        }
        int offsetMinutes = 0;
        if (pos < value.size() && (value[pos] == 'Z' || value[pos] == 'z')) {
            ++pos;
        } else {
            if (pos + 6 != value.size() || (value[pos] != '+' && value[pos] != '-') ||
                value[pos + 3] != ':')
                return false;
            int offsetHour = 0, offsetMinute = 0;
            if (!parseFixedDigits(value, pos + 1, 2, offsetHour) ||
                !parseFixedDigits(value, pos + 4, 2, offsetMinute) || offsetHour > 23 ||
                offsetMinute > 59)
                return false;
            offsetMinutes = offsetHour * 60 + offsetMinute;
            if (value[pos] == '-')
                offsetMinutes = -offsetMinutes;
            pos += 6;
        }
        if (pos != value.size())
            return false;
        if (second == 60) {
            int utcMinute = (hour * 60 + minute - offsetMinutes) % (24 * 60);
            if (utcMinute < 0)
                utcMinute += 24 * 60;
            if (utcMinute != 23 * 60 + 59)
                return false;
        }
        return true;
    }

    bool validDateTime(const std::string& value) {
        return value.size() > 11 && (value[10] == 'T' || value[10] == 't') &&
               validDate(value.substr(0, 10)) && validTime(value.substr(11));
    }

    bool validIPv4(const std::string& value) {
        size_t pos = 0;
        for (int part = 0; part < 4; ++part) {
            const size_t begin = pos;
            int octet = 0;
            while (pos < value.size() && isAsciiDigit(value[pos])) {
                octet = octet * 10 + (value[pos] - '0');
                if (octet > 255)
                    return false;
                ++pos;
            }
            const size_t digits = pos - begin;
            if (digits == 0 || digits > 3 || (digits > 1 && value[begin] == '0'))
                return false;
            if (part != 3) {
                if (pos >= value.size() || value[pos] != '.')
                    return false;
                ++pos;
            }
        }
        return pos == value.size();
    }

    bool parseIPv6Side(const std::string& side, bool mayContainIPv4, int& units) {
        if (side.empty())
            return true;
        size_t start = 0;
        while (start <= side.size()) {
            const size_t colon = side.find(':', start);
            const size_t end = colon == std::string::npos ? side.size() : colon;
            if (end == start)
                return false;
            const std::string token = side.substr(start, end - start);
            if (token.find('.') != std::string::npos) {
                if (!mayContainIPv4 || end != side.size() || !validIPv4(token))
                    return false;
                units += 2;
            } else {
                if (token.size() > 4)
                    return false;
                for (size_t i = 0; i < token.size(); ++i) {
                    if (!isAsciiHex(token[i]))
                        return false;
                }
                ++units;
            }
            if (colon == std::string::npos)
                break;
            start = colon + 1;
            if (start == side.size())
                return false;
        }
        return true;
    }

    bool validIPv6(const std::string& value) {
        if (value.empty())
            return false;
        const size_t compression = value.find("::");
        if (compression != std::string::npos &&
            value.find("::", compression + 2) != std::string::npos)
            return false;
        int units = 0;
        if (compression == std::string::npos)
            return parseIPv6Side(value, true, units) && units == 8;
        const std::string left = value.substr(0, compression);
        const std::string right = value.substr(compression + 2);
        if (!parseIPv6Side(left, false, units) || !parseIPv6Side(right, true, units))
            return false;
        return units < 8;
    }

    bool validUuid(const std::string& value) {
        if (value.size() != 36 || value[8] != '-' || value[13] != '-' || value[18] != '-' ||
            value[23] != '-')
            return false;
        for (size_t i = 0; i < value.size(); ++i) {
            if (i == 8 || i == 13 || i == 18 || i == 23)
                continue;
            if (!isAsciiHex(value[i]))
                return false;
        }
        return true;
    }

    bool knownFormatValid(const std::string& format, const std::string& value, bool& known) {
        known = true;
        if (format == "date")
            return validDate(value);
        if (format == "time")
            return validTime(value);
        if (format == "date-time")
            return validDateTime(value);
        if (format == "ipv4")
            return validIPv4(value);
        if (format == "ipv6")
            return validIPv6(value);
        if (format == "uuid")
            return validUuid(value);
        known = false;
        return true;
    }

    bool percentDecodeFragment(const std::string& fragment, std::string& decoded) {
        decoded.clear();
        for (size_t i = 0; i < fragment.size(); ++i) {
            if (fragment[i] != '%') {
                decoded += fragment[i];
                continue;
            }
            if (i + 2 >= fragment.size() || !isAsciiHex(fragment[i + 1]) ||
                !isAsciiHex(fragment[i + 2]))
                return false;
            const char hi = fragment[i + 1];
            const char lo = fragment[i + 2];
            const int high =
                isAsciiDigit(hi) ? hi - '0' : (hi >= 'a' ? hi - 'a' + 10 : hi - 'A' + 10);
            const int low =
                isAsciiDigit(lo) ? lo - '0' : (lo >= 'a' ? lo - 'a' + 10 : lo - 'A' + 10);
            decoded += static_cast<char>((high << 4) | low);
            i += 2;
        }
        return true;
    }

    bool uriHasScheme(const std::string& uri) {
        if (uri.empty() || !((uri[0] >= 'A' && uri[0] <= 'Z') || (uri[0] >= 'a' && uri[0] <= 'z')))
            return false;
        for (size_t i = 1; i < uri.size(); ++i) {
            const char c = uri[i];
            if (c == ':')
                return true;
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '+' || c == '-' || c == '.'))
                return false;
        }
        return false;
    }

    bool validAnchorName(const std::string& name) {
        if (name.empty() || !((name[0] >= 'A' && name[0] <= 'Z') ||
                              (name[0] >= 'a' && name[0] <= 'z') || name[0] == '_'))
            return false;
        for (size_t i = 1; i < name.size(); ++i) {
            const char c = name[i];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-' || c == '.' || c == ':'))
                return false;
        }
        return true;
    }

    std::string stripFragment(const std::string& uri) {
        const size_t hash = uri.find('#');
        return hash == std::string::npos ? uri : uri.substr(0, hash);
    }

    void splitReference(const std::string& uri, std::string& document, std::string& fragment) {
        const size_t hash = uri.find('#');
        document = hash == std::string::npos ? uri : uri.substr(0, hash);
        fragment = hash == std::string::npos ? std::string() : uri.substr(hash + 1);
    }

    std::string normalizePath(const std::string& path) {
        const bool absolute = !path.empty() && path[0] == '/';
        std::vector<std::string> segments;
        size_t begin = 0;
        while (begin <= path.size()) {
            const size_t slash = path.find('/', begin);
            const std::string segment =
                path.substr(begin, slash == std::string::npos ? std::string::npos : slash - begin);
            if (segment.empty() || segment == ".") {
                // Preserve only the leading slash through `absolute`.
            } else if (segment == "..") {
                if (!segments.empty())
                    segments.pop_back();
            } else {
                segments.push_back(segment);
            }
            if (slash == std::string::npos)
                break;
            begin = slash + 1;
        }
        std::string result = absolute ? "/" : std::string();
        for (size_t i = 0; i < segments.size(); ++i) {
            if (!result.empty() && result[result.size() - 1] != '/')
                result += '/';
            result += segments[i];
        }
        if (!path.empty() && path[path.size() - 1] == '/' &&
            (result.empty() || result[result.size() - 1] != '/'))
            result += '/';
        return result;
    }

    void splitPathSuffix(const std::string& value, std::string& path, std::string& suffix) {
        const size_t marker = value.find_first_of("?#");
        path = marker == std::string::npos ? value : value.substr(0, marker);
        suffix = marker == std::string::npos ? std::string() : value.substr(marker);
    }

    // RFC 3986 reference resolution sufficient for hierarchical HTTP/file URIs
    // and opaque URNs used by the official suite. Query strings are preserved.
    std::string resolveUri(const std::string& baseWithFragment, const std::string& reference) {
        std::string base = stripFragment(baseWithFragment);
        if (reference.empty())
            return base;
        if (uriHasScheme(reference))
            return reference;
        if (reference[0] == '#')
            return base + reference;

        const size_t colon = base.find(':');
        if (colon == std::string::npos)
            return normalizePath(reference);
        const std::string scheme = base.substr(0, colon + 1);
        const std::string remainder = base.substr(colon + 1);
        if (remainder.compare(0, 2, "//") != 0)
            return scheme + reference; // Opaque URI (for example urn:).
        if (reference.compare(0, 2, "//") == 0)
            return scheme + reference;

        const size_t authorityEnd = remainder.find('/', 2);
        const std::string authority =
            authorityEnd == std::string::npos ? remainder : remainder.substr(0, authorityEnd);
        const std::string basePath =
            authorityEnd == std::string::npos ? std::string("/") : remainder.substr(authorityEnd);
        std::string referencePath;
        std::string referenceSuffix;
        splitPathSuffix(reference, referencePath, referenceSuffix);
        std::string cleanBasePath;
        std::string ignoredSuffix;
        splitPathSuffix(basePath, cleanBasePath, ignoredSuffix);
        if (!reference.empty() && reference[0] == '?')
            return scheme + authority + cleanBasePath + reference;
        if (!referencePath.empty() && referencePath[0] == '/')
            return scheme + authority + normalizePath(referencePath) + referenceSuffix;
        const size_t slash = cleanBasePath.rfind('/');
        const std::string directory =
            slash == std::string::npos ? std::string() : cleanBasePath.substr(0, slash + 1);
        return scheme + authority + normalizePath(directory + referencePath) + referenceSuffix;
    }

    void bestEffortSchemaError(std::vector<SchemaError>& errors, const std::string& path,
                               const std::string& message) noexcept {
        try {
            errors.push_back(SchemaError(path, message));
        } catch (...) {
            return;
        }
    }

    struct DepthGuard {
        ValidationCtx& ctx;
        size_t levels;
        explicit DepthGuard(ValidationCtx& aCtx)
                : ctx(aCtx)
                , levels(1) {
            ++ctx.depth;
        }
        void enterResolvedReference() {
            ++ctx.depth;
            ++levels;
        }
        ~DepthGuard() { ctx.depth -= levels; }
    };

    struct ActiveRefGuard {
        std::vector<std::pair<const pjson*, const pjson*>>& refs;
        const size_t initialSize;
        explicit ActiveRefGuard(std::vector<std::pair<const pjson*, const pjson*>>& aRefs)
                : refs(aRefs)
                , initialSize(aRefs.size()) {}
        void push(const pjson* node, const pjson* schema) {
            refs.push_back(std::make_pair(node, schema));
        }
        ~ActiveRefGuard() {
            while (refs.size() > initialSize)
                refs.pop_back();
        }
    };

    struct DynamicScopeGuard {
        std::vector<SchemaTarget>& scope;
        const size_t initialSize;
        explicit DynamicScopeGuard(std::vector<SchemaTarget>& aScope)
                : scope(aScope)
                , initialSize(aScope.size()) {}
        void pushResource(const SchemaTarget& target) {
            if (scope.empty() || scope.back().baseUri != target.baseUri ||
                scope.back().resourceRoot != target.resourceRoot)
                scope.push_back(target);
        }
        ~DynamicScopeGuard() { scope.resize(initialSize); }
    };

    void failValidationBudget(ValidationCtx& ctx, ErrorSink& errors, const std::string& path,
                              const std::string& message) {
        if (ctx.aborted)
            return;
        ctx.aborted = true;
        const size_t errorLimit = ctx.options.maxErrors == 0 ? size_t(100) : ctx.options.maxErrors;
        if (ctx.errorsUsed >= errorLimit)
            return;
        std::vector<SchemaError>& destination =
            ctx.publicErrors != nullptr ? *ctx.publicErrors : errors.values;
        const size_t before = destination.size();
        bestEffortSchemaError(destination, path, message);
        if (destination.size() != before)
            ++ctx.errorsUsed;
    }

    size_t validationDepthLimit(const Options& options) {
        const size_t requested = options.maxValidationDepth == 0 ? kSchemaValidationDepthHardLimit
                                                                 : options.maxValidationDepth;
        return std::min(requested, kSchemaValidationDepthHardLimit);
    }

    size_t validationRefLimit(const Options& options) {
        return options.maxRefResolutions == 0 ? size_t(1024) : options.maxRefResolutions;
    }

    size_t validationWorkLimit(const Options& options) {
        return options.maxValidationWork == 0 ? size_t(1000000) : options.maxValidationWork;
    }

    size_t resolvedDocumentLimit(const Options& options) {
        return options.maxResolvedDocuments == 0 ? size_t(32) : options.maxResolvedDocuments;
    }

    size_t resolvedByteLimit(const Options& options) {
        return options.maxResolvedBytes == 0 ? size_t(16) * 1024 * 1024 : options.maxResolvedBytes;
    }

    bool chargeValidationWork(ValidationCtx& ctx, ErrorSink& errors, const std::string& path,
                              size_t amount = 1) {
        const size_t limit = validationWorkLimit(ctx.options);
        if (amount > limit - std::min(ctx.workUsed, limit)) {
            failValidationBudget(ctx, errors, path, "schema validation work budget exceeded");
            return false;
        }
        ctx.workUsed += amount;
        return true;
    }

    bool chargeLoopWork(ValidationCtx& ctx, ErrorSink& errors, const std::string& path,
                        size_t amount = 1) {
        return chargeValidationWork(ctx, errors, path, amount);
    }

    bool unicodeLength(const std::string& value, ValidationCtx& ctx, ErrorSink& errors,
                       const std::string& path, size_t& count) {
        count = 0;
        for (size_t offset = 0; offset < value.size(); ++count) {
            const int bytes = utf8Len(value.data(), offset, value.size());
            const size_t consumed = bytes > 0 ? static_cast<size_t>(bytes) : size_t(1);
            if (!chargeLoopWork(ctx, errors, path, consumed))
                return false;
            offset += consumed;
        }
        return true;
    }

    bool addSchemaError(ValidationCtx&, ErrorSink& errors, const std::string& path,
                        const std::string& message) {
        errors.push_back(SchemaError(path, message));
        return !errors.ctx.aborted;
    }

    bool evaluateRegex(const std::string& subject, const std::string& pattern,
                       const std::string& path, ErrorSink& errors, ValidationCtx& ctx,
                       bool& matches) {
        matches = false;
        if (ctx.options.maxRegexSubjectBytes != 0 &&
            subject.size() > ctx.options.maxRegexSubjectBytes) {
            errors.push_back(
                SchemaError(path, "string exceeds regex safety limit (" +
                                      std::to_string(subject.size()) + " bytes, limit " +
                                      std::to_string(ctx.options.maxRegexSubjectBytes) + ")"));
            return false;
        }

        RegexCacheEntry& cached = ctx.regexCache[pattern];
        if (cached.state == RegexCacheEntry::Uninitialized) {
            if (!chargeLoopWork(ctx, errors, path, pattern.size() + size_t(1)))
                return false;
            if (ctx.options.maxRegexPatternBytes != 0 &&
                pattern.size() > ctx.options.maxRegexPatternBytes) {
                cached.state = RegexCacheEntry::PatternTooLarge;
            } else if (!ctx.options.allowUnsafeRegex && !isSafeRegex(pattern)) {
                cached.state = RegexCacheEntry::UnsafePattern;
            } else {
                try {
                    cached.expression.assign(pattern, std::regex::ECMAScript);
                    cached.state = RegexCacheEntry::Ready;
                } catch (const std::regex_error&) {
                    cached.state = RegexCacheEntry::InvalidPattern;
                }
            }
        }

        if (cached.state == RegexCacheEntry::PatternTooLarge) {
            errors.push_back(SchemaError(path, "schema regex pattern exceeds safety limit"));
            return false;
        }
        if (cached.state == RegexCacheEntry::UnsafePattern) {
            errors.push_back(SchemaError(path, "schema regex pattern rejected by safety policy"));
            return false;
        }
        if (cached.state == RegexCacheEntry::InvalidPattern) {
            errors.push_back(SchemaError(path, "schema has an invalid regex pattern"));
            return false;
        }
        if (!chargeLoopWork(ctx, errors, path, subject.size() + size_t(1)))
            return false;
        matches = std::regex_search(subject, cached.expression);
        return true;
    }

    bool isSupportedSchemaKeyword(const std::string& keyword) {
        static const char* const kSupported[] = {
            "type",
            "enum",
            "const",
            "$ref",
            "properties",
            "patternProperties",
            "propertyNames",
            "required",
            "dependentRequired",
            "dependencies",
            "dependentSchemas",
            "additionalProperties",
            "minProperties",
            "maxProperties",
            "items",
            "prefixItems",
            "contains",
            "minContains",
            "maxContains",
            "minItems",
            "maxItems",
            "uniqueItems",
            "minimum",
            "maximum",
            "exclusiveMinimum",
            "exclusiveMaximum",
            "multipleOf",
            "minLength",
            "maxLength",
            "pattern",
            "format",
            "allOf",
            "anyOf",
            "oneOf",
            "not",
            "if",
            "then",
            "else",
            // Metadata/identifier keywords impose no constraint and are always safe.
            "$schema",
            "$id",
            "$anchor",
            "$dynamicAnchor",
            "$dynamicRef",
            "$vocabulary",
            "$defs",
            "$comment",
            "definitions",
            "title",
            "description",
            "default",
            "examples",
            "deprecated",
            "readOnly",
            "writeOnly",
            "unevaluatedItems",
            "unevaluatedProperties",
        };
        for (const char* name : kSupported) {
            if (keyword == name)
                return true;
        }
        return false;
    }

    bool isStandardSchemaKeyword(const std::string& keyword) {
        static const char* const kStandardUnsupported[] = {
            "$recursiveRef",   "$recursiveAnchor", "additionalItems",
            "contentEncoding", "contentMediaType", "contentSchema",
        };
        for (const char* name : kStandardUnsupported) {
            if (keyword == name)
                return true;
        }
        return false;
    }

    std::string schemaPointer(const std::string& keyword) {
        return "/" + pjson::escapePointerToken(keyword);
    }

    void addCompilationError(std::vector<SchemaError>& errors, const std::string& path,
                             const std::string& message) {
        errors.push_back(SchemaError(path, message, SchemaError::SchemaCompilation));
    }

    // Establishes the root schema's dialect and required-vocabulary contract.
    // pjson deliberately names its implemented subset with a private URN rather
    // than accepting the official 2020-12 meta-schema URI and over-claiming
    // conformance. Unknown optional vocabularies are annotations; unknown
    // required vocabularies fail compilation.
    void compileDialectContract(const pjson& schema, const Options& options, std::string& dialect,
                                std::vector<SchemaError>& errors) {
        const size_t errorLimit = options.maxErrors == 0 ? size_t(100) : options.maxErrors;
        dialect = options.defaultDialectUri.empty() ? kDocumentedSubsetDialect
                                                    : options.defaultDialectUri;

        if (schema.isObject()) {
            const pjson* declared = schema.find("$schema");
            if (declared != nullptr) {
                if (!declared->isString()) {
                    addCompilationError(errors, schemaPointer("$schema"),
                                        "$schema must be a string URI");
                } else {
                    dialect = strOf(*declared);
                }
            }
        }

        if (dialect != kDocumentedSubsetDialect) {
            addCompilationError(errors, schemaPointer("$schema"),
                                "unsupported schema dialect: " + dialect);
            return;
        }

        if (!schema.isObject())
            return;
        const pjson* vocabularies = schema.find("$vocabulary");
        if (vocabularies == nullptr)
            return;
        if (!vocabularies->isObject()) {
            addCompilationError(errors, schemaPointer("$vocabulary"),
                                "$vocabulary must be an object mapping URI strings to booleans");
            return;
        }

        const std::vector<std::string> uris = vocabularies->keys();
        for (size_t i = 0; i < uris.size() && errors.size() < errorLimit; ++i) {
            const pjson* requirement = vocabularies->find(uris[i]);
            const std::string path =
                schemaPointer("$vocabulary") + "/" + pjson::escapePointerToken(uris[i]);
            if (requirement == nullptr || !requirement->isBool()) {
                addCompilationError(errors, path, "$vocabulary entries must be boolean");
                continue;
            }
            bool required = false;
            requirement->tryGet(required);
            if (required && uris[i] != kDocumentedSubsetVocabulary) {
                addCompilationError(errors, path,
                                    "unsupported required schema vocabulary: " + uris[i]);
            }
        }
    }

    void compileSchemaResource(const pjson& node, const pjson* resourceRoot,
                               const std::string& inheritedBase, CompiledSchemaIndex& index,
                               std::vector<SchemaError>& errors, const Options& options,
                               const std::string& path, size_t depth = 0) {
        const size_t errorLimit = options.maxErrors == 0 ? size_t(100) : options.maxErrors;
        if (errors.size() >= errorLimit)
            return;
        if (depth >= validationDepthLimit(options)) {
            addCompilationError(errors, path, "schema compilation depth budget exceeded");
            return;
        }
        const size_t workLimit = validationWorkLimit(options);
        if (index.workUsed >= workLimit) {
            addCompilationError(errors, path, "schema compilation work budget exceeded");
            return;
        }
        ++index.workUsed;
        if (!node.isObject() && !node.isBool()) {
            if (options.strictSubset)
                addCompilationError(errors, path, "schema must be an object or boolean");
            return;
        }
        const pjson* currentResource = resourceRoot;
        std::string currentBase = inheritedBase;
        if (node.isObject()) {
            const pjson* id = node.find("$id");
            if (id != nullptr && !id->isString()) {
                addCompilationError(errors, pointerAppend(path, "$id"),
                                    "$id must be a string URI-reference");
                return;
            }
            if (id != nullptr) {
                currentBase = stripFragment(resolveUri(inheritedBase, strOf(*id)));
                currentResource = &node;
                if (resourceRoot != &node) {
                    std::string nestedDialect;
                    compileDialectContract(node, options, nestedDialect, errors);
                }
            }
            if (!currentBase.empty()) {
                std::map<std::string, SchemaResource>::const_iterator existing =
                    index.resources.find(currentBase);
                if (existing != index.resources.end() && existing->second.root != currentResource) {
                    addCompilationError(errors, pointerAppend(path, "$id"),
                                        "duplicate schema resource identifier: " + currentBase);
                    return;
                }
                index.resources[currentBase] = SchemaResource(currentResource, currentBase);
            }
            const SchemaTarget nodeTarget(&node, currentResource, currentBase);
            index.nodeTargets[&node] = nodeTarget;

            const pjson* anchor = node.find("$anchor");
            if (anchor != nullptr && (!anchor->isString() || !validAnchorName(strOf(*anchor)))) {
                addCompilationError(errors, pointerAppend(path, "$anchor"),
                                    "$anchor must be a valid anchor name");
                return;
            }
            if (anchor != nullptr) {
                const std::string name = strOf(*anchor);
                const std::string key = currentBase + "#" + name;
                if (index.anchors.find(key) != index.anchors.end()) {
                    addCompilationError(errors, pointerAppend(path, "$anchor"),
                                        "duplicate schema anchor: " + key);
                    return;
                }
                index.anchors[key] = nodeTarget;
            }
            const pjson* dynamicAnchor = node.find("$dynamicAnchor");
            if (dynamicAnchor != nullptr &&
                (!dynamicAnchor->isString() || !validAnchorName(strOf(*dynamicAnchor)))) {
                addCompilationError(errors, pointerAppend(path, "$dynamicAnchor"),
                                    "$dynamicAnchor must be a valid anchor name");
                return;
            }
            if (dynamicAnchor != nullptr) {
                const std::string name = strOf(*dynamicAnchor);
                const std::string key = currentBase + "#" + name;
                if (index.dynamicAnchors.find(key) != index.dynamicAnchors.end() ||
                    index.anchors.find(key) != index.anchors.end()) {
                    addCompilationError(errors, pointerAppend(path, "$dynamicAnchor"),
                                        "duplicate schema anchor: " + key);
                    return;
                }
                index.dynamicAnchors[key] = nodeTarget;
                index.anchors[key] = nodeTarget;
            }

            for (const char* keyword : {"$ref", "$dynamicRef"}) {
                const pjson* reference = node.find(keyword);
                if (reference == nullptr)
                    continue;
                if (!reference->isString()) {
                    addCompilationError(errors, pointerAppend(path, keyword),
                                        std::string(keyword) + " must be a string URI-reference");
                    continue;
                }
                std::string document;
                std::string fragment;
                splitReference(resolveUri(currentBase, strOf(*reference)), document, fragment);
                if (!document.empty())
                    index.pendingDocuments.insert(document);
            }

            // Traverse only positions whose values are schemas. Objects stored
            // in const/default/examples or application annotations are instance
            // data and must never create resources or anchors.
            for (const char* keyword :
                 {"additionalProperties", "unevaluatedProperties", "unevaluatedItems", "items",
                  "contains", "propertyNames", "not", "if", "then", "else"}) {
                const pjson* child = node.find(keyword);
                if (child != nullptr && (child->isObject() || child->isBool()))
                    compileSchemaResource(*child, currentResource, currentBase, index, errors,
                                          options, pointerAppend(path, keyword), depth + 1);
            }
            for (const char* keyword :
                 {"$defs", "definitions", "properties", "patternProperties", "dependentSchemas"}) {
                const pjson* container = node.find(keyword);
                if (container == nullptr || !container->isObject())
                    continue;
                const std::vector<std::string> names = container->keys();
                for (size_t i = 0; i < names.size(); ++i) {
                    const pjson* child = container->find(names[i]);
                    if (child != nullptr)
                        compileSchemaResource(
                            *child, currentResource, currentBase, index, errors, options,
                            pointerAppend(pointerAppend(path, keyword), names[i]), depth + 1);
                }
            }
            // Legacy dependencies may contain either property-name arrays or schemas.
            if (const pjson* dependencies = node.find("dependencies")) {
                if (dependencies->isObject()) {
                    const std::vector<std::string> names = dependencies->keys();
                    for (size_t i = 0; i < names.size(); ++i) {
                        const pjson* child = dependencies->find(names[i]);
                        if (child != nullptr && (child->isObject() || child->isBool()))
                            compileSchemaResource(
                                *child, currentResource, currentBase, index, errors, options,
                                pointerAppend(pointerAppend(path, "dependencies"), names[i]),
                                depth + 1);
                    }
                }
            }
            for (const char* keyword : {"allOf", "anyOf", "oneOf", "prefixItems"}) {
                const pjson* array = node.find(keyword);
                if (array == nullptr || !array->isArray())
                    continue;
                for (size_t i = 0; i < array->size(); ++i) {
                    const pjson* child = array->find(static_cast<int>(i));
                    if (child != nullptr)
                        compileSchemaResource(
                            *child, currentResource, currentBase, index, errors, options,
                            pointerAppend(pointerAppend(path, keyword), std::to_string(i)),
                            depth + 1);
                }
            }
            // Draft 7 tuple-form items is an array of schemas.
            if (const pjson* items = node.find("items")) {
                if (items->isArray()) {
                    for (size_t i = 0; i < items->size(); ++i) {
                        const pjson* child = items->find(static_cast<int>(i));
                        if (child != nullptr)
                            compileSchemaResource(
                                *child, currentResource, currentBase, index, errors, options,
                                pointerAppend(pointerAppend(path, "items"), std::to_string(i)),
                                depth + 1);
                    }
                }
            }
        }
        return;
    }

    void compileExternalResources(CompiledSchemaIndex& index, const Options& options,
                                  std::vector<SchemaError>& errors) {
        const size_t errorLimit = options.maxErrors == 0 ? size_t(100) : options.maxErrors;
        while (!index.pendingDocuments.empty() && errors.size() < errorLimit) {
            const std::string documentUri = *index.pendingDocuments.begin();
            index.pendingDocuments.erase(index.pendingDocuments.begin());
            if (index.resources.find(documentUri) != index.resources.end())
                continue;
            if (!uriHasScheme(documentUri)) {
                addCompilationError(
                    errors, "",
                    "relative external schema reference requires a retrieval URI or root $id: " +
                        documentUri);
                index.failedDocuments.insert(documentUri);
                continue;
            }
            if (options.resolver == nullptr) {
                addCompilationError(errors, "", "no resolver for external schema: " + documentUri);
                index.failedDocuments.insert(documentUri);
                continue;
            }
            if (index.documents.size() >= resolvedDocumentLimit(options)) {
                addCompilationError(errors, "", "schema resolved-document budget exceeded");
                index.failedDocuments.insert(documentUri);
                return;
            }

            index.documents.push_back(ResolvedDocument(documentUri));
            ResolvedDocument& loaded = index.documents.back();
            pjson temporary;
            bool resolved = false;
            try {
                resolved = options.resolver(documentUri, temporary, options.resolverContext);
            } catch (const std::exception& exception) {
                index.documents.pop_back();
                addCompilationError(errors, "",
                                    "external schema resolver threw for " + documentUri + ": " +
                                        exception.what());
                index.failedDocuments.insert(documentUri);
                continue;
            } catch (...) {
                index.documents.pop_back();
                addCompilationError(errors, "",
                                    "external schema resolver threw for " + documentUri);
                index.failedDocuments.insert(documentUri);
                continue;
            }
            if (!resolved) {
                index.documents.pop_back();
                addCompilationError(errors, "",
                                    "external schema resolution failed: " + documentUri);
                index.failedDocuments.insert(documentUri);
                continue;
            }
            loaded.schema.copyFrom(temporary);
            std::string resolvedDialect;
            const size_t beforeContract = errors.size();
            compileDialectContract(loaded.schema, options, resolvedDialect, errors);
            if (errors.size() != beforeContract) {
                index.documents.pop_back();
                index.failedDocuments.insert(documentUri);
                continue;
            }
            const size_t limit = resolvedByteLimit(options);
            if (index.resolvedBytes >= limit) {
                index.documents.pop_back();
                addCompilationError(errors, "", "schema resolved-byte budget exceeded");
                index.failedDocuments.insert(documentUri);
                return;
            }
            std::string compact;
            try {
                pjson::SerializeOptions compactOptions;
                compactOptions.maxOutputBytes = limit - index.resolvedBytes;
                compact = loaded.schema.toString(compactOptions);
            } catch (const std::length_error&) {
                index.documents.pop_back();
                addCompilationError(errors, "", "schema resolved-byte budget exceeded");
                index.failedDocuments.insert(documentUri);
                return;
            } catch (const std::exception& exception) {
                index.documents.pop_back();
                addCompilationError(errors, "",
                                    "resolved schema is not serializable: " +
                                        std::string(exception.what()));
                index.failedDocuments.insert(documentUri);
                continue;
            }
            if (compact.size() > limit - std::min(index.resolvedBytes, limit)) {
                index.documents.pop_back();
                addCompilationError(errors, "", "schema resolved-byte budget exceeded");
                index.failedDocuments.insert(documentUri);
                return;
            }
            index.resolvedBytes += compact.size();

            const pjson* root = &loaded.schema;
            // Keep the retrieval URI as an alias, then let compilation apply the
            // root `$id` exactly once relative to that retrieval URI.
            index.resources[documentUri] = SchemaResource(root, documentUri);
            compileSchemaResource(*root, root, documentUri, index, errors, options, "");
        }
    }

    bool resolveCompiledTarget(const std::string& reference, const std::string& baseUri,
                               const CompiledSchemaIndex& index, SchemaTarget& target) {
        const std::string absolute = resolveUri(baseUri, reference);
        std::string document;
        std::string fragment;
        splitReference(absolute, document, fragment);
        if (document.empty())
            document = stripFragment(baseUri);
        std::map<std::string, SchemaResource>::const_iterator resource =
            index.resources.find(document);
        if (resource == index.resources.end())
            return false;

        const pjson* root = resource->second.root;
        if (fragment.empty()) {
            target = SchemaTarget(root, root, resource->second.baseUri);
            return true;
        }

        std::string decoded;
        if (!percentDecodeFragment(fragment, decoded))
            return false;
        if (decoded.empty() || decoded[0] != '/') {
            const std::string key = document + "#" + decoded;
            std::map<std::string, SchemaTarget>::const_iterator anchor = index.anchors.find(key);
            if (anchor == index.anchors.end())
                return false;
            target = anchor->second;
            return true;
        }

        pjson::PointerError error;
        const pjson* selected = root->findPointer(decoded, error);
        if (selected == nullptr)
            return false;
        std::map<const pjson*, SchemaTarget>::const_iterator indexed =
            index.nodeTargets.find(selected);
        target = indexed == index.nodeTargets.end()
                     ? SchemaTarget(selected, root, resource->second.baseUri)
                     : indexed->second;
        return true;
    }

    void validateCompiledReferences(const CompiledSchemaIndex& index, const Options& options,
                                    std::vector<SchemaError>& errors) {
        const size_t errorLimit = options.maxErrors == 0 ? size_t(100) : options.maxErrors;
        for (std::map<const pjson*, SchemaTarget>::const_iterator it = index.nodeTargets.begin();
             it != index.nodeTargets.end() && errors.size() < errorLimit; ++it) {
            const pjson* schema = it->first;
            if (!schema->isObject())
                continue;
            for (const char* keyword : {"$ref", "$dynamicRef"}) {
                const pjson* reference = schema->find(keyword);
                if (reference == nullptr || !reference->isString())
                    continue;
                SchemaTarget target;
                if (!resolveCompiledTarget(strOf(*reference), it->second.baseUri, index, target)) {
                    std::string document;
                    std::string fragment;
                    splitReference(resolveUri(it->second.baseUri, strOf(*reference)), document,
                                   fragment);
                    if (index.failedDocuments.find(document) != index.failedDocuments.end())
                        continue;
                    addCompilationError(errors, "",
                                        std::string("unresolved ") + keyword + ": " +
                                            strOf(*reference));
                } else if (options.strictSubset && target.schema != nullptr &&
                           !target.schema->isObject() && !target.schema->isBool()) {
                    addCompilationError(errors, "",
                                        std::string(keyword) +
                                            " target must be an object or boolean schema");
                }
            }
        }
    }

    bool resolveSchemaReference(const std::string& reference, const pjson* resourceRoot,
                                const std::string& baseUri, ValidationCtx& ctx, ErrorSink& errors,
                                const std::string& path, SchemaTarget& target) {
        const std::string absolute = resolveUri(baseUri, reference);
        std::string document;
        std::string fragment;
        splitReference(absolute, document, fragment);
        std::string decodedFragment;
        if (!percentDecodeFragment(fragment, decodedFragment)) {
            errors.push_back(SchemaError(path, "malformed schema reference: " + reference));
            return false;
        }

        if (document.empty())
            document = stripFragment(baseUri);
        std::map<std::string, SchemaResource>::const_iterator resource =
            ctx.compiled.resources.find(document);
        if (resource == ctx.compiled.resources.end()) {
            errors.push_back(SchemaError(path, "unresolved compiled schema resource: " + document));
            return false;
        }

        const pjson* root = resource->second.root != nullptr ? resource->second.root : resourceRoot;
        if (fragment.empty()) {
            target = SchemaTarget(root, root, resource->second.baseUri);
            return true;
        }
        if (decodedFragment.empty() || decodedFragment[0] != '/') {
            const std::string anchorKey = document + "#" + decodedFragment;
            std::map<std::string, SchemaTarget>::const_iterator found =
                ctx.compiled.anchors.find(anchorKey);
            if (found == ctx.compiled.anchors.end()) {
                errors.push_back(SchemaError(path, "unresolved schema anchor: " + absolute));
                return false;
            }
            target = found->second;
            return true;
        }

        pjson::PointerError pointerError;
        const pjson* selected = root->findPointer(decodedFragment, pointerError);
        if (selected == nullptr) {
            errors.push_back(SchemaError(path, "unresolved schema reference: " + reference));
            return false;
        }
        std::map<const pjson*, SchemaTarget>::const_iterator indexed =
            ctx.compiled.nodeTargets.find(selected);
        target = indexed == ctx.compiled.nodeTargets.end()
                     ? SchemaTarget(selected, root, resource->second.baseUri)
                     : indexed->second;
        return true;
    }

    // Forward declaration: the recursive core.
    bool validateCtx(const pjson& node, const pjson& schema, const std::string& path,
                     ErrorSink& errors, ValidationCtx& ctx, const pjson* resourceRoot = nullptr,
                     const std::string& baseUri = std::string(),
                     SchemaAnnotations* annotations = nullptr);

    //===------------------------------------------------------------------===//
    // Budgeted structural equality (public-API traversal)
    //===------------------------------------------------------------------===//
    // Returns true and sets equal when the comparison completes within budget;
    // returns false only when a resource budget was exhausted.
    bool equalWithBudget(const pjson& left, const pjson& right, ValidationCtx& ctx,
                         ErrorSink& errors, const std::string& path, bool& equal) {
        struct Pair {
            const pjson* left;
            const pjson* right;
        };
        std::vector<Pair> work;
        Pair root = {&left, &right};
        work.push_back(root);
        equal = false;

        while (!work.empty()) {
            if (!chargeLoopWork(ctx, errors, path))
                return false;
            const Pair current = work.back();
            work.pop_back();
            const pjson& l = *current.left;
            const pjson& r = *current.right;

            if (l.isNumber() && r.isNumber()) {
                int order = 0;
                if (!l.tryCompareNumber(r, order) || order != 0)
                    return true; // not equal (or NaN-unordered)
                continue;
            }
            if (l.getType() != r.getType())
                return true;

            if (l.isNull() || l.isBool()) {
                bool lb = false, rb = false;
                if (l.isBool()) {
                    l.tryGet(lb);
                    r.tryGet(rb);
                    if (lb != rb)
                        return true;
                }
            } else if (l.isString()) {
                std::string ls = strOf(l), rs = strOf(r);
                const size_t bytes = std::max(ls.size(), rs.size());
                if (!chargeLoopWork(ctx, errors, path, bytes))
                    return false;
                if (ls != rs)
                    return true;
            } else if (l.isArray()) {
                if (l.size() != r.size())
                    return true;
                for (size_t i = 0; i < l.size(); ++i) {
                    const pjson* le = l.find(static_cast<int>(i));
                    const pjson* re = r.find(static_cast<int>(i));
                    if (le == nullptr || re == nullptr)
                        return true;
                    Pair child = {le, re};
                    work.push_back(child);
                }
            } else if (l.isObject()) {
                if (l.size() != r.size())
                    return true;
                const std::vector<std::string> lk = l.keys();
                const std::vector<std::string> rk = r.keys();
                if (lk != rk)
                    return true; // key sets (sorted) differ
                for (size_t i = 0; i < lk.size(); ++i) {
                    if (!chargeLoopWork(ctx, errors, path, lk[i].size() + size_t(1)))
                        return false;
                    const pjson* lv = l.find(lk[i]);
                    const pjson* rv = r.find(lk[i]);
                    if (lv == nullptr || rv == nullptr)
                        return true;
                    Pair child = {lv, rv};
                    work.push_back(child);
                }
            }
        }
        equal = true;
        return true;
    }

    //===------------------------------------------------------------------===//
    // Recursive validation core (pure public-API traversal)
    //===------------------------------------------------------------------===//
    bool validateCtx(const pjson& node, const pjson& schema0, const std::string& path,
                     ErrorSink& errors, ValidationCtx& ctx, const pjson* resourceRoot,
                     const std::string& baseUri, SchemaAnnotations* annotations) {
        SchemaAnnotations localAnnotations;
        SchemaAnnotations& evaluated = annotations == nullptr ? localAnnotations : *annotations;
        if (ctx.aborted)
            return false;
        if (!chargeValidationWork(ctx, errors, path))
            return false;
        if (ctx.depth >= validationDepthLimit(ctx.options)) {
            failValidationBudget(ctx, errors, path, "schema validation depth budget exceeded");
            return false;
        }
        DepthGuard depthGuard(ctx);
        ActiveRefGuard activeRefGuard(ctx.activeRefs);
        DynamicScopeGuard dynamicScopeGuard(ctx.dynamicScope);
        const pjson* currentSchema = &schema0;
        const pjson* currentResourceRoot = resourceRoot == nullptr ? &ctx.rootSchema : resourceRoot;
        std::string currentBaseUri = baseUri;

        std::map<const pjson*, SchemaTarget>::const_iterator initialTarget =
            ctx.compiled.nodeTargets.find(currentSchema);
        if (initialTarget != ctx.compiled.nodeTargets.end()) {
            currentResourceRoot = initialTarget->second.resourceRoot;
            currentBaseUri = initialTarget->second.baseUri;
        }

        // Resolve consecutive static references iteratively (stack-safe). In
        // pjson's subset dialect a string $ref ignores siblings, preserving its
        // documented draft-07-compatible behavior.
        for (;;) {
            if (currentSchema->isBool()) {
                if (!boolOf(*currentSchema)) {
                    errors.push_back(SchemaError(path, "schema is false; no value is valid here"));
                    return false;
                }
                return true;
            }
            if (!currentSchema->isObject())
                return true;

            const pjson* ref = currentSchema->find("$ref");
            if (ref == nullptr || !ref->isString())
                break;
            if (ctx.options.refSiblings)
                break;

            const std::string refText = strOf(*ref);
            if (ctx.refResolutions >= validationRefLimit(ctx.options)) {
                failValidationBudget(ctx, errors, path, "schema $ref resolution budget exceeded");
                return false;
            }
            ++ctx.refResolutions;

            SchemaTarget resolved;
            if (!resolveSchemaReference(refText, currentResourceRoot, currentBaseUri, ctx, errors,
                                        path, resolved))
                return false;

            const std::pair<const pjson*, const pjson*> active(&node, resolved.schema);
            if (std::find(ctx.activeRefs.begin(), ctx.activeRefs.end(), active) !=
                ctx.activeRefs.end()) {
                errors.push_back(SchemaError(path, "schema reference cycle detected: " + refText));
                return false;
            }
            activeRefGuard.push(&node, resolved.schema);

            if (!chargeValidationWork(ctx, errors, path))
                return false;
            if (ctx.depth >= validationDepthLimit(ctx.options)) {
                failValidationBudget(ctx, errors, path, "schema validation depth budget exceeded");
                return false;
            }
            depthGuard.enterResolvedReference();
            currentSchema = resolved.schema;
            currentResourceRoot = resolved.resourceRoot;
            currentBaseUri = resolved.baseUri;
        }

        const pjson& schema = *currentSchema;
        std::map<const pjson*, SchemaTarget>::const_iterator resolvedTarget =
            ctx.compiled.nodeTargets.find(currentSchema);
        if (resolvedTarget != ctx.compiled.nodeTargets.end()) {
            currentResourceRoot = resolvedTarget->second.resourceRoot;
            currentBaseUri = resolvedTarget->second.baseUri;
        }
        const size_t before = errors.size();
        dynamicScopeGuard.pushResource(
            SchemaTarget(currentResourceRoot, currentResourceRoot, currentBaseUri));

        if (ctx.options.refSiblings) {
            const pjson* ref = schema.find("$ref");
            if (ref != nullptr && ref->isString()) {
                if (ctx.refResolutions >= validationRefLimit(ctx.options)) {
                    failValidationBudget(ctx, errors, path,
                                         "schema $ref resolution budget exceeded");
                    return false;
                }
                ++ctx.refResolutions;
                SchemaTarget resolved;
                const std::string refText = strOf(*ref);
                if (!resolveSchemaReference(refText, currentResourceRoot, currentBaseUri, ctx,
                                            errors, path, resolved))
                    return false;
                const std::pair<const pjson*, const pjson*> active(&node, resolved.schema);
                if (std::find(ctx.activeRefs.begin(), ctx.activeRefs.end(), active) !=
                    ctx.activeRefs.end()) {
                    errors.push_back(
                        SchemaError(path, "schema reference cycle detected: " + refText));
                    return false;
                }
                activeRefGuard.push(&node, resolved.schema);
                SchemaAnnotations referenced;
                const bool referenceValid =
                    validateCtx(node, *resolved.schema, path, errors, ctx, resolved.resourceRoot,
                                resolved.baseUri, &referenced);
                if (referenceValid)
                    evaluated.merge(referenced);
                if (ctx.aborted)
                    return false;
            }
        }

        // A dynamic reference first resolves statically. When that target
        // declares the same dynamic anchor, the outermost matching resource in
        // the current dynamic scope replaces it, as required by Draft 2020-12.
        if (const pjson* dynamicRef = schema.find("$dynamicRef")) {
            if (dynamicRef->isString()) {
                const std::string refText = strOf(*dynamicRef);
                if (ctx.refResolutions >= validationRefLimit(ctx.options)) {
                    failValidationBudget(ctx, errors, path,
                                         "schema $dynamicRef resolution budget exceeded");
                    return false;
                }
                ++ctx.refResolutions;
                SchemaTarget resolved;
                if (!resolveSchemaReference(refText, currentResourceRoot, currentBaseUri, ctx,
                                            errors, path, resolved))
                    return false;

                std::string document;
                std::string fragment;
                splitReference(resolveUri(currentBaseUri, refText), document, fragment);
                if (!fragment.empty() && fragment[0] != '/' && resolved.schema->isObject()) {
                    const pjson* declaration = resolved.schema->find("$dynamicAnchor");
                    if (declaration != nullptr && declaration->isString() &&
                        strOf(*declaration) == fragment) {
                        for (size_t i = 0; i < ctx.dynamicScope.size(); ++i) {
                            const std::string key = ctx.dynamicScope[i].baseUri + "#" + fragment;
                            std::map<std::string, SchemaTarget>::const_iterator scoped =
                                ctx.compiled.dynamicAnchors.find(key);
                            if (scoped != ctx.compiled.dynamicAnchors.end()) {
                                resolved = scoped->second;
                                break;
                            }
                        }
                    }
                }

                const std::pair<const pjson*, const pjson*> active(&node, resolved.schema);
                if (std::find(ctx.activeRefs.begin(), ctx.activeRefs.end(), active) !=
                    ctx.activeRefs.end()) {
                    errors.push_back(
                        SchemaError(path, "schema dynamic-reference cycle detected: " + refText));
                    return false;
                }
                activeRefGuard.push(&node, resolved.schema);
                SchemaAnnotations referenced;
                const bool referenceValid =
                    validateCtx(node, *resolved.schema, path, errors, ctx, resolved.resourceRoot,
                                resolved.baseUri, &referenced);
                if (referenceValid)
                    evaluated.merge(referenced);
                if (ctx.aborted)
                    return false;
            }
        }

        // ---- strict, fail-closed subset check ----
        if (ctx.options.strictSubset) {
            const std::vector<std::string> keys = schema.keys();
            for (size_t i = 0; i < keys.size(); ++i) {
                if (!chargeLoopWork(ctx, errors, path))
                    return false;
                if (!isSupportedSchemaKeyword(keys[i]) && isStandardSchemaKeyword(keys[i])) {
                    addSchemaError(ctx, errors, path,
                                   "strict schema mode: unsupported standard keyword \"" + keys[i] +
                                       "\"");
                }
            }
            if (ctx.aborted)
                return false;
        }

        // ---- type ----
        if (const pjson* t = schema.find("type")) {
            if (t->isString()) {
                if (!typeMatches(node, strOf(*t)))
                    errors.push_back(SchemaError(path, "expected type " + strOf(*t) + ", got " +
                                                           typeName(node)));
            } else if (t->isArray()) {
                bool matched = false;
                std::string names;
                for (size_t i = 0; i < t->size(); ++i) {
                    if (!chargeLoopWork(ctx, errors, path))
                        return false;
                    const pjson* e = t->find(static_cast<int>(i));
                    if (e && e->isString()) {
                        if (!names.empty())
                            names += ", ";
                        names += strOf(*e);
                        if (typeMatches(node, strOf(*e))) {
                            matched = true;
                            break;
                        }
                    }
                }
                if (!matched)
                    errors.push_back(SchemaError(path, "expected one of type [" + names +
                                                           "], got " + typeName(node)));
            }
        }

        // ---- const ----
        if (const pjson* cst = schema.find("const")) {
            bool equal = false;
            if (!equalWithBudget(node, *cst, ctx, errors, path, equal))
                return false;
            if (!equal)
                errors.push_back(SchemaError(path, "value does not equal the required const"));
        }

        // ---- enum ----
        if (const pjson* en = schema.find("enum")) {
            if (en->isArray()) {
                bool found = false;
                for (size_t i = 0; i < en->size(); ++i) {
                    const pjson* opt = en->find(static_cast<int>(i));
                    if (opt == nullptr)
                        continue;
                    bool equal = false;
                    if (!equalWithBudget(node, *opt, ctx, errors, path, equal))
                        return false;
                    if (equal) {
                        found = true;
                        break;
                    }
                }
                if (!found)
                    errors.push_back(SchemaError(path, "value is not in the allowed enum"));
            }
        }

        // ---- numeric constraints ----
        if (node.isNumber()) {
            int order = 0;
            if (const pjson* m = schema.find("minimum")) {
                if (m->isNumber() && node.tryCompareNumber(*m, order) && order < 0)
                    addSchemaError(ctx, errors, path,
                                   "value " + formatNumber(node) + " is below minimum " +
                                       formatNumber(*m));
            }
            if (const pjson* m = schema.find("maximum")) {
                if (m->isNumber() && node.tryCompareNumber(*m, order) && order > 0)
                    addSchemaError(ctx, errors, path,
                                   "value " + formatNumber(node) + " is above maximum " +
                                       formatNumber(*m));
            }
            if (const pjson* m = schema.find("exclusiveMinimum")) {
                if (m->isNumber() && node.tryCompareNumber(*m, order) && order <= 0)
                    addSchemaError(ctx, errors, path,
                                   "value " + formatNumber(node) +
                                       " is not greater than exclusiveMinimum " + formatNumber(*m));
            }
            if (const pjson* m = schema.find("exclusiveMaximum")) {
                if (m->isNumber() && node.tryCompareNumber(*m, order) && order >= 0)
                    addSchemaError(ctx, errors, path,
                                   "value " + formatNumber(node) +
                                       " is not less than exclusiveMaximum " + formatNumber(*m));
            }
            if (const pjson* m = schema.find("multipleOf")) {
                if (m->isNumber() && !isExactMultiple(node, *m))
                    addSchemaError(ctx, errors, path,
                                   "value " + formatNumber(node) + " is not a multiple of " +
                                       formatNumber(*m));
            }
        }

        // ---- string constraints ----
        if (node.isString()) {
            const std::string s = strOf(node);
            size_t length = 0;
            if (!unicodeLength(s, ctx, errors, path, length))
                return false;
            if (const pjson* m = schema.find("minLength")) {
                size_t bound = 0;
                bool aboveRange = false;
                if (schemaSize(*m, bound, aboveRange) && (aboveRange || length < bound))
                    addSchemaError(ctx, errors, path,
                                   "string length " + std::to_string(length) +
                                       " is below minLength " + formatNumber(*m));
            }
            if (const pjson* m = schema.find("maxLength")) {
                size_t bound = 0;
                bool aboveRange = false;
                if (schemaSize(*m, bound, aboveRange) && !aboveRange && length > bound)
                    addSchemaError(ctx, errors, path,
                                   "string length " + std::to_string(length) +
                                       " is above maxLength " + formatNumber(*m));
            }
            if (const pjson* p = schema.find("pattern")) {
                if (p->isString()) {
                    const std::string pattern = strOf(*p);
                    bool matches = false;
                    if (evaluateRegex(s, pattern, path, errors, ctx, matches) && !matches)
                        errors.push_back(
                            SchemaError(path, "string does not match pattern /" + pattern + "/"));
                }
            }
            if (ctx.options.validateFormats) {
                if (const pjson* format = schema.find("format")) {
                    if (format->isString()) {
                        bool known = false;
                        if (!knownFormatValid(strOf(*format), s, known) && known)
                            errors.push_back(SchemaError(path, "string is not a valid " +
                                                                   strOf(*format) + " format"));
                    }
                }
            }
        }

        // ---- array constraints ----
        if (node.isArray()) {
            const size_t arrSize = node.size();
            if (const pjson* m = schema.find("minItems")) {
                size_t bound = 0;
                bool aboveRange = false;
                if (schemaSize(*m, bound, aboveRange) && (aboveRange || arrSize < bound))
                    addSchemaError(ctx, errors, path,
                                   "array has " + std::to_string(arrSize) +
                                       " items, below minItems " + formatNumber(*m));
            }
            if (const pjson* m = schema.find("maxItems")) {
                size_t bound = 0;
                bool aboveRange = false;
                if (schemaSize(*m, bound, aboveRange) && !aboveRange && arrSize > bound)
                    addSchemaError(ctx, errors, path,
                                   "array has " + std::to_string(arrSize) +
                                       " items, above maxItems " + formatNumber(*m));
            }
            if (const pjson* u = schema.find("uniqueItems")) {
                if (u->isBool() && boolOf(*u)) {
                    bool dup = false;
                    for (size_t i = 0; i < arrSize && !dup; ++i) {
                        for (size_t j = i + 1; j < arrSize; ++j) {
                            const pjson* a = node.find(static_cast<int>(i));
                            const pjson* b = node.find(static_cast<int>(j));
                            bool equal = false;
                            if (a && b && !equalWithBudget(*a, *b, ctx, errors, path, equal))
                                return false;
                            if (equal) {
                                dup = true;
                                break;
                            }
                        }
                    }
                    if (dup)
                        errors.push_back(SchemaError(path, "array items are not unique"));
                }
            }
            const pjson* items = schema.find("items");
            const pjson* prefixItems = schema.find("prefixItems");
            size_t prefixCount = 0;
            if (prefixItems && prefixItems->isArray()) {
                prefixCount = std::min(arrSize, prefixItems->size());
                for (size_t i = 0; i < prefixCount && !ctx.aborted; ++i) {
                    if (!chargeLoopWork(ctx, errors, path))
                        return false;
                    const pjson* elem = node.find(static_cast<int>(i));
                    const pjson* sub = prefixItems->find(static_cast<int>(i));
                    if (elem && sub) {
                        evaluated.items.insert(i);
                        validateCtx(*elem, *sub, pointerAppend(path, std::to_string(i)), errors,
                                    ctx);
                    }
                }
            }
            if (items != nullptr) {
                if (items->isArray() && prefixItems == nullptr) {
                    // Legacy tuple form of "items".
                    const size_t count = std::min(arrSize, items->size());
                    for (size_t i = 0; i < count && !ctx.aborted; ++i) {
                        if (!chargeLoopWork(ctx, errors, path))
                            return false;
                        const pjson* elem = node.find(static_cast<int>(i));
                        const pjson* sub = items->find(static_cast<int>(i));
                        if (elem && sub) {
                            evaluated.items.insert(i);
                            validateCtx(*elem, *sub, pointerAppend(path, std::to_string(i)), errors,
                                        ctx);
                        }
                    }
                } else {
                    for (size_t i = prefixCount; i < arrSize && !ctx.aborted; ++i) {
                        if (!chargeLoopWork(ctx, errors, path))
                            return false;
                        const pjson* elem = node.find(static_cast<int>(i));
                        if (elem) {
                            evaluated.items.insert(i);
                            validateCtx(*elem, *items, pointerAppend(path, std::to_string(i)),
                                        errors, ctx);
                        }
                    }
                }
            }

            // ---- contains / minContains / maxContains ----
            if (const pjson* contains = schema.find("contains")) {
                size_t matched = 0;
                for (size_t i = 0; i < arrSize && !ctx.aborted; ++i) {
                    if (!chargeLoopWork(ctx, errors, path))
                        return false;
                    const pjson* elem = node.find(static_cast<int>(i));
                    if (elem == nullptr)
                        continue;
                    std::vector<SchemaError> scratch;
                    ErrorSink scratchSink(scratch, ctx, false);
                    if (validateCtx(*elem, *contains, pointerAppend(path, std::to_string(i)),
                                    scratchSink, ctx)) {
                        ++matched;
                        evaluated.items.insert(i);
                    }
                    if (ctx.aborted)
                        return false;
                }
                size_t minContains = 1;
                bool aboveRange = false;
                if (const pjson* mc = schema.find("minContains")) {
                    size_t bound = 0;
                    if (schemaSize(*mc, bound, aboveRange))
                        minContains = aboveRange ? std::numeric_limits<size_t>::max() : bound;
                }
                if (matched < minContains)
                    addSchemaError(ctx, errors, path,
                                   "array has " + std::to_string(matched) +
                                       " items matching \"contains\", below minContains " +
                                       std::to_string(minContains));
                if (const pjson* xc = schema.find("maxContains")) {
                    size_t bound = 0;
                    bool xcAbove = false;
                    if (schemaSize(*xc, bound, xcAbove) && !xcAbove && matched > bound)
                        addSchemaError(ctx, errors, path,
                                       "array has " + std::to_string(matched) +
                                           " items matching \"contains\", above maxContains " +
                                           std::to_string(bound));
                }
            }
        }

        // ---- object constraints ----
        if (node.isObject()) {
            const std::vector<std::string> memberKeys = node.keys();

            if (const pjson* req = schema.find("required")) {
                if (req->isArray()) {
                    for (size_t i = 0; i < req->size(); ++i) {
                        if (!chargeLoopWork(ctx, errors, path))
                            return false;
                        const pjson* k = req->find(static_cast<int>(i));
                        if (k && k->isString() && !node.hasKey(strOf(*k)))
                            errors.push_back(SchemaError(path, "missing required property \"" +
                                                                   strOf(*k) + "\""));
                    }
                }
            }
            if (const pjson* m = schema.find("minProperties")) {
                size_t bound = 0;
                bool aboveRange = false;
                if (schemaSize(*m, bound, aboveRange) && (aboveRange || memberKeys.size() < bound))
                    addSchemaError(ctx, errors, path,
                                   "object has " + std::to_string(memberKeys.size()) +
                                       " properties, below minProperties " + formatNumber(*m));
            }
            if (const pjson* m = schema.find("maxProperties")) {
                size_t bound = 0;
                bool aboveRange = false;
                if (schemaSize(*m, bound, aboveRange) && !aboveRange && memberKeys.size() > bound)
                    addSchemaError(ctx, errors, path,
                                   "object has " + std::to_string(memberKeys.size()) +
                                       " properties, above maxProperties " + formatNumber(*m));
            }

            const pjson* props = schema.find("properties");
            if (props && props->isObject()) {
                const std::vector<std::string> propKeys = props->keys();
                for (size_t i = 0; i < propKeys.size(); ++i) {
                    if (!chargeLoopWork(ctx, errors, path))
                        return false;
                    const pjson* member = node.find(propKeys[i]);
                    const pjson* sub = props->find(propKeys[i]);
                    if (member && sub) {
                        evaluated.properties.insert(propKeys[i]);
                        validateCtx(*member, *sub, pointerAppend(path, propKeys[i]), errors, ctx);
                    }
                    if (ctx.aborted)
                        return false;
                }
            }

            const pjson* patternProps = schema.find("patternProperties");
            std::set<std::string> patternMatched;
            if (patternProps && patternProps->isObject()) {
                const std::vector<std::string> patKeys = patternProps->keys();
                for (size_t p = 0; p < patKeys.size(); ++p) {
                    const pjson* patSchema = patternProps->find(patKeys[p]);
                    for (size_t i = 0; i < memberKeys.size(); ++i) {
                        if (!chargeLoopWork(ctx, errors, path))
                            return false;
                        bool matches = false;
                        if (evaluateRegex(memberKeys[i], patKeys[p],
                                          pointerAppend(path, memberKeys[i]), errors, ctx,
                                          matches) &&
                            matches) {
                            patternMatched.insert(memberKeys[i]);
                            evaluated.properties.insert(memberKeys[i]);
                            const pjson* member = node.find(memberKeys[i]);
                            if (member && patSchema)
                                validateCtx(*member, *patSchema, pointerAppend(path, memberKeys[i]),
                                            errors, ctx);
                        }
                        if (ctx.aborted)
                            return false;
                    }
                }
            }

            if (const pjson* propertyNames = schema.find("propertyNames")) {
                for (size_t i = 0; i < memberKeys.size(); ++i) {
                    if (!chargeLoopWork(ctx, errors, path))
                        return false;
                    pjson nameValue;
                    nameValue = memberKeys[i];
                    validateCtx(nameValue, *propertyNames, pointerAppend(path, memberKeys[i]),
                                errors, ctx);
                    if (ctx.aborted)
                        return false;
                }
            }

            const pjson* dependentRequired = schema.find("dependentRequired");
            if (dependentRequired && dependentRequired->isObject()) {
                const std::vector<std::string> depKeys = dependentRequired->keys();
                for (size_t d = 0; d < depKeys.size(); ++d) {
                    if (!chargeLoopWork(ctx, errors, path))
                        return false;
                    const pjson* list = dependentRequired->find(depKeys[d]);
                    if (!node.hasKey(depKeys[d]) || list == nullptr || !list->isArray())
                        continue;
                    for (size_t i = 0; i < list->size(); ++i) {
                        if (!chargeLoopWork(ctx, errors, path))
                            return false;
                        const pjson* required = list->find(static_cast<int>(i));
                        if (required && required->isString() && !node.hasKey(strOf(*required)))
                            errors.push_back(SchemaError(path, "property \"" + depKeys[d] +
                                                                   "\" requires property \"" +
                                                                   strOf(*required) + "\""));
                    }
                }
            }

            const pjson* dependencies = schema.find("dependencies");
            if (dependencies && dependencies->isObject()) {
                const std::vector<std::string> depKeys = dependencies->keys();
                for (size_t d = 0; d < depKeys.size(); ++d) {
                    if (!chargeLoopWork(ctx, errors, path))
                        return false;
                    if (!node.hasKey(depKeys[d]))
                        continue;
                    const pjson* dep = dependencies->find(depKeys[d]);
                    if (dep == nullptr)
                        continue;
                    if (dep->isArray()) {
                        for (size_t i = 0; i < dep->size(); ++i) {
                            if (!chargeLoopWork(ctx, errors, path))
                                return false;
                            const pjson* required = dep->find(static_cast<int>(i));
                            if (required && required->isString() && !node.hasKey(strOf(*required)))
                                errors.push_back(SchemaError(path, "property \"" + depKeys[d] +
                                                                       "\" requires property \"" +
                                                                       strOf(*required) + "\""));
                        }
                    } else {
                        SchemaAnnotations dependencyAnnotations;
                        const bool dependencyValid =
                            validateCtx(node, *dep, path, errors, ctx, nullptr, std::string(),
                                        &dependencyAnnotations);
                        if (dependencyValid)
                            evaluated.merge(dependencyAnnotations);
                        if (ctx.aborted)
                            return false;
                    }
                }
            }

            if (const pjson* addl = schema.find("additionalProperties")) {
                for (size_t i = 0; i < memberKeys.size(); ++i) {
                    if (!chargeLoopWork(ctx, errors, path))
                        return false;
                    const bool declared =
                        props && props->isObject() && props->hasKey(memberKeys[i]);
                    const bool matched = patternMatched.find(memberKeys[i]) != patternMatched.end();
                    if (declared || matched)
                        continue;
                    evaluated.properties.insert(memberKeys[i]);
                    if (addl->isBool()) {
                        if (!boolOf(*addl))
                            errors.push_back(SchemaError(pointerAppend(path, memberKeys[i]),
                                                         "additional property \"" + memberKeys[i] +
                                                             "\" is not allowed"));
                    } else {
                        const pjson* member = node.find(memberKeys[i]);
                        if (member)
                            validateCtx(*member, *addl, pointerAppend(path, memberKeys[i]), errors,
                                        ctx);
                    }
                    if (ctx.aborted)
                        return false;
                }
            }

            // ---- dependentSchemas ----
            const pjson* dependentSchemas = schema.find("dependentSchemas");
            if (dependentSchemas && dependentSchemas->isObject()) {
                const std::vector<std::string> depKeys = dependentSchemas->keys();
                for (size_t d = 0; d < depKeys.size(); ++d) {
                    if (!chargeLoopWork(ctx, errors, path))
                        return false;
                    if (!node.hasKey(depKeys[d]))
                        continue;
                    const pjson* dep = dependentSchemas->find(depKeys[d]);
                    if (dep) {
                        SchemaAnnotations dependencyAnnotations;
                        const bool dependencyValid =
                            validateCtx(node, *dep, path, errors, ctx, nullptr, std::string(),
                                        &dependencyAnnotations);
                        if (dependencyValid)
                            evaluated.merge(dependencyAnnotations);
                    }
                    if (ctx.aborted)
                        return false;
                }
            }
        }

        // ---- if / then / else ----
        if (const pjson* ifSchema = schema.find("if")) {
            std::vector<SchemaError> scratch;
            ErrorSink scratchSink(scratch, ctx, false);
            SchemaAnnotations conditionalAnnotations;
            const bool matched = validateCtx(node, *ifSchema, path, scratchSink, ctx, nullptr,
                                             std::string(), &conditionalAnnotations);
            if (ctx.aborted)
                return false;
            if (matched) {
                evaluated.merge(conditionalAnnotations);
                if (const pjson* thenSchema = schema.find("then")) {
                    SchemaAnnotations branchAnnotations;
                    const bool branchValid =
                        validateCtx(node, *thenSchema, path, errors, ctx, nullptr, std::string(),
                                    &branchAnnotations);
                    if (branchValid) {
                        evaluated.merge(branchAnnotations);
                    }
                    if (ctx.aborted)
                        return false;
                }
            } else {
                if (const pjson* elseSchema = schema.find("else")) {
                    SchemaAnnotations branchAnnotations;
                    const bool branchValid =
                        validateCtx(node, *elseSchema, path, errors, ctx, nullptr, std::string(),
                                    &branchAnnotations);
                    if (branchValid)
                        evaluated.merge(branchAnnotations);
                    if (ctx.aborted)
                        return false;
                }
            }
        }

        // ---- logical combinators ----
        if (const pjson* allOf = schema.find("allOf")) {
            if (allOf->isArray()) {
                for (size_t i = 0; i < allOf->size(); ++i) {
                    if (!chargeLoopWork(ctx, errors, path))
                        return false;
                    const pjson* sub = allOf->find(static_cast<int>(i));
                    if (sub) {
                        SchemaAnnotations branchAnnotations;
                        const bool branchValid = validateCtx(node, *sub, path, errors, ctx, nullptr,
                                                             std::string(), &branchAnnotations);
                        if (branchValid)
                            evaluated.merge(branchAnnotations);
                    }
                    if (ctx.aborted)
                        return false;
                }
            }
        }
        if (const pjson* anyOf = schema.find("anyOf")) {
            if (anyOf->isArray()) {
                bool any = false;
                for (size_t i = 0; i < anyOf->size(); ++i) {
                    if (!chargeLoopWork(ctx, errors, path))
                        return false;
                    const pjson* sub = anyOf->find(static_cast<int>(i));
                    if (sub == nullptr)
                        continue;
                    std::vector<SchemaError> scratch;
                    ErrorSink scratchSink(scratch, ctx, false);
                    SchemaAnnotations branchAnnotations;
                    if (validateCtx(node, *sub, path, scratchSink, ctx, nullptr, std::string(),
                                    &branchAnnotations)) {
                        any = true;
                        evaluated.merge(branchAnnotations);
                    }
                    if (ctx.aborted)
                        return false;
                }
                if (!any)
                    errors.push_back(SchemaError(path, "value does not match any schema in anyOf"));
            }
        }
        if (const pjson* oneOf = schema.find("oneOf")) {
            if (oneOf->isArray()) {
                int matches = 0;
                SchemaAnnotations matchingAnnotations;
                for (size_t i = 0; i < oneOf->size(); ++i) {
                    if (!chargeLoopWork(ctx, errors, path))
                        return false;
                    const pjson* sub = oneOf->find(static_cast<int>(i));
                    if (sub == nullptr)
                        continue;
                    std::vector<SchemaError> scratch;
                    ErrorSink scratchSink(scratch, ctx, false);
                    SchemaAnnotations branchAnnotations;
                    if (validateCtx(node, *sub, path, scratchSink, ctx, nullptr, std::string(),
                                    &branchAnnotations)) {
                        ++matches;
                        matchingAnnotations = branchAnnotations;
                    }
                    if (ctx.aborted)
                        return false;
                }
                if (matches != 1)
                    errors.push_back(
                        SchemaError(path, "value matched " + std::to_string(matches) +
                                              " schemas in oneOf (exactly 1 required)"));
                else
                    evaluated.merge(matchingAnnotations);
            }
        }
        const pjson* nots = schema.find("not");
        if (nots != nullptr && (nots->isBool() || nots->isObject())) {
            std::vector<SchemaError> scratch;
            ErrorSink scratchSink(scratch, ctx, false);
            if (validateCtx(node, *nots, path, scratchSink, ctx))
                errors.push_back(SchemaError(path, "value must not match the \"not\" schema"));
            if (ctx.aborted)
                return false;
        }

        if (node.isObject()) {
            if (const pjson* unevaluated = schema.find("unevaluatedProperties")) {
                const std::vector<std::string> keys = node.keys();
                for (size_t i = 0; i < keys.size(); ++i) {
                    if (evaluated.properties.find(keys[i]) != evaluated.properties.end())
                        continue;
                    if (!chargeLoopWork(ctx, errors, path))
                        return false;
                    const pjson* member = node.find(keys[i]);
                    const std::string memberPath = pointerAppend(path, keys[i]);
                    if (unevaluated->isBool() && !boolOf(*unevaluated)) {
                        errors.push_back(
                            SchemaError(memberPath, "unevaluated property is not allowed"));
                    } else if (member != nullptr) {
                        validateCtx(*member, *unevaluated, memberPath, errors, ctx);
                    }
                    evaluated.properties.insert(keys[i]);
                    if (ctx.aborted)
                        return false;
                }
            }
        }

        if (node.isArray()) {
            if (const pjson* unevaluated = schema.find("unevaluatedItems")) {
                for (size_t i = 0; i < node.size(); ++i) {
                    if (evaluated.items.find(i) != evaluated.items.end())
                        continue;
                    if (!chargeLoopWork(ctx, errors, path))
                        return false;
                    const pjson* item = node.find(static_cast<int>(i));
                    const std::string itemPath = pointerAppend(path, std::to_string(i));
                    if (unevaluated->isBool() && !boolOf(*unevaluated)) {
                        errors.push_back(SchemaError(itemPath, "unevaluated item is not allowed"));
                    } else if (item != nullptr) {
                        validateCtx(*item, *unevaluated, itemPath, errors, ctx);
                    }
                    evaluated.items.insert(i);
                    if (ctx.aborted)
                        return false;
                }
            }
        }

        return !ctx.aborted && errors.size() == before;
    }

    // Runs one noexcept validation session over a compiled schema.
    bool runValidation(const pjson& node, const pjson& schema, std::vector<SchemaError>& errors,
                       const Options& options, const CompiledSchemaIndex& compiled) noexcept {
        try {
            ValidationCtx ctx(schema, options, compiled, &errors);
            ErrorSink sink(errors, ctx);
            return validateCtx(node, schema, "", sink, ctx);
        } catch (const SchemaBudgetExceeded&) {
            return false;
        } catch (const std::bad_alloc&) {
            bestEffortSchemaError(errors, "", "schema validation ran out of memory");
        } catch (const std::exception&) {
            bestEffortSchemaError(errors, "",
                                  "schema validation failed with an internal exception");
        } catch (...) {
            bestEffortSchemaError(errors, "", "schema validation failed with an unknown exception");
        }
        return false;
    }

} // namespace

//===----------------------------------------------------------------------===//
// Public pJsonSchemaValidator surface
//===----------------------------------------------------------------------===//
struct pJsonSchemaValidator::Impl {
    pjson schema;
    Options options;
    std::string dialect;
    std::vector<Error> schemaErrors;
    CompiledSchemaIndex compiled;

    Impl(const pjson& aSchema, const Options& aOptions)
            : options(aOptions) {
        // copyFrom() preserves this default-constructed destination allocator,
        // so the validator never borrows the caller's allocator lifetime.
        schema.copyFrom(aSchema);
        compileDialectContract(schema, options, dialect, schemaErrors);
        if (!schemaErrors.empty()) {
            options.resolver = nullptr;
            options.resolverContext = nullptr;
            return;
        }
        const std::string retrievalBase = stripFragment(options.retrievalUri);
        compiled.resources[retrievalBase] = SchemaResource(&schema, retrievalBase);
        compileSchemaResource(schema, &schema, retrievalBase, compiled, schemaErrors, options, "");
        compileExternalResources(compiled, options, schemaErrors);
        validateCompiledReferences(compiled, options, schemaErrors);
        // Resolver state is construction-only. Do not retain an application
        // context pointer that may become dangling after compilation finishes.
        options.resolver = nullptr;
        options.resolverContext = nullptr;
    }
};

pJsonSchemaValidator::Error::Error()
        : category(InstanceValidation) {}
pJsonSchemaValidator::Error::Error(const std::string& aPath, const std::string& aMsg,
                                   Category aCategory)
        : path(aPath)
        , message(aMsg)
        , category(aCategory) {}

pJsonSchemaValidator::Options::Options()
        : maxRegexPatternBytes(256)
        , maxRegexSubjectBytes(4096)
        , allowUnsafeRegex(false)
        , maxValidationDepth(kSchemaValidationDepthHardLimit)
        , maxRefResolutions(1024)
        , maxValidationWork(1000000)
        , maxErrors(100)
        , validateFormats(true)
        , strictSubset(false)
        , refSiblings(false)
        , retrievalUri()
        , defaultDialectUri(kDocumentedSubsetDialect)
        , resolver(nullptr)
        , resolverContext(nullptr)
        , maxResolvedDocuments(32)
        , maxResolvedBytes(size_t(16) * 1024 * 1024) {}

/*static*/
pJsonSchemaValidator::Options pJsonSchemaValidator::Options::trustedRegex() {
    Options o;
    o.maxRegexPatternBytes = 0;
    o.maxRegexSubjectBytes = 0;
    o.allowUnsafeRegex = true;
    return o;
}

/*static*/
pJsonSchemaValidator::Options pJsonSchemaValidator::Options::strict() {
    Options o;
    o.strictSubset = true;
    return o;
}

/*static*/
pJsonSchemaValidator::Options pJsonSchemaValidator::Options::modernSubset() {
    Options o;
    o.refSiblings = true;
    o.validateFormats = false; // Draft 2020-12's default format vocabulary is annotation-only.
    return o;
}

pJsonSchemaValidator::pJsonSchemaValidator(const pjson& aSchema, const Options& aOptions)
        : _impl(new Impl(aSchema, aOptions)) {}

pJsonSchemaValidator::~pJsonSchemaValidator() {
    delete _impl;
}

bool pJsonSchemaValidator::validate(const pjson& aInstance) const noexcept {
    if (!isSchemaValid())
        return false;
    std::vector<Error> errors;
    return runValidation(aInstance, _impl->schema, errors, _impl->options, _impl->compiled);
}

bool pJsonSchemaValidator::validate(const pjson& aInstance,
                                    std::vector<Error>& aErrors) const noexcept {
    if (!isSchemaValid()) {
        try {
            aErrors.insert(aErrors.end(), _impl->schemaErrors.begin(), _impl->schemaErrors.end());
        } catch (...) {
            // The invalid-schema result remains reliable even when the
            // best-effort diagnostic copy cannot allocate.
            return false;
        }
        return false;
    }
    return runValidation(aInstance, _impl->schema, aErrors, _impl->options, _impl->compiled);
}

/*static*/
const char* pJsonSchemaValidator::documentedSubsetDialectUri() noexcept {
    return kDocumentedSubsetDialect;
}

/*static*/
const char* pJsonSchemaValidator::documentedSubsetVocabularyUri() noexcept {
    return kDocumentedSubsetVocabulary;
}

bool pJsonSchemaValidator::isSchemaValid() const noexcept {
    return _impl->schemaErrors.empty();
}

const std::vector<pJsonSchemaValidator::Error>&
pJsonSchemaValidator::schemaErrors() const noexcept {
    return _impl->schemaErrors;
}

const std::string& pJsonSchemaValidator::dialect() const noexcept {
    return _impl->dialect;
}

const pjson& pJsonSchemaValidator::schema() const noexcept {
    return _impl->schema;
}

const pJsonSchemaValidator::Options& pJsonSchemaValidator::options() const noexcept {
    return _impl->options;
}
