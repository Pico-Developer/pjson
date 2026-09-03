// SPDX-FileCopyrightText: 2026 ByteDance Ltd. and/or its affiliates
// SPDX-License-Identifier: Apache-2.0
//
// JSON serialization, output validation, and floating-point formatting.

#include "pjson_internal.h"
#include "ryu/ryu.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <new>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <utility>

using namespace ByteDance;

namespace {
    // Publishes a structured serialization failure without weakening the
    // noexcept contract if storing the optional diagnostic text allocates.
    void setSerializeError(pjson::SerializeError& aError, pjson::SerializeError::Code aCode,
                           const char* aMessage) noexcept {
        aError.code = aCode;
        try {
            aError.message = aMessage;
        } catch (...) {
            aError.message.clear();
        }
    }
} // namespace

pjson::SerializeOptions::SerializeOptions()
        : pretty(false)
        , indentWidth(2)
        , indentCharacter(' ')
        , escapeNonAscii(false)
        , keyOrder(AscendingKeys)
        , nonFinite(RejectNonFinite)
        , maxOutputBytes(size_t(64) * 1024U * 1024U) {}

pjson::SerializeOptions pjson::SerializeOptions::prettyPrinted() {
    SerializeOptions options;
    options.pretty = true;
    return options;
}

pjson::SerializeError::SerializeError()
        : code(None) {}

void pjson::SerializeError::reset() noexcept {
    code = None;
    message.clear();
}

// Formats a finite double with Ryu's proven shortest-round-trip conversion.
// A '.0' suffix is appended when the result would otherwise look like an
// integer, so the value re-parses into the double representation (type-stable).
/*static*/
std::string pjsonImpl::_formatDouble(double aValue) {
    if (!std::isfinite(aValue)) {
        // JSON has no representation for NaN/Infinity.
        return "null";
    }
    char buffer[32];
    const int length = d2s_buffered_n(aValue, buffer);
    std::string result(buffer, static_cast<size_t>(length));
    for (size_t i = 0; i < result.size(); ++i) {
        if (result[i] == 'E')
            result[i] = 'e';
    }
    const size_t exponent = result.find('e');
    if (exponent != std::string::npos) {
        size_t cursor = exponent + 1;
        bool negativeExponent = false;
        if (cursor < result.size() && (result[cursor] == '+' || result[cursor] == '-')) {
            negativeExponent = result[cursor] == '-';
            ++cursor;
        }
        int exponentValue = 0;
        while (cursor < result.size()) {
            exponentValue = exponentValue * 10 + (result[cursor] - '0');
            ++cursor;
        }
        if (negativeExponent)
            exponentValue = -exponentValue;
        if (exponentValue >= -4 && exponentValue < std::numeric_limits<double>::digits10) {
            const bool negative = !result.empty() && result[0] == '-';
            const size_t mantissaBegin = negative ? 1 : 0;
            std::string digits = result.substr(mantissaBegin, exponent - mantissaBegin);
            const size_t dot = digits.find('.');
            if (dot != std::string::npos)
                digits.erase(dot, 1);
            const int decimalPosition = 1 + exponentValue;
            if (decimalPosition <= 0) {
                digits.insert(0, static_cast<size_t>(-decimalPosition), '0');
                digits.insert(0, "0.");
            } else if (static_cast<size_t>(decimalPosition) >= digits.size()) {
                digits.append(static_cast<size_t>(decimalPosition) - digits.size(), '0');
            } else {
                digits.insert(static_cast<size_t>(decimalPosition), 1, '.');
            }
            result = negative ? "-" + digits : digits;
        }
    }
    if (result.find_first_of(".eE") == std::string::npos) {
        result += ".0";
    }
    return result;
}
namespace {
    //===------------------------------------------------------------------===//
    // Serializer sink adapters
    //
    // The serializer targets this tiny common protocol. String failures throw
    // as normal allocation/length errors; stream failures set failbit and are
    // returned as false. This keeps traversal and escaping logic identical.
    //===------------------------------------------------------------------===//

    // Appends serialized bytes directly to a caller-owned string.
    class StringSink {
    public:
        // Binds to the output string without clearing its existing contents.
        StringSink(std::string& aOut, size_t aLimit)
                : _out(aOut)
                , _limit(aLimit)
                , _written(0) {}

        // Appends one byte.
        void put(char aChar) {
            account(1);
            _out += aChar;
        }
        // Appends an exact byte range.
        void write(const char* aData, size_t aSize) {
            account(aSize);
            _out.append(aData, aSize);
        }
        // Appends repeated indentation, preserving std::string's length checks.
        bool repeat(char aChar, size_t aCount) {
            // std::string::append performs the correct max_size check and
            // throws std::length_error before attempting an impossible
            // allocation. This keeps pathological indentation options from
            // turning into an effectively unbounded byte-at-a-time loop.
            account(aCount);
            _out.append(aCount, aChar);
            return true;
        }
        // Converts arithmetic overflow in indentation sizing into a length error.
        bool fail() { throw std::length_error("JSON indentation exceeds string limits"); }
        // A std::string cannot report failure state, so reject invalid UTF-8 by exception.
        bool invalidUtf8() { throw std::invalid_argument("JSON string contains invalid UTF-8"); }
        // Non-finite double under the RejectNonFinite policy: report by exception.
        bool invalidNumber() { throw std::invalid_argument("JSON number is not finite"); }
        // A live string sink has no independent error state.
        explicit operator bool() const { return true; }

    private:
        void account(size_t amount) {
            if (_limit != 0 && amount > _limit - std::min(_written, _limit))
                throw std::length_error("JSON output exceeds maxOutputBytes");
            _written += amount;
        }

        std::string& _out;
        size_t _limit;
        size_t _written;
    };

    // Runs the exact serializer without retaining bytes. This preflight keeps
    // logical failures (invalid UTF-8, indentation overflow, and output-budget
    // exhaustion) from partially modifying a caller's stream.
    class CountingSink {
    public:
        explicit CountingSink(size_t aLimit)
                : _limit(aLimit)
                , _written(0)
                , _valid(true)
                , _invalidUtf8(false)
                , _invalidNumber(false) {}

        void put(char) { account(1); }
        void write(const char*, size_t aSize) { account(aSize); }
        bool repeat(char, size_t aCount) { return account(aCount); }
        bool fail() {
            _valid = false;
            return false;
        }
        bool invalidUtf8() {
            _invalidUtf8 = true;
            return fail();
        }
        bool invalidNumber() {
            _invalidNumber = true;
            return fail();
        }
        explicit operator bool() const { return _valid; }
        size_t size() const { return _written; }
        bool hasInvalidUtf8() const { return _invalidUtf8; }
        bool hasInvalidNumber() const { return _invalidNumber; }

    private:
        bool account(size_t aAmount) {
            if (!_valid)
                return false;
            if (aAmount > std::numeric_limits<size_t>::max() - _written ||
                (_limit != 0 && aAmount > _limit - std::min(_written, _limit))) {
                _valid = false;
                return false;
            }
            _written += aAmount;
            return true;
        }

        size_t _limit;
        size_t _written;
        bool _valid;
        bool _invalidUtf8;
        bool _invalidNumber;
    };

    // Writes serialized bytes incrementally and reflects ostream failure state.
    class StreamSink {
    public:
        // Binds to a caller-owned stream without changing its formatting flags.
        StreamSink(std::ostream& aOut, size_t aLimit)
                : _out(aOut)
                , _limit(aLimit)
                , _written(0) {}

        // Writes one byte through the stream buffer.
        void put(char aChar) {
            if (account(1))
                _out.put(aChar);
        }
        // Writes an exact byte range.
        void write(const char* aData, size_t aSize) {
            if (aSize > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
                _out.setstate(std::ios::failbit);
                return;
            }
            if (!account(aSize))
                return;
            _out.write(aData, static_cast<std::streamsize>(aSize));
        }
        // Emits indentation in bounded blocks and rejects counts that cannot be
        // represented by ostream::write's streamsize parameter.
        bool repeat(char aChar, size_t aCount) {
            const size_t maxWrite =
                static_cast<size_t>(std::numeric_limits<std::streamsize>::max());
            if (aCount > maxWrite || !account(aCount)) {
                if (_out)
                    _out.setstate(std::ios::failbit);
                return false;
            }

            char block[256];
            std::memset(block, static_cast<unsigned char>(aChar), sizeof(block));
            while (aCount != 0 && _out) {
                const size_t amount = std::min(aCount, sizeof(block));
                _out.write(block, static_cast<std::streamsize>(amount));
                aCount -= amount;
            }
            return static_cast<bool>(_out);
        }
        // Marks non-I/O serializer failures in the stream's normal error state.
        bool fail() {
            _out.setstate(std::ios::failbit);
            return false;
        }
        // Streaming reports invalid programmatic string data through failbit.
        bool invalidUtf8() { return fail(); }
        // Streaming reports a non-finite double (RejectNonFinite) through failbit.
        bool invalidNumber() { return fail(); }
        // Exposes the underlying stream state to generic serializer code.
        explicit operator bool() const { return static_cast<bool>(_out); }

    private:
        bool account(size_t amount) {
            if (_limit != 0 && amount > _limit - std::min(_written, _limit)) {
                _out.setstate(std::ios::failbit);
                return false;
            }
            _written += amount;
            return true;
        }

        std::ostream& _out;
        size_t _limit;
        size_t _written;
    };

    template <typename Sink>
    // Writes depth * indentWidth characters after checking multiplication overflow.
    bool writeIndent(Sink& out, size_t depth, const pjson::SerializeOptions& opts) {
        const char indent = opts.indentCharacter == '\t' ? '\t' : ' ';
        if (opts.indentWidth != 0 && depth > size_t(-1) / opts.indentWidth)
            return out.fail();
        const size_t count = depth * opts.indentWidth;
        return out.repeat(indent, count);
    }

    // Writes one UTF-16 code unit in canonical lower-case \uXXXX form.
    template <typename Sink> bool writeUnicodeEscape(Sink& out, uint16_t value) {
        static const char hex[] = "0123456789abcdef";
        char escape[6] = {'\\',
                          'u',
                          hex[(value >> 12U) & 0x0FU],
                          hex[(value >> 8U) & 0x0FU],
                          hex[(value >> 4U) & 0x0FU],
                          hex[value & 0x0FU]};
        out.write(escape, sizeof(escape));
        return static_cast<bool>(out);
    }
} // namespace

template <typename Sink>
// Writes a JSON string body, optionally converting every non-ASCII code point
// to one UTF-16 escape (or a surrogate pair) without adding surrounding quotes.
bool pjsonImpl::_writeEscapedTo(Sink& aOut, const std::string& aIn, bool bEscapeNonAscii) {
    size_t i = 0;
    while (i < aIn.size()) {
        const unsigned char ch = static_cast<unsigned char>(aIn[i]);
        const char* escape = nullptr;
        switch (ch) {
            case '"':
                escape = "\\\"";
                break;
            case '\\':
                escape = "\\\\";
                break;
            case '\b':
                escape = "\\b";
                break;
            case '\f':
                escape = "\\f";
                break;
            case '\n':
                escape = "\\n";
                break;
            case '\r':
                escape = "\\r";
                break;
            case '\t':
                escape = "\\t";
                break;
            default:
                break;
        }
        if (escape) {
            aOut.write(escape, 2);
            ++i;
            if (!aOut)
                return false;
            continue;
        }
        if (ch < 0x20) {
            if (!writeUnicodeEscape(aOut, static_cast<uint16_t>(ch)))
                return false;
            ++i;
            continue;
        }
        if (ch < 0x80) {
            aOut.put(static_cast<char>(ch));
            ++i;
            if (!aOut)
                return false;
            continue;
        }

        const int byteCount = _utf8Len(aIn.data(), i, aIn.size());
        if (byteCount == 0)
            return aOut.invalidUtf8();
        if (!bEscapeNonAscii) {
            aOut.write(aIn.data() + i, static_cast<size_t>(byteCount));
            i += static_cast<size_t>(byteCount);
            if (!aOut)
                return false;
            continue;
        }

        uint32_t codePoint = ch & (byteCount == 2 ? 0x1FU : byteCount == 3 ? 0x0FU : 0x07U);
        for (int k = 1; k < byteCount; ++k)
            codePoint = (codePoint << 6U) | (static_cast<unsigned char>(aIn[i + k]) & 0x3FU);
        if (codePoint <= 0xFFFFU) {
            if (!writeUnicodeEscape(aOut, static_cast<uint16_t>(codePoint)))
                return false;
        } else {
            codePoint -= 0x10000U;
            const uint16_t high = static_cast<uint16_t>(0xD800U + (codePoint >> 10U));
            const uint16_t low = static_cast<uint16_t>(0xDC00U + (codePoint & 0x3FFU));
            if (!writeUnicodeEscape(aOut, high) || !writeUnicodeEscape(aOut, low))
                return false;
        }
        i += static_cast<size_t>(byteCount);
    }
    return static_cast<bool>(aOut);
}

template <typename Sink>
// Emits a scalar or an empty container immediately. For a non-empty container,
// emits its opening delimiter and pushes a frame whose cursor is at its first
// child; the caller owns closing it after all children have been traversed.
bool pjsonImpl::_openOrEmit(Sink& aOut, const pjson* aValue, size_t aDepth,
                            const pjson::SerializeOptions& aOpts,
                            std::vector<SerializeFrame>& aFrames) {
    switch (aValue->_eType) {
        case jsonType::jsonNull:
            aOut.write("null", 4);
            return static_cast<bool>(aOut);
        case jsonType::jsonString:
            aOut.put('"');
            if (!aOut ||
                !_writeEscapedTo(aOut, *aValue->_uValue._pValueString, aOpts.escapeNonAscii))
                return false;
            aOut.put('"');
            return static_cast<bool>(aOut);
        case jsonType::jsonNumberInt: {
            const std::string text = std::to_string(aValue->_uValue._valueInt);
            aOut.write(text.data(), text.size());
            return static_cast<bool>(aOut);
        }
        case jsonType::jsonNumberUInt: {
            const std::string text = std::to_string(aValue->_uValue._valueUInt);
            aOut.write(text.data(), text.size());
            return static_cast<bool>(aOut);
        }
        case jsonType::jsonNumberDouble: {
            const double d = aValue->_uValue._valueDouble;
            if (!std::isfinite(d)) {
                switch (aOpts.nonFinite) {
                    case pjson::SerializeOptions::RejectNonFinite:
                        return aOut.invalidNumber();
                    case pjson::SerializeOptions::NonFiniteToNull:
                        aOut.write("null", 4);
                        return static_cast<bool>(aOut);
                    case pjson::SerializeOptions::NonFiniteToString: {
                        const char* text =
                            std::isnan(d) ? "\"NaN\"" : (d < 0 ? "\"-Infinity\"" : "\"Infinity\"");
                        aOut.write(text, std::char_traits<char>::length(text));
                        return static_cast<bool>(aOut);
                    }
                }
            }
            const std::string text = _formatDouble(d);
            aOut.write(text.data(), text.size());
            return static_cast<bool>(aOut);
        }
        case jsonType::jsonBoolean:
            if (aValue->_uValue._valueBool)
                aOut.write("true", 4);
            else
                aOut.write("false", 5);
            return static_cast<bool>(aOut);
        case jsonType::jsonArray:
            if (aValue->_uValue._pValueArray->empty()) {
                aOut.write("[]", 2);
                return static_cast<bool>(aOut);
            }
            aOut.put('[');
            break;
        case jsonType::jsonObject:
            if (aValue->_uValue._pValueMap->empty()) {
                aOut.write("{}", 2);
                return static_cast<bool>(aOut);
            }
            aOut.put('{');
            break;
    }
    if (!aOut)
        return false;

    SerializeFrame frame;
    frame.isObject = aValue->_eType == jsonType::jsonObject;
    frame.depth = aDepth;
    frame.first = true;
    frame.array = frame.isObject ? nullptr : aValue->_uValue._pValueArray;
    frame.arrayIndex = 0;
    frame.object = frame.isObject ? aValue->_uValue._pValueMap : nullptr;
    if (frame.isObject) {
        frame.objectIt = frame.object->begin();
        frame.objectReverseIt = frame.object->rbegin();
    }
    aFrames.push_back(frame);
    return true;
}

template <typename Sink>
// Serializes without recursive C++ calls. Before descending, the parent cursor
// advances past the chosen child, so a pushed child frame cannot invalidate the
// parent's progress when the vector reallocates.
bool pjsonImpl::_writeValueTo(Sink& aOut, const pjson& aValue,
                              const pjson::SerializeOptions& aOpts) {
    std::vector<SerializeFrame> stack;
    stack.reserve(32);
    if (!_openOrEmit(aOut, &aValue, 0, aOpts, stack))
        return false;

    while (!stack.empty()) {
        SerializeFrame& frame = stack.back();
        const pjson* child = nullptr;
        const std::string* key = nullptr;
        bool hasNext = false;
        if (frame.isObject) {
            if (aOpts.keyOrder == pjson::SerializeOptions::DescendingKeys) {
                hasNext = frame.objectReverseIt != frame.object->rend();
                if (hasNext) {
                    key = &frame.objectReverseIt->first;
                    child = frame.objectReverseIt->second;
                }
            } else {
                hasNext = frame.objectIt != frame.object->end();
                if (hasNext) {
                    key = &frame.objectIt->first;
                    child = frame.objectIt->second;
                }
            }
        } else {
            hasNext = frame.arrayIndex < frame.array->size();
            if (hasNext)
                child = (*frame.array)[frame.arrayIndex];
        }

        if (hasNext) {
            if (!frame.first)
                aOut.put(',');
            frame.first = false;
            const size_t childDepth = frame.depth + 1;
            const bool isObject = frame.isObject;
            if (isObject) {
                if (aOpts.keyOrder == pjson::SerializeOptions::DescendingKeys)
                    ++frame.objectReverseIt;
                else
                    ++frame.objectIt;
            } else {
                ++frame.arrayIndex;
            }
            if (aOpts.pretty) {
                aOut.put('\n');
                if (!writeIndent(aOut, childDepth, aOpts))
                    return false;
            }
            if (isObject) {
                aOut.put('"');
                if (!aOut || !_writeEscapedTo(aOut, *key, aOpts.escapeNonAscii))
                    return false;
                if (aOpts.pretty)
                    aOut.write("\": ", 3);
                else
                    aOut.write("\":", 2);
            }
            if (!aOut || !_openOrEmit(aOut, child, childDepth, aOpts, stack))
                return false;
        } else {
            const size_t depth = frame.depth;
            const bool isObject = frame.isObject;
            stack.pop_back();
            if (aOpts.pretty) {
                aOut.put('\n');
                if (!writeIndent(aOut, depth, aOpts))
                    return false;
            }
            aOut.put(isObject ? '}' : ']');
            if (!aOut)
                return false;
        }
    }
    return static_cast<bool>(aOut);
}

/*static*/
// Appends one serialized value to an existing string.
void pjsonImpl::_appendValue(std::string& aOut, const pjson& aValue,
                             const pjson::SerializeOptions& aOpts) {
    StringSink sink(aOut, aOpts.maxOutputBytes);
    _writeValueTo(sink, aValue, aOpts);
}

/*static*/
// Streams one serialized value and returns the resulting stream health.
bool pjsonImpl::_writeValue(std::ostream& aOut, const pjson& aValue,
                            const pjson::SerializeOptions& aOpts) {
    CountingSink count(aOpts.maxOutputBytes);
    if (!_writeValueTo(count, aValue, aOpts)) {
        aOut.setstate(std::ios::failbit);
        return false;
    }
    // Preflight owns the configured budget; emission itself is unlimited so a
    // successful count cannot fail due to double-accounting.
    StreamSink sink(aOut, 0);
    return _writeValueTo(sink, aValue, aOpts);
}

// Serializes with compact default options.
std::string pjson::toString() const {
    return toString(SerializeOptions());
}

// Serializes this complete DOM to a newly allocated string.
std::string pjson::toString(const SerializeOptions& aOpts) const {
    CountingSink count(aOpts.maxOutputBytes);
    if (!pjsonImpl::_writeValueTo(count, *this, aOpts)) {
        if (count.hasInvalidUtf8())
            throw std::invalid_argument("JSON string contains invalid UTF-8");
        if (count.hasInvalidNumber())
            throw std::invalid_argument("JSON number is not finite");
        throw std::length_error("JSON output exceeds maxOutputBytes or contains invalid data");
    }
    std::string result;
    result.reserve(count.size());
    pjsonImpl::_appendValue(result, *this, aOpts);
    return result;
}

// Serializes transactionally into a caller-owned string. The caller's prior
// bytes survive every logical, allocation, or internal failure.
bool pjson::toString(std::string& aOut, SerializeError& aError,
                     const SerializeOptions& aOpts) const noexcept {
    aError.reset();
    try {
        CountingSink count(aOpts.maxOutputBytes);
        if (!pjsonImpl::_writeValueTo(count, *this, aOpts)) {
            if (count.hasInvalidUtf8()) {
                setSerializeError(aError, SerializeError::InvalidUtf8,
                                  "JSON string contains invalid UTF-8");
            } else if (count.hasInvalidNumber()) {
                setSerializeError(aError, SerializeError::NonFiniteNumber,
                                  "JSON number is not finite");
            } else {
                setSerializeError(aError, SerializeError::OutputLimit,
                                  "JSON output exceeds maxOutputBytes or representable size");
            }
            return false;
        }
        std::string result;
        result.reserve(count.size());
        pjsonImpl::_appendValue(result, *this, aOpts);
        aOut.swap(result);
        return true;
    } catch (const std::bad_alloc&) {
        setSerializeError(aError, SerializeError::AllocationFailure,
                          "JSON serialization ran out of memory");
    } catch (const std::length_error& exception) {
        setSerializeError(aError, SerializeError::OutputLimit, exception.what());
    } catch (const std::invalid_argument& exception) {
        setSerializeError(aError, SerializeError::InternalError, exception.what());
    } catch (...) {
        setSerializeError(aError, SerializeError::InternalError,
                          "JSON serialization failed with an internal exception");
    }
    return false;
}

// Streams with compact default options.
void pjson::write(std::ostream& aOut) const {
    write(aOut, SerializeOptions());
}

// Writes this complete DOM incrementally; callers inspect the stream state for
// output errors because the public streaming API reports through std::ostream.
void pjson::write(std::ostream& aOut, const SerializeOptions& aOpts) const {
    pjsonImpl::_writeValue(aOut, *this, aOpts);
}

// Non-throwing stream serialization. Logical failures are detected by the
// existing preflight before emission; only a physical stream failure may have
// emitted a prefix.
bool pjson::write(std::ostream& aOut, SerializeError& aError,
                  const SerializeOptions& aOpts) const noexcept {
    aError.reset();
    try {
        CountingSink count(aOpts.maxOutputBytes);
        if (!pjsonImpl::_writeValueTo(count, *this, aOpts)) {
            if (count.hasInvalidUtf8()) {
                setSerializeError(aError, SerializeError::InvalidUtf8,
                                  "JSON string contains invalid UTF-8");
            } else if (count.hasInvalidNumber()) {
                setSerializeError(aError, SerializeError::NonFiniteNumber,
                                  "JSON number is not finite");
            } else {
                setSerializeError(aError, SerializeError::OutputLimit,
                                  "JSON output exceeds maxOutputBytes or representable size");
            }
            try {
                aOut.setstate(std::ios::failbit);
            } catch (...) {
                // Keep the more precise logical SerializeError category even
                // when the caller enabled stream exceptions for failbit.
                (void)0;
            }
            return false;
        }
        StreamSink sink(aOut, 0);
        if (!pjsonImpl::_writeValueTo(sink, *this, aOpts)) {
            setSerializeError(aError, SerializeError::StreamFailure,
                              "JSON destination stream write failed");
            return false;
        }
        return true;
    } catch (const std::bad_alloc&) {
        setSerializeError(aError, SerializeError::AllocationFailure,
                          "JSON serialization ran out of memory");
    } catch (const std::ios_base::failure& exception) {
        setSerializeError(aError, SerializeError::StreamFailure, exception.what());
    } catch (...) {
        setSerializeError(aError, SerializeError::InternalError,
                          "JSON serialization failed with an internal exception");
    }
    try {
        aOut.setstate(std::ios::failbit);
    } catch (...) {
        // The structured result remains authoritative for this noexcept API.
        (void)0;
    }
    return false;
}
