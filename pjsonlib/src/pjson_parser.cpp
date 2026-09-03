// SPDX-FileCopyrightText: 2026 ByteDance Ltd. and/or its affiliates
// SPDX-License-Identifier: Apache-2.0

#include "pjson_parser_internal.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <locale>
#include <new>
#include <sstream>
#include <streambuf>
#include <utility>

using namespace ByteDance;

namespace {
    int clampParseDepth(int configured) {
        if (configured <= 0)
            return 1;
        return configured < pJsonParserImpl::DepthHardLimit ? configured
                                                            : pJsonParserImpl::DepthHardLimit;
    }

    const pJsonParser::Options& effectiveOptions(const pJsonParserImpl* implementation) noexcept {
        static const pJsonParser::Options defaults;
        return implementation == nullptr ? defaults : implementation->options;
    }

    pjson::Allocator& effectiveAllocator(const pJsonParserImpl* implementation) noexcept {
        return implementation == nullptr ? pjsonImpl::_defaultAllocator()
                                         : *implementation->allocator;
    }
} // namespace

namespace ByteDance {
    pJsonParser::Options::Options()
            : maxDepth(512)
            , maxNodes(1000000)
            , maxInputBytes(size_t(64) * 1024 * 1024)
            , duplicateKeys(RejectDuplicateKeys)
            , numberPolicy(RejectUnrepresentableNumbers) {}

    pJsonParser::Error::Error()
            : ok(true)
            , code(None)
            , offset(0)
            , line(1)
            , column(1) {}

    pJsonParser::SaxHandler::~SaxHandler() {}
    bool pJsonParser::SaxHandler::onNull() {
        return true;
    }
    bool pJsonParser::SaxHandler::onBool(bool) {
        return true;
    }
    bool pJsonParser::SaxHandler::onInt(int64_t) {
        return true;
    }
    bool pJsonParser::SaxHandler::onUInt(uint64_t) {
        return true;
    }
    bool pJsonParser::SaxHandler::onDouble(double) {
        return true;
    }
    bool pJsonParser::SaxHandler::onString(const std::string&) {
        return true;
    }
    bool pJsonParser::SaxHandler::onStartArray() {
        return true;
    }
    bool pJsonParser::SaxHandler::onEndArray() {
        return true;
    }
    bool pJsonParser::SaxHandler::onStartObject() {
        return true;
    }
    bool pJsonParser::SaxHandler::onKey(const std::string&) {
        return true;
    }
    bool pJsonParser::SaxHandler::onEndObject() {
        return true;
    }

    pJsonParser::pJsonParser(const Options& options)
            : _impl(new pJsonParserImpl(pjsonImpl::_defaultAllocator(), options)) {}

    pJsonParser::pJsonParser(pjson::Allocator& allocator, const Options& options)
            : _impl(new pJsonParserImpl(allocator, options)) {}

    pJsonParser::~pJsonParser() {
        delete _impl;
    }

    pJsonParser::pJsonParser(const pJsonParser& other)
            : _impl(new pJsonParserImpl(effectiveAllocator(other._impl),
                                        effectiveOptions(other._impl))) {}

    pJsonParser::pJsonParser(pJsonParser&& other) noexcept
            : _impl(other._impl) {
        other._impl = nullptr;
    }

    pJsonParser& pJsonParser::operator=(const pJsonParser& other) {
        if (this == &other)
            return *this;
        pJsonParserImpl* replacement =
            new pJsonParserImpl(effectiveAllocator(other._impl), effectiveOptions(other._impl));
        delete _impl;
        _impl = replacement;
        return *this;
    }

    pJsonParser& pJsonParser::operator=(pJsonParser&& other) noexcept {
        if (this == &other)
            return *this;
        delete _impl;
        _impl = other._impl;
        other._impl = nullptr;
        return *this;
    }

    const pJsonParser::Options& pJsonParser::options() const noexcept {
        return effectiveOptions(_impl);
    }
    pjson::Allocator& pJsonParser::allocator() const noexcept {
        return effectiveAllocator(_impl);
    }

    pjson pJsonParser::parse(const std::string& input) const {
        return pJsonParserImpl::parseTop(input.c_str(), input.size(), effectiveOptions(_impl),
                                         nullptr, effectiveAllocator(_impl));
    }
    pjson pJsonParser::parse(const char* input, size_t size) const {
        return pJsonParserImpl::parseTop(input, size, effectiveOptions(_impl), nullptr,
                                         effectiveAllocator(_impl));
    }
    pjson pJsonParser::parse(const std::string& input, Error& error) const {
        return pJsonParserImpl::parseTop(input.c_str(), input.size(), effectiveOptions(_impl),
                                         &error, effectiveAllocator(_impl));
    }
    pjson pJsonParser::parse(const char* input, size_t size, Error& error) const {
        return pJsonParserImpl::parseTop(input, size, effectiveOptions(_impl), &error,
                                         effectiveAllocator(_impl));
    }
    pjson pJsonParser::parseStream(std::istream& input) const {
        return pJsonParserImpl::parseStream(input, effectiveOptions(_impl), nullptr,
                                            effectiveAllocator(_impl));
    }
    pjson pJsonParser::parseStream(std::istream& input, Error& error) const {
        return pJsonParserImpl::parseStream(input, effectiveOptions(_impl), &error,
                                            effectiveAllocator(_impl));
    }
    bool pJsonParser::parseSax(const std::string& input, pJsonParser::SaxHandler& handler) const {
        return pJsonParserImpl::parseSaxTop(input.c_str(), input.size(), handler,
                                            effectiveOptions(_impl), nullptr);
    }
    bool pJsonParser::parseSax(const char* input, size_t size,
                               pJsonParser::SaxHandler& handler) const {
        return pJsonParserImpl::parseSaxTop(input, size, handler, effectiveOptions(_impl), nullptr);
    }
    bool pJsonParser::parseSax(const std::string& input, pJsonParser::SaxHandler& handler,
                               Error& error) const {
        return pJsonParserImpl::parseSaxTop(input.c_str(), input.size(), handler,
                                            effectiveOptions(_impl), &error);
    }
    bool pJsonParser::parseSax(const char* input, size_t size, pJsonParser::SaxHandler& handler,
                               Error& error) const {
        return pJsonParserImpl::parseSaxTop(input, size, handler, effectiveOptions(_impl), &error);
    }
    bool pJsonParser::parseSaxStream(std::istream& input, pJsonParser::SaxHandler& handler) const {
        return pJsonParserImpl::parseSaxStream(input, handler, effectiveOptions(_impl), nullptr);
    }
    bool pJsonParser::parseSaxStream(std::istream& input, pJsonParser::SaxHandler& handler,
                                     Error& error) const {
        return pJsonParserImpl::parseSaxStream(input, handler, effectiveOptions(_impl), &error);
    }
} // namespace ByteDance
bool pJsonParserImpl::isWhitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}
// Encodes a Unicode code point as UTF-8 and appends it to aOut.
/*static*/
void pJsonParserImpl::appendUtf8(uint32_t aCodePoint, std::string& aOut) {
    if (aCodePoint <= 0x7F) {
        aOut += static_cast<char>(aCodePoint);
    } else if (aCodePoint <= 0x7FF) {
        aOut += static_cast<char>(0xC0 | (aCodePoint >> 6));
        aOut += static_cast<char>(0x80 | (aCodePoint & 0x3F));
    } else if (aCodePoint <= 0xFFFF) {
        aOut += static_cast<char>(0xE0 | (aCodePoint >> 12));
        aOut += static_cast<char>(0x80 | ((aCodePoint >> 6) & 0x3F));
        aOut += static_cast<char>(0x80 | (aCodePoint & 0x3F));
    } else {
        aOut += static_cast<char>(0xF0 | (aCodePoint >> 18));
        aOut += static_cast<char>(0x80 | ((aCodePoint >> 12) & 0x3F));
        aOut += static_cast<char>(0x80 | ((aCodePoint >> 6) & 0x3F));
        aOut += static_cast<char>(0x80 | (aCodePoint & 0x3F));
    }
}
// Decodes exactly four hexadecimal bytes at aStart into one UTF-16 code unit.
bool pJsonParserImpl::hex4(const char* aSrc, size_t aStart, uint32_t& aOut) {
    aOut = 0;
    for (int k = 0; k < 4; ++k) {
        char h = aSrc[aStart + k];
        aOut <<= 4;
        if (h >= '0' && h <= '9')
            aOut |= static_cast<uint32_t>(h - '0');
        else if (h >= 'a' && h <= 'f')
            aOut |= static_cast<uint32_t>(h - 'a' + 10);
        else if (h >= 'A' && h <= 'F')
            aOut |= static_cast<uint32_t>(h - 'A' + 10);
        else
            return false;
    }
    return true;
}
// Returns the length (1..4) of the valid UTF-8 sequence starting at
// src[pos], or 0 if the bytes there are not valid UTF-8. Requires pos < end.
// Formats a finite double with Ryu's proven shortest-round-trip conversion.
// A '.0' suffix is appended when the result would otherwise look like an
// integer, so the value re-parses into the double representation (type-stable).
/*static*/
bool pJsonParserImpl::parseDouble(const std::string& aText, double& aValue,
                                  bool* aUnderflowToZero) {
    if (aUnderflowToZero != nullptr)
        *aUnderflowToZero = false;
    std::istringstream in(aText);
    in.imbue(std::locale::classic());
    in >> std::noskipws >> aValue;
    const bool cleanParse = !in.fail() && in.peek() == std::char_traits<char>::eof();
    // libstdc++/libc++ set failbit as well as eofbit for both underflow and
    // overflow. Classify the range direction from the decimal exponent instead
    // of trusting the implementation-specific saturated result. A negative
    // effective decimal exponent cannot overflow binary64, so its finite zero or
    // subnormal result is valid; nonnegative range failures are overflow.
    if (!cleanParse && (!in.eof() || !std::isfinite(aValue)))
        return false;
    const size_t signOffset = !aText.empty() && aText[0] == '-' ? 1 : 0;
    const size_t exponentMark = aText.find_first_of("eE");
    const size_t significandEnd = exponentMark == std::string::npos ? aText.size() : exponentMark;
    const size_t point = aText.find('.', signOffset);
    const size_t digitsBeforePoint = point != std::string::npos && point < significandEnd
                                         ? point - signOffset
                                         : significandEnd - signOffset;
    size_t digitOrdinal = 0;
    size_t firstNonzero = std::string::npos;
    for (size_t i = signOffset; i < significandEnd; ++i) {
        if (aText[i] == '.')
            continue;
        if (firstNonzero == std::string::npos && aText[i] != '0')
            firstNonzero = digitOrdinal;
        ++digitOrdinal;
    }
    if (firstNonzero == std::string::npos)
        return true; // exact zero cannot overflow
    if (cleanParse) {
        if (aUnderflowToZero != nullptr && aValue == 0.0)
            *aUnderflowToZero = true;
        return true;
    }

    const int64_t kExponentCap = INT64_C(1000000000);
    int64_t explicitExponent = 0;
    if (exponentMark != std::string::npos) {
        size_t i = exponentMark + 1;
        bool negative = false;
        if (i < aText.size() && (aText[i] == '+' || aText[i] == '-')) {
            negative = aText[i] == '-';
            ++i;
        }
        for (; i < aText.size(); ++i) {
            const int digit = aText[i] - '0';
            if (explicitExponent > (kExponentCap - digit) / 10) {
                explicitExponent = kExponentCap;
                break;
            }
            explicitExponent = explicitExponent * 10 + digit;
        }
        if (negative)
            explicitExponent = -explicitExponent;
    }
    const int64_t baseExponent =
        static_cast<int64_t>(digitsBeforePoint) - static_cast<int64_t>(firstNonzero) - 1;
    const int64_t effectiveExponent = explicitExponent > kExponentCap - baseExponent ? kExponentCap
                                      : explicitExponent < -kExponentCap - baseExponent
                                          ? -kExponentCap
                                          : explicitExponent + baseExponent;
    if (effectiveExponent >= 0)
        return false;
    if (aUnderflowToZero != nullptr && aValue == 0.0)
        *aUnderflowToZero = true;
    return true;
}

// Converts one already grammar-validated number token. Both DOM and SAX use
// this routine so storage classification and lossy-number policy cannot drift.
bool pJsonParserImpl::convertNumberToken(const std::string& aText, bool aIsFloat,
                                         pJsonParser::Options::NumberPolicy aPolicy,
                                         pJsonParserImpl::ParsedNumber& aResult,
                                         const char*& aErrorMessage) {
    aErrorMessage = nullptr;
    const bool allowLossy = aPolicy == pJsonParser::Options::AllowLossyNumbers;
    if (!aIsFloat) {
        errno = 0;
        const long long signedValue = strtoll(aText.c_str(), nullptr, 10);
        if (errno != ERANGE) {
            aResult.kind = pJsonParserImpl::ParsedNumber::SignedInteger;
            aResult.signedValue = static_cast<int64_t>(signedValue);
            return true;
        }
        if (aText.empty() || aText[0] != '-') {
            errno = 0;
            const unsigned long long unsignedValue = strtoull(aText.c_str(), nullptr, 10);
            if (errno != ERANGE) {
                aResult.kind = pJsonParserImpl::ParsedNumber::UnsignedInteger;
                aResult.unsignedValue = static_cast<uint64_t>(unsignedValue);
                return true;
            }
        }
        if (!allowLossy) {
            aErrorMessage = "integer out of range; enable AllowLossyNumbers to store as double";
            return false;
        }
    }

    double floatingValue = 0.0;
    bool underflowToZero = false;
    if (!parseDouble(aText, floatingValue, &underflowToZero) || !std::isfinite(floatingValue)) {
        aErrorMessage = "number out of range";
        return false;
    }
    if (underflowToZero && !allowLossy) {
        aErrorMessage = "number underflows to zero; enable AllowLossyNumbers to permit rounding";
        return false;
    }
    aResult.kind = pJsonParserImpl::ParsedNumber::FloatingPoint;
    aResult.floatingValue = floatingValue;
    return true;
}
namespace {
    //===------------------------------------------------------------------===//
    // Parse diagnostics and SAX cursor adapters
    //===------------------------------------------------------------------===//

    // Converts a zero-based byte offset into one-based source coordinates. CRLF
    // counts as one line ending; a lone CR or LF also starts a new line.
    void lineAndColumn(const char* src, size_t size, size_t offset, size_t& line, size_t& column) {
        line = 1;
        column = 1;
        const size_t end = offset < size ? offset : size;
        for (size_t i = 0; i < end; ++i) {
            if (src[i] == '\r') {
                if (i + 1 < end && src[i + 1] == '\n')
                    ++i;
                ++line;
                column = 1;
            } else if (src[i] == '\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
        }
    }

    // Maps a parser diagnostic message to a stable pJsonParser::Error::Code. The exact
    // message wording may evolve; this keeps the machine-facing category stable
    // by classifying on the well-known phrases the parser emits.
    pJsonParser::Error::Code classifyParseMessage(const std::string& message) {
        if (message.find("UTF-8") != std::string::npos ||
            message.find("surrogate") != std::string::npos ||
            message.find("escape") != std::string::npos || message.find("\\u") != std::string::npos)
            return pJsonParser::Error::InvalidEncoding;
        if (message.find("duplicate object key") != std::string::npos)
            return pJsonParser::Error::DuplicateKey;
        if (message.find("out of range") != std::string::npos ||
            message.find("number") != std::string::npos)
            return pJsonParser::Error::NumberRange;
        if (message.find("nesting depth") != std::string::npos)
            return pJsonParser::Error::DepthLimit;
        if (message.find("maxInputBytes") != std::string::npos)
            return pJsonParser::Error::InputLimit;
        if (message.find("maxNodes") != std::string::npos ||
            message.find("node budget") != std::string::npos)
            return pJsonParser::Error::NodeLimit;
        if (message.find("out of memory") != std::string::npos)
            return pJsonParser::Error::AllocationFailure;
        if (message.find("stream read") != std::string::npos)
            return pJsonParser::Error::StreamError;
        return pJsonParser::Error::Syntax;
    }

    // Publishes a buffer-parser failure, deriving source coordinates from the
    // authoritative byte offset. A null destination intentionally discards it.
    // The code is classified from the message unless an explicit one is given.
    void setParseError(pJsonParser::Error* err, const char* src, size_t size, size_t offset,
                       const std::string& message,
                       pJsonParser::Error::Code code = pJsonParser::Error::None) {
        if (!err)
            return;
        err->ok = false;
        err->code = code == pJsonParser::Error::None ? classifyParseMessage(message) : code;
        err->offset = offset;
        lineAndColumn(src, size, offset, err->line, err->column);
        err->message = message;
    }

    // Restores the public error object to its successful, start-of-input state.
    void resetParseError(pJsonParser::Error* err) {
        if (!err)
            return;
        err->ok = true;
        err->code = pJsonParser::Error::None;
        err->offset = 0;
        err->line = 1;
        err->column = 1;
        err->message.clear();
    }

    // Internal control-flow exception used to unwind immediately when a SAX
    // callback returns false; parseDocument converts it back into pJsonParser::Error.
    class SaxParseCancelled : public std::exception {
    public:
        // Supplies a stable diagnostic if cancellation escapes an internal frame.
        const char* what() const noexcept override { return "SAX parse aborted"; }
    };

    template <typename Cursor>
    bool scanJsonNumber(Cursor& cursor, std::string& text, bool& isFloat,
                        const char*& errorMessage) {
        text.clear();
        isFloat = false;
        errorMessage = nullptr;
        char ch = 0;
        if (!cursor.peek(ch)) {
            errorMessage = "unexpected end of input; expected a value";
            return false;
        }
        if (ch == '-') {
            if (!cursor.take(ch))
                return false;
            text.push_back(ch);
            if (!cursor.peek(ch)) {
                errorMessage = "invalid number: expected digit";
                return false;
            }
        }
        if (ch == '0') {
            if (!cursor.take(ch))
                return false;
            text.push_back(ch);
        } else if (ch >= '1' && ch <= '9') {
            do {
                if (!cursor.take(ch))
                    return false;
                text.push_back(ch);
            } while (cursor.peek(ch) && ch >= '0' && ch <= '9');
        } else {
            errorMessage = "invalid number: expected digit";
            return false;
        }
        if (cursor.peek(ch) && ch == '.') {
            isFloat = true;
            if (!cursor.take(ch))
                return false;
            text.push_back(ch);
            if (!cursor.peek(ch) || ch < '0' || ch > '9') {
                errorMessage = "invalid number: '.' must be followed by a digit";
                return false;
            }
            do {
                if (!cursor.take(ch))
                    return false;
                text.push_back(ch);
            } while (cursor.peek(ch) && ch >= '0' && ch <= '9');
        }
        if (cursor.peek(ch) && (ch == 'e' || ch == 'E')) {
            isFloat = true;
            if (!cursor.take(ch))
                return false;
            text.push_back(ch);
            if (cursor.peek(ch) && (ch == '+' || ch == '-')) {
                if (!cursor.take(ch))
                    return false;
                text.push_back(ch);
            }
            if (!cursor.peek(ch) || ch < '0' || ch > '9') {
                errorMessage = "invalid number: exponent must have a digit";
                return false;
            }
            do {
                if (!cursor.take(ch))
                    return false;
                text.push_back(ch);
            } while (cursor.peek(ch) && ch >= '0' && ch <= '9');
        }
        return true;
    }

    // Non-owning cursor over a contiguous input buffer. Positions are byte
    // offsets, while line/column values are maintained incrementally.
    class BufferSaxCursor {
    public:
        // Binds the cursor to caller-owned bytes, which must outlive parsing.
        BufferSaxCursor(const char* src, size_t size)
                : _src(src)
                , _size(size)
                , _pos(0)
                , _line(1)
                , _column(1)
                , _prevWasCR(false) {}

        // Observes the next byte without advancing source coordinates.
        bool peek(char& ch) {
            if (_pos >= _size)
                return false;
            ch = _src[_pos];
            return true;
        }

        // Consumes one byte and advances CR/LF-aware source coordinates.
        bool get(char& ch) {
            if (!peek(ch))
                return false;
            advance(ch);
            ++_pos;
            return true;
        }

        // Reports whether every byte in the fixed buffer has been consumed.
        bool eof() const { return _pos >= _size; }
        // A memory cursor cannot suffer an I/O failure.
        bool failed() const { return false; }
        // Returns the zero-based byte offset of the next input byte.
        size_t position() const { return _pos; }
        // Returns the one-based line containing the next input byte.
        size_t line() const { return _line; }
        // Returns the one-based column containing the next input byte.
        size_t column() const { return _column; }

    private:
        // Counts CRLF as one newline even though its bytes arrive separately.
        void advance(char ch) {
            if (ch == '\r') {
                ++_line;
                _column = 1;
                _prevWasCR = true;
            } else if (ch == '\n') {
                if (_prevWasCR) {
                    _prevWasCR = false;
                } else {
                    ++_line;
                    _column = 1;
                }
            } else {
                ++_column;
                _prevWasCR = false;
            }
        }

        const char* _src;
        size_t _size;
        size_t _pos;
        size_t _line;
        size_t _column;
        bool _prevWasCR;
    };

    // Buffered cursor that gives the SAX parser the same interface for streams
    // without first materializing the complete input.
    class StreamSaxCursor {
    public:
        // Binds to a caller-owned stream and delays reads until bytes are needed.
        explicit StreamSaxCursor(std::istream& in)
                : _in(in)
                , _used(0)
                , _posInBuf(0)
                , _pos(0)
                , _line(1)
                , _column(1)
                , _prevWasCR(false)
                , _failed(false)
                , _eof(false) {}

        // Observes the next buffered byte, refilling on demand.
        bool peek(char& ch) {
            if (!ensure())
                return false;
            ch = _buffer[_posInBuf];
            return true;
        }

        // Consumes one byte while maintaining absolute and source positions.
        bool get(char& ch) {
            if (!ensure())
                return false;
            ch = _buffer[_posInBuf++];
            if (ch == '\r') {
                ++_line;
                _column = 1;
                _prevWasCR = true;
            } else if (ch == '\n') {
                if (_prevWasCR) {
                    _prevWasCR = false;
                } else {
                    ++_line;
                    _column = 1;
                }
            } else {
                ++_column;
                _prevWasCR = false;
            }
            ++_pos;
            return true;
        }

        // Reports EOF only after both the stream and the refill buffer are empty.
        bool eof() const { return _eof && _posInBuf >= _used; }
        // Distinguishes an I/O failure from an ordinary end of stream.
        bool failed() const { return _failed; }
        // Returns the number of bytes consumed across all refills.
        size_t position() const { return _pos; }
        // Returns the one-based line containing the next input byte.
        size_t line() const { return _line; }
        // Returns the one-based column containing the next input byte.
        size_t column() const { return _column; }

    private:
        // Makes one byte available unless EOF or an unrecoverable read failure
        // has already been observed. Short reads with data are still usable.
        bool ensure() {
            if (_posInBuf < _used)
                return true;
            if (_eof || _failed)
                return false;
            // Pull directly from streambuf so a source that intentionally
            // exposes one short chunk at a time is not mistaken for EOF by
            // istream::read's exact-count semantics. One byte is sufficient for
            // the parser; the streambuf retains any remaining get-area bytes.
            std::streambuf* buffer = _in.rdbuf();
            if (buffer == nullptr || _in.bad()) {
                _failed = true;
                return false;
            }
            const std::streambuf::int_type next = buffer->sbumpc();
            if (!std::streambuf::traits_type::eq_int_type(next,
                                                          std::streambuf::traits_type::eof())) {
                _buffer[0] = std::streambuf::traits_type::to_char_type(next);
                _used = 1;
                _posInBuf = 0;
                return true;
            }
            if (_in.bad()) {
                _failed = true;
                return false;
            }
            _eof = true;
            return false;
        }

        std::istream& _in;
        char _buffer[8192];
        size_t _used;
        size_t _posInBuf;
        size_t _pos;
        size_t _line;
        size_t _column;
        bool _prevWasCR;
        bool _failed;
        bool _eof;
    };

    // Recursive-descent event parser shared by buffer and stream cursors. It
    // applies the same grammar, resource budgets, and duplicate-key policy as
    // DOM parsing, but can suppress callbacks for KeepFirstDuplicate values.
    template <typename Cursor> struct SaxParser {
        Cursor& cur;
        pJsonParser::SaxHandler& handler;
        const pJsonParser::Options& opts;
        pJsonParser::Error* err;
        size_t nodeCount;

        // Couples a cursor and event sink for one parse, with fresh node accounting.
        SaxParser(Cursor& aCur, pJsonParser::SaxHandler& aHandler,
                  const pJsonParser::Options& aOpts, pJsonParser::Error* aErr)
                : cur(aCur)
                , handler(aHandler)
                , opts(aOpts)
                , err(aErr)
                , nodeCount(0) {}

        // Parses exactly one complete document, translating parser, handler,
        // allocation, and stream failures into a stable non-throwing result.
        bool parseDocument() noexcept {
            try {
                resetParseError(err);
                if (!parseValue(0, true))
                    return false;
                if (!skipWhitespace())
                    return false;
                char ch = 0;
                if (opts.maxInputBytes != 0 && cur.position() >= opts.maxInputBytes) {
                    if (cur.peek(ch))
                        return failAt(opts.maxInputBytes, cur.line(), cur.column(),
                                      "input exceeds maxInputBytes");
                } else if (cur.peek(ch)) {
                    return fail("trailing characters after JSON value");
                }
                if (cur.failed())
                    return fail("stream read failed");
                return true;
            } catch (const SaxParseCancelled&) {
                return failNoThrow("SAX parse aborted");
            } catch (const std::bad_alloc&) {
                return failNoThrow("SAX parse ran out of memory");
            } catch (const std::exception&) {
                return failNoThrow("SAX parse or handler exception");
            } catch (...) {
                return failNoThrow("SAX parse or handler exception");
            }
        }

        // Dispatches one value at the current nesting depth. emit=false still
        // validates and counts the subtree but deliberately skips callbacks.
        bool parseValue(size_t depth, bool emit) {
            if (!skipWhitespace())
                return false;

            char ch = 0;
            if (!cur.peek(ch)) {
                if (cur.failed())
                    return fail("stream read failed");
                return fail("unexpected end of input; expected a value");
            }

            if (ch == '"')
                return parseStringValue(emit);
            if (ch == '{')
                return parseObject(depth + 1, emit);
            if (ch == '[')
                return parseArray(depth + 1, emit);
            if (ch == '-' || (ch >= '0' && ch <= '9'))
                return parseNumberValue(emit);
            return parseKeywordValue(emit);
        }

        // Consumes only the four whitespace bytes admitted by JSON.
        bool skipWhitespace() {
            char ch = 0;
            while (cur.peek(ch) && pJsonParserImpl::isWhitespace(ch)) {
                if (!getChar(ch))
                    return false;
            }
            if (cur.failed())
                return fail("stream read failed");
            return true;
        }

        // Parses a string value and emits it after it has consumed one node from
        // the configured budget. Object keys are handled separately.
        bool parseStringValue(bool emit) {
            if (!reserveNode())
                return false;
            std::string value;
            if (!parseStringRaw(value))
                return false;
            if (!emit)
                return true;
            return dispatch(handler.onString(value));
        }

        // Recognizes the lowercase null/boolean literals required by RFC 8259.
        bool parseKeywordValue(bool emit) {
            char ch = 0;
            if (!cur.peek(ch))
                return fail("unexpected end of input; expected a value");

            if (ch == 'n') {
                if (!matchLiteral("null"))
                    return false;
                if (!reserveNode())
                    return false;
                return !emit || dispatch(handler.onNull());
            }
            if (ch == 't') {
                if (!matchLiteral("true"))
                    return false;
                if (!reserveNode())
                    return false;
                return !emit || dispatch(handler.onBool(true));
            }
            if (ch == 'f') {
                if (!matchLiteral("false"))
                    return false;
                if (!reserveNode())
                    return false;
                return !emit || dispatch(handler.onBool(false));
            }
            return fail("invalid JSON value");
        }

        // Scans the JSON number grammar before conversion. Integral tokens that
        // overflow int64 are preserved as finite doubles rather than truncated.
        bool parseNumberValue(bool emit) {
            std::string text;
            bool isFloat = false;
            const char* scanError = nullptr;
            struct Adapter {
                SaxParser& parser;
                bool peek(char& ch) { return parser.cur.peek(ch); }
                bool take(char& ch) { return parser.getChar(ch); }
            } adapter = {*this};
            if (!scanJsonNumber(adapter, text, isFloat, scanError))
                return scanError == nullptr ? false : fail(scanError);

            if (!reserveNode())
                return false;

            pJsonParserImpl::ParsedNumber number;
            const char* message = nullptr;
            if (!pJsonParserImpl::convertNumberToken(text, isFloat, opts.numberPolicy, number,
                                                     message))
                return fail(message);
            if (!emit)
                return true;
            if (number.kind == pJsonParserImpl::ParsedNumber::SignedInteger)
                return dispatch(handler.onInt(number.signedValue));
            if (number.kind == pJsonParserImpl::ParsedNumber::UnsignedInteger)
                return dispatch(handler.onUInt(number.unsignedValue));
            return dispatch(handler.onDouble(number.floatingValue));
        }

        // Parses an array while explicitly tracking comma state so leading,
        // repeated, missing, and trailing commas receive deterministic errors.
        bool parseArray(size_t depth, bool emit) {
            const size_t maxDepth = static_cast<size_t>(clampParseDepth(opts.maxDepth));
            if (depth > maxDepth)
                return fail("maximum nesting depth exceeded");
            if (!reserveNode())
                return false;

            char ch = 0;
            if (!getChar(ch) || ch != '[')
                return fail("unexpected end of input; expected a value");
            if (emit && !dispatch(handler.onStartArray()))
                return false;

            bool expectValue = false;
            bool any = false;
            while (true) {
                if (!skipWhitespace())
                    return false;
                if (!cur.peek(ch)) {
                    if (cur.failed())
                        return fail("stream read failed");
                    return fail("unterminated array");
                }
                if (ch == ']') {
                    if (expectValue)
                        return fail("trailing comma in array");
                    if (!getChar(ch))
                        return false;
                    return !emit || dispatch(handler.onEndArray());
                }
                if (ch == ',') {
                    if (!any || expectValue)
                        return fail("unexpected ',' in array");
                    if (!getChar(ch))
                        return false;
                    expectValue = true;
                    continue;
                }
                if (any && !expectValue)
                    return fail("missing ',' between array elements");
                if (!parseValue(depth, emit))
                    return false;
                any = true;
                expectValue = false;
            }
        }

        // Parses an object and implements duplicate-key policy at event time.
        // KeepFirst parses duplicate values with emit=false so malformed input
        // and resource-limit violations cannot hide inside discarded members.
        bool parseObject(size_t depth, bool emit) {
            const size_t maxDepth = static_cast<size_t>(clampParseDepth(opts.maxDepth));
            if (depth > maxDepth)
                return fail("maximum nesting depth exceeded");
            if (!reserveNode())
                return false;

            char ch = 0;
            if (!getChar(ch) || ch != '{')
                return fail("unexpected end of input; expected a value");
            if (emit && !dispatch(handler.onStartObject()))
                return false;

            bool expectMember = false;
            bool any = false;
            std::map<std::string, bool> seenKeys;
            while (true) {
                if (!skipWhitespace())
                    return false;
                if (!cur.peek(ch)) {
                    if (cur.failed())
                        return fail("stream read failed");
                    return fail("unterminated object");
                }
                if (ch == '}') {
                    if (expectMember)
                        return fail("trailing comma in object");
                    if (!getChar(ch))
                        return false;
                    return !emit || dispatch(handler.onEndObject());
                }
                if (ch == ',') {
                    if (!any || expectMember)
                        return fail("unexpected ',' in object");
                    if (!getChar(ch))
                        return false;
                    expectMember = true;
                    continue;
                }
                if (ch != '"')
                    return fail("expected '\"' to start an object key");
                if (any && !expectMember)
                    return fail("missing ',' between object members");

                const size_t keyOffset = cur.position();
                const size_t keyLine = cur.line();
                const size_t keyColumn = cur.column();
                std::string key;
                if (!parseStringRaw(key))
                    return false;
                if (!skipWhitespace())
                    return false;
                if (!getChar(ch) || ch != ':')
                    return fail("expected ':' after object key");

                bool duplicate = false;
                if (opts.duplicateKeys != pJsonParser::Options::KeepLastDuplicate) {
                    duplicate = seenKeys.find(key) != seenKeys.end();
                }
                if (duplicate && opts.duplicateKeys == pJsonParser::Options::RejectDuplicateKeys) {
                    return failAt(keyOffset, keyLine, keyColumn, "duplicate object key");
                }
                if (!duplicate && opts.duplicateKeys != pJsonParser::Options::KeepLastDuplicate)
                    seenKeys[key] = true;

                const bool emitValue =
                    emit &&
                    !(duplicate && opts.duplicateKeys == pJsonParser::Options::KeepFirstDuplicate);
                if (emitValue && !dispatch(handler.onKey(key)))
                    return false;
                if (!parseValue(depth, emitValue))
                    return false;
                any = true;
                expectMember = false;
            }
        }

        // Decodes a quoted JSON string and rejects invalid Unicode/control bytes.
        bool parseStringRaw(std::string& out) {
            char ch = 0;
            if (!getChar(ch) || ch != '"')
                return fail("expected '\"' to start a string");

            out.clear();
            while (true) {
                if (!getChar(ch)) {
                    if (cur.failed())
                        return fail("stream read failed");
                    return fail("unterminated string");
                }
                const unsigned char uch = static_cast<unsigned char>(ch);
                if (ch == '"')
                    return true;
                if (ch == '\\') {
                    if (!getChar(ch))
                        return fail("dangling escape at end of input");
                    switch (ch) {
                        case '"':
                            out += '"';
                            break;
                        case '\\':
                            out += '\\';
                            break;
                        case '/':
                            out += '/';
                            break;
                        case 'b':
                            out += '\b';
                            break;
                        case 'f':
                            out += '\f';
                            break;
                        case 'n':
                            out += '\n';
                            break;
                        case 'r':
                            out += '\r';
                            break;
                        case 't':
                            out += '\t';
                            break;
                        case 'u': {
                            uint32_t cp = 0;
                            if (!readHex4(cp))
                                return false;
                            if (cp >= 0xD800 && cp <= 0xDBFF) {
                                char slash = 0;
                                if (cur.peek(slash) && slash == '\\') {
                                    if (!getChar(slash))
                                        return fail("invalid \\u escape");
                                    char u = 0;
                                    if (!getChar(u))
                                        return fail("invalid \\u escape");
                                    if (u == 'u') {
                                        std::string hex;
                                        hex.reserve(4);
                                        bool complete = true;
                                        for (int i = 0; i < 4; ++i) {
                                            char hx = 0;
                                            if (!getChar(hx)) {
                                                complete = false;
                                                break;
                                            }
                                            hex.push_back(hx);
                                        }
                                        uint32_t low = 0;
                                        const bool validLow =
                                            complete && hex.size() == 4 &&
                                            pJsonParserImpl::hex4(hex.c_str(), 0, low) &&
                                            low >= 0xDC00 && low <= 0xDFFF;
                                        if (validLow) {
                                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                                        } else {
                                            return fail("unpaired high surrogate");
                                        }
                                    } else {
                                        return fail("unpaired high surrogate");
                                    }
                                } else {
                                    return fail("unpaired high surrogate");
                                }
                            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                                return fail("unpaired low surrogate");
                            }
                            pJsonParserImpl::appendUtf8(cp, out);
                            break;
                        }
                        default:
                            return fail("invalid escape sequence");
                    }
                    continue;
                }
                if (uch < 0x20) {
                    return fail("unescaped control character in string");
                }
                if (uch >= 0x80) {
                    out += static_cast<char>(uch);
                    if (!consumeUtf8Tail(uch, out))
                        return false;
                    continue;
                }
                out += static_cast<char>(uch);
            }
        }

        // Consumes and validates the continuation bytes for an already-stored
        // UTF-8 lead byte, including overlong, surrogate, and range checks.
        bool consumeUtf8Tail(unsigned char lead, std::string& out) {
            int need = 0;
            uint32_t code = 0;
            if ((lead & 0xE0U) == 0xC0U) {
                need = 1;
                code = lead & 0x1FU;
            } else if ((lead & 0xF0U) == 0xE0U) {
                need = 2;
                code = lead & 0x0FU;
            } else if ((lead & 0xF8U) == 0xF0U) {
                need = 3;
                code = lead & 0x07U;
            } else {
                return fail("invalid UTF-8 sequence");
            }
            for (int i = 0; i < need; ++i) {
                char ch = 0;
                if (!getChar(ch))
                    return fail("invalid UTF-8 sequence");
                const unsigned char byte = static_cast<unsigned char>(ch);
                if ((byte & 0xC0U) != 0x80U)
                    return fail("invalid UTF-8 sequence");
                code = (code << 6) | (byte & 0x3FU);
                out += ch;
            }
            if ((need == 1 && code < 0x80U) || (need == 2 && code < 0x800U) ||
                (need == 3 && code < 0x10000U) || code > 0x10FFFFU ||
                (code >= 0xD800U && code <= 0xDFFFU)) {
                return fail("invalid UTF-8 sequence");
            }
            return true;
        }

        // Reads exactly four hexadecimal digits following a \u escape.
        bool readHex4(uint32_t& out) {
            out = 0;
            for (int i = 0; i < 4; ++i) {
                char ch = 0;
                if (!getChar(ch))
                    return fail("invalid \\u escape");
                out <<= 4;
                if (ch >= '0' && ch <= '9')
                    out |= static_cast<uint32_t>(ch - '0');
                else if (ch >= 'a' && ch <= 'f')
                    out |= static_cast<uint32_t>(10 + ch - 'a');
                else if (ch >= 'A' && ch <= 'F')
                    out |= static_cast<uint32_t>(10 + ch - 'A');
                else
                    return fail("invalid \\u escape");
            }
            return true;
        }

        // Consumes one known lowercase JSON literal.
        bool matchLiteral(const char* lit) {
            for (size_t i = 0; lit[i] != '\0'; ++i) {
                char ch = 0;
                if (!getChar(ch))
                    return fail("invalid JSON value");
                const char want = lit[i];
                if (ch != want) {
                    return fail("invalid JSON value");
                }
            }
            return true;
        }

        // Centralizes byte-budget enforcement so no consuming parser path can
        // advance beyond maxInputBytes.
        bool getChar(char& ch) {
            if (opts.maxInputBytes != 0 && cur.position() >= opts.maxInputBytes)
                return failAt(opts.maxInputBytes, cur.line(), cur.column(),
                              "input exceeds maxInputBytes");
            return cur.get(ch);
        }

        // Accounts for one JSON value even when its callbacks are suppressed.
        bool reserveNode() {
            if (opts.maxNodes != 0 && nodeCount >= opts.maxNodes)
                return fail("document too large (node budget exceeded)");
            ++nodeCount;
            return true;
        }

        // Converts a handler's false return into an exception solely to unwind
        // nested parse calls; the public SAX API never exposes the exception.
        bool dispatch(bool ok) {
            if (!ok)
                throw SaxParseCancelled();
            return true;
        }

        // Records a failure at the cursor's current source location.
        bool fail(const std::string& message) {
            if (err) {
                err->ok = false;
                err->code = classifyParseMessage(message);
                err->offset = cur.position();
                err->line = cur.line();
                err->column = cur.column();
                err->message = message;
            }
            return false;
        }

        // Records a failure at a saved location, such as a duplicate key's start.
        bool failAt(size_t offset, size_t line, size_t column, const std::string& message) {
            if (err) {
                err->ok = false;
                err->code = classifyParseMessage(message);
                err->offset = offset;
                err->line = line;
                err->column = column;
                err->message = message;
            }
            return false;
        }

        // Catch-path diagnostics must not replace the original handler/parser
        // failure with an allocation exception while assigning the message.
        bool failNoThrow(const char* message) noexcept {
            if (err) {
                err->ok = false;
                err->code = pJsonParser::Error::CallbackError;
                err->offset = cur.position();
                err->line = cur.line();
                err->column = cur.column();
                try {
                    err->message = message;
                } catch (...) {
                    // basic_string::clear is non-allocating; retain the
                    // structured coordinates even when message assignment fails.
                    err->message.clear();
                }
            }
            return false;
        }
    };
} // namespace

// Records the first parse error (byte offset + message) and returns false so
// callers can `return fail(...)`.
/*static*/
bool pJsonParserImpl::fail(pJsonParserImpl::ParseCtx& c, size_t aPos, const char* aMsg) {
    if (!c.failed) {
        c.failed = true;
        c.errPos = aPos;
        c.errMsg = aMsg;
    }
    return false;
}
// Allocates a new pjson while enforcing the node budget. Returns nullptr (and
// records a "document too large" failure) once maxNodes values have been
// created, which caps total memory even for inputs that stay within maxDepth
// (e.g. a huge flat array). The caller propagates the nullptr as a parse error.
/*static*/
pjson* pJsonParserImpl::newNode(pJsonParserImpl::ParseCtx& c) {
    if (c.maxNodes != 0 && c.nodeCount >= c.maxNodes) {
        fail(c, c.pos, "document too large (node budget exceeded)");
        return nullptr;
    }
    ++c.nodeCount;
    return pjsonImpl::_allocateNode(*c.allocator);
}

// Decodes a JSON string body from c.pos into aOut. With bStopAtQuote, decoding
// stops at (and consumes) the first unescaped '"'. RFC 8259-invalid escapes,
// control bytes, surrogate halves, and UTF-8 are rejected.
/*static*/
bool pJsonParserImpl::decodeStringBody(pJsonParserImpl::ParseCtx& c, std::string& aOut,
                                       bool bStopAtQuote) {
    aOut.clear();
    while (c.pos < c.end) {
        unsigned char ch = static_cast<unsigned char>(c.src[c.pos]);
        if (bStopAtQuote && ch == '\"') {
            ++c.pos;
            return true;
        }
        if (ch == '\\') {
            ++c.pos;
            if (c.pos >= c.end) {
                return fail(c, c.pos, "dangling escape at end of input");
            }
            char e = c.src[c.pos++];
            switch (e) {
                case '\"':
                    aOut += '\"';
                    break;
                case '\\':
                    aOut += '\\';
                    break;
                case '/':
                    aOut += '/';
                    break;
                case 'b':
                    aOut += '\b';
                    break;
                case 'f':
                    aOut += '\f';
                    break;
                case 'n':
                    aOut += '\n';
                    break;
                case 'r':
                    aOut += '\r';
                    break;
                case 't':
                    aOut += '\t';
                    break;
                case 'u': {
                    uint32_t cp = 0;
                    if (c.pos + 4 > c.end || !pJsonParserImpl::hex4(c.src, c.pos, cp)) {
                        return fail(c, c.pos, "invalid \\u escape");
                    }
                    c.pos += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        // High surrogate: look for a following low surrogate.
                        uint32_t low = 0;
                        if (c.pos + 6 <= c.end && c.src[c.pos] == '\\' && c.src[c.pos + 1] == 'u' &&
                            pJsonParserImpl::hex4(c.src, c.pos + 2, low) && low >= 0xDC00 &&
                            low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            c.pos += 6;
                        } else {
                            return fail(c, c.pos, "unpaired high surrogate");
                        }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return fail(c, c.pos, "unpaired low surrogate");
                    }
                    appendUtf8(cp, aOut);
                    break;
                }
                default:
                    return fail(c, c.pos - 1, "invalid escape sequence");
            }
        } else if (ch < 0x20) {
            return fail(c, c.pos, "unescaped control character in string");
        } else if (ch >= 0x80) {
            int n = pjsonImpl::_utf8Len(c.src, c.pos, c.end);
            if (n == 0) {
                return fail(c, c.pos, "invalid UTF-8 sequence");
            }
            aOut.append(c.src + c.pos, static_cast<size_t>(n));
            c.pos += static_cast<size_t>(n);
        } else {
            aOut += static_cast<char>(ch);
            ++c.pos;
        }
    }
    if (bStopAtQuote) {
        return fail(c, c.pos, "unterminated string");
    }
    return true;
}

// Reads incrementally so maxInputBytes bounds memory before the complete stream
// has been materialized. Returns the parsed document by value (null on failure).
/*static*/
pjson pJsonParserImpl::parseStream(std::istream& aIn, const pJsonParser::Options& aOpts,
                                   pJsonParser::Error* aErr, pjson::Allocator& aAlloc) {
    std::string content;
    char buffer[8192];
    while (aIn.good()) {
        aIn.read(buffer, sizeof(buffer));
        const std::streamsize got = aIn.gcount();
        if (got <= 0)
            continue;
        const size_t chunk = static_cast<size_t>(got);
        if (aOpts.maxInputBytes != 0 && (content.size() > aOpts.maxInputBytes ||
                                         chunk > aOpts.maxInputBytes - content.size())) {
            // Include as much of this chunk as fits, allowing line/column to be
            // calculated at the exact configured byte boundary.
            if (content.size() < aOpts.maxInputBytes) {
                content.append(buffer, aOpts.maxInputBytes - content.size());
            }
            setParseError(aErr, content.data(), content.size(), aOpts.maxInputBytes,
                          "input exceeds maxInputBytes", pJsonParser::Error::InputLimit);
            return pjson(aAlloc);
        }
        content.append(buffer, chunk);
    }
    if (aIn.bad()) {
        setParseError(aErr, content.data(), content.size(), content.size(), "stream read failed",
                      pJsonParser::Error::StreamError);
        return pjson(aAlloc);
    }
    return parseTop(content.c_str(), content.length(), aOpts, aErr, aAlloc);
}
/*static*/
bool pJsonParserImpl::parseSaxTop(const char* aSrc, size_t aSize, pJsonParser::SaxHandler& aHandler,
                                  const pJsonParser::Options& aOpts, pJsonParser::Error* aErr) {
    resetParseError(aErr);
    if (aSrc == nullptr) {
        setParseError(aErr, "", 0, 0, "null input", pJsonParser::Error::InvalidArgument);
        return false;
    }
    if (aOpts.maxInputBytes != 0 && aSize > aOpts.maxInputBytes) {
        setParseError(aErr, aSrc, aSize, aOpts.maxInputBytes, "input exceeds maxInputBytes");
        return false;
    }
    BufferSaxCursor cursor(aSrc, aSize);
    SaxParser<BufferSaxCursor> parser(cursor, aHandler, aOpts, aErr);
    return parser.parseDocument();
}
/*static*/
bool pJsonParserImpl::parseSaxStream(std::istream& aIn, pJsonParser::SaxHandler& aHandler,
                                     const pJsonParser::Options& aOpts, pJsonParser::Error* aErr) {
    resetParseError(aErr);
    StreamSaxCursor cursor(aIn);
    SaxParser<StreamSaxCursor> parser(cursor, aHandler, aOpts, aErr);
    return parser.parseDocument();
}

//===----------------------------------------------------------------------===//
// DOM recursive-descent parser
//
// The cursor advances only across validated syntax, every materialized value
// consumes the shared node budget, and local pjsonImpl::OwnedNode guards retain ownership
// until a child is attached. The first grammar error remains authoritative.
//===----------------------------------------------------------------------===//

// Shared driver: parse a single top-level value, require only trailing
// whitespace, and report success/failure through the optional pJsonParser::Error.
/*static*/
pjson pJsonParserImpl::parseTop(const char* aSrc, size_t aSize, const pJsonParser::Options& aOpts,
                                pJsonParser::Error* aErr, pjson::Allocator& aAlloc) {
    resetParseError(aErr);
    if (aSrc == nullptr) {
        setParseError(aErr, "", 0, 0, "null input", pJsonParser::Error::InvalidArgument);
        return pjson(aAlloc);
    }

    // Reject an over-large input up front (cheap DoS guard before any work).
    if (aOpts.maxInputBytes != 0 && aSize > aOpts.maxInputBytes) {
        setParseError(aErr, aSrc, aSize, aOpts.maxInputBytes, "input exceeds maxInputBytes",
                      pJsonParser::Error::InputLimit);
        return pjson(aAlloc);
    }

    pJsonParserImpl::ParseCtx c;
    c.src = aSrc;
    c.pos = 0;
    c.end = aSize;
    c.duplicateKeys = aOpts.duplicateKeys;
    c.numberPolicy = aOpts.numberPolicy;
    c.depth = 0;
    c.maxDepth = clampParseDepth(aOpts.maxDepth);
    c.nodeCount = 0;
    c.maxNodes = aOpts.maxNodes;
    c.allocator = &aAlloc;
    c.failed = false;
    c.errPos = 0;

    try {
        pjson* parsed = nullptr;
        if (!parseValue(c, parsed)) {
            pjsonImpl::_destroyNode(parsed);
            setParseError(aErr, aSrc, aSize, c.errPos, c.errMsg.empty() ? "parse error" : c.errMsg);
            return pjson(aAlloc);
        }
        // Own the parsed node so it is freed even if the trailing check throws.
        pjsonImpl::OwnedNode owned(parsed);

        // A valid document is a single value; only trailing whitespace may follow.
        char trailing;
        if (peek(c, trailing)) {
            setParseError(aErr, aSrc, aSize, c.pos, "trailing characters after JSON value",
                          pJsonParser::Error::Syntax);
            return pjson(aAlloc);
        }
        // Move the parsed node's storage into a value bound to the same allocator.
        // O(1): the value adopts the node's inline storage; the node wrapper is
        // then freed empty by OwnedNode, so no smart pointer escapes to the caller.
        pjson result(aAlloc);
        pjsonImpl::_swapStorage(result, *parsed);
        return result;
    } catch (const std::bad_alloc&) {
        setParseError(aErr, aSrc, aSize, c.pos, "parse ran out of memory",
                      pJsonParser::Error::AllocationFailure);
    } catch (const std::exception& ex) {
        setParseError(aErr, aSrc, aSize, c.pos,
                      std::string("parse failed with exception: ") + ex.what());
    } catch (...) {
        setParseError(aErr, aSrc, aSize, c.pos, "parse failed with exception");
    }
    return pjson(aAlloc);
}
// Skips whitespace and reports the next character without consuming it.
/*static*/
bool pJsonParserImpl::peek(pJsonParserImpl::ParseCtx& c, char& aOut) {
    while (c.pos < c.end) {
        aOut = c.src[c.pos];
        if (isWhitespace(aOut)) {
            ++c.pos;
        } else {
            return true;
        }
    }
    return false;
}
// Consumes the ':' separating an object key from its value (skipping ws).
/*static*/
bool pJsonParserImpl::skipColon(pJsonParserImpl::ParseCtx& c) {
    while (c.pos < c.end) {
        char ch = c.src[c.pos++];
        if (ch == ':') {
            return true;
        } else if (isWhitespace(ch)) {
            // ignore
        } else {
            return fail(c, c.pos - 1, "expected ':' after object key");
        }
    }
    return fail(c, c.pos, "expected ':' after object key");
}
// Dispatches on the next non-whitespace character to the right sub-parser.
/*static*/
bool pJsonParserImpl::parseValue(pJsonParserImpl::ParseCtx& c, pjson*& aOut) {
    char ch;
    if (!peek(c, ch)) {
        return fail(c, c.pos, "unexpected end of input; expected a value");
    }
    if (ch == '\"') {
        return parseString(c, aOut);
    } else if (ch == '{') {
        return parseObject(c, aOut);
    } else if (ch == '[') {
        return parseArray(c, aOut);
    } else if (ch == '-' || (ch >= '0' && ch <= '9')) {
        return parseNumber(c, aOut);
    } else {
        // RFC 8259 null / true / false literals.
        return parseKeyword(c, aOut);
    }
}
// Matches a keyword literal using the exact lowercase RFC spelling.
/*static*/
bool pJsonParserImpl::parseKeyword(pJsonParserImpl::ParseCtx& c, pjson*& aOut) {
    struct KW {
        const char* word;
        size_t len;
        int kind;
    }; // kind: 0 null,1 true,2 false
    static const KW kws[] = {
        {"null", 4, 0},
        {"true", 4, 1},
        {"false", 5, 2},
    };
    for (const KW& kw : kws) {
        if (c.pos + kw.len > c.end)
            continue;
        bool match = true;
        for (size_t k = 0; k < kw.len; ++k) {
            char a = c.src[c.pos + k];
            char b = kw.word[k];
            if (a != b) {
                match = false;
                break;
            }
        }
        if (match) {
            c.pos += kw.len;
            pjsonImpl::OwnedNode value(newNode(c));
            if (!value)
                return false;
            if (kw.kind == 1)
                *value = true;
            else if (kw.kind == 2)
                *value = false;
            // kind 0 leaves it as null
            aOut = value.release();
            return true;
        }
    }
    return fail(c, c.pos, "invalid JSON value");
}
// Reads a quoted string body starting at the opening '"'.
/*static*/
bool pJsonParserImpl::extractString(pJsonParserImpl::ParseCtx& c, std::string& aOut) {
    if (c.pos >= c.end || c.src[c.pos] != '\"') {
        return fail(c, c.pos, "expected '\"' to start a string");
    }
    ++c.pos; // consume opening quote
    return pJsonParserImpl::decodeStringBody(c, aOut, /*aStopAtQuote=*/true);
}
/*static*/
// Parses and allocates one string value after decoding its complete token.
bool pJsonParserImpl::parseString(pJsonParserImpl::ParseCtx& c, pjson*& aOut) {
    std::string s;
    if (!extractString(c, s)) {
        return false;
    }
    pjsonImpl::OwnedNode value(newNode(c));
    if (!value)
        return false;
    *value = s;
    aOut = value.release();
    return true;
}
// Parses a JSON number following the grammar
//   -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
// Integer tokens in [INT64_MIN, INT64_MAX] are stored as jsonNumberInt; tokens
// in (INT64_MAX, UINT64_MAX] are stored as jsonNumberUInt; anything with a
// fraction/exponent is stored as a double. Integer tokens outside the exact
// 64-bit range and floating tokens outside binary64 are rejected unless the
// AllowLossyNumbers policy opts in to storing the nearest finite double. Never
// throws.
/*static*/
bool pJsonParserImpl::parseNumber(pJsonParserImpl::ParseCtx& c, pjson*& aOut) {
    const size_t begin = c.pos;
    size_t scanPosition = c.pos;
    struct Adapter {
        pJsonParserImpl::ParseCtx& context;
        size_t& position;
        bool peek(char& ch) {
            if (position >= context.end)
                return false;
            ch = context.src[position];
            return true;
        }
        bool take(char& ch) {
            if (!peek(ch))
                return false;
            ++position;
            return true;
        }
    } adapter = {c, scanPosition};
    std::string text;
    bool bFloat = false;
    const char* scanError = nullptr;
    if (!scanJsonNumber(adapter, text, bFloat, scanError))
        return fail(c, scanPosition, scanError == nullptr ? "invalid number" : scanError);
    pJsonParserImpl::ParsedNumber number;
    const char* message = nullptr;
    if (!convertNumberToken(text, bFloat, c.numberPolicy, number, message))
        return fail(c, begin, message);
    pjsonImpl::OwnedNode value(newNode(c));
    if (!value)
        return false;
    if (number.kind == pJsonParserImpl::ParsedNumber::SignedInteger)
        *value = number.signedValue;
    else if (number.kind == pJsonParserImpl::ParsedNumber::UnsignedInteger)
        *value = number.unsignedValue;
    else
        *value = number.floatingValue;
    aOut = value.release();
    c.pos = scanPosition;
    return true;
}
// Parses one array under a balanced depth charge. A child remains RAII-owned
// until vector growth succeeds, preventing leaks on allocation failure.
/*static*/
bool pJsonParserImpl::parseArray(pJsonParserImpl::ParseCtx& c, pjson*& aOut) {
    if (++c.depth > c.maxDepth) {
        --c.depth;
        return fail(c, c.pos, "maximum nesting depth exceeded");
    }
    pjsonImpl::OwnedNode arr(newNode(c));
    if (!arr) {
        --c.depth;
        return false;
    }
    arr->resetTo(pjson::jsonType::jsonArray);
    ++c.pos; // consume '['

    bool bExpectValue = false; // a comma was seen, a value must follow
    bool bAny = false;         // at least one value parsed
    char ch;
    while (peek(c, ch)) {
        if (ch == ']') {
            if (bExpectValue) {
                --c.depth;
                return fail(c, c.pos, "trailing comma in array");
            }
            ++c.pos;
            --c.depth;
            aOut = arr.release();
            return true;
        } else if (ch == ',') {
            if (!bAny || bExpectValue) {
                --c.depth;
                return fail(c, c.pos, "unexpected ',' in array");
            }
            ++c.pos;
            bExpectValue = true;
        } else {
            if (bAny && !bExpectValue) {
                --c.depth;
                return fail(c, c.pos, "missing ',' between array elements");
            }
            pjson* elem = nullptr;
            if (!parseValue(c, elem)) {
                pjsonImpl::_destroyNode(elem);
                --c.depth;
                return false;
            }
            pjsonImpl::OwnedNode ownedElem(elem);
            pjsonImpl::_array(*arr).push_back(nullptr);
            pjsonImpl::_array(*arr).back() = ownedElem.release();
            bAny = true;
            bExpectValue = false;
        }
    }
    --c.depth;
    return fail(c, c.pos, "unterminated array");
}
// Parses one object under a balanced depth charge and applies duplicate policy
// only after the replacement value is fully parsed and owned.
/*static*/
bool pJsonParserImpl::parseObject(pJsonParserImpl::ParseCtx& c, pjson*& aOut) {
    if (++c.depth > c.maxDepth) {
        --c.depth;
        return fail(c, c.pos, "maximum nesting depth exceeded");
    }
    pjsonImpl::OwnedNode obj(newNode(c));
    if (!obj) {
        --c.depth;
        return false;
    }
    obj->resetTo(pjson::jsonType::jsonObject);
    ++c.pos; // consume '{'

    bool bExpectMember = false; // a comma was seen, a member must follow
    bool bAny = false;          // at least one member parsed
    char ch;
    while (peek(c, ch)) {
        if (ch == '}') {
            if (bExpectMember) {
                --c.depth;
                return fail(c, c.pos, "trailing comma in object");
            }
            ++c.pos;
            --c.depth;
            aOut = obj.release();
            return true;
        } else if (ch == ',') {
            if (!bAny || bExpectMember) {
                --c.depth;
                return fail(c, c.pos, "unexpected ',' in object");
            }
            ++c.pos;
            bExpectMember = true;
        } else if (ch == '\"') {
            if (bAny && !bExpectMember) {
                --c.depth;
                return fail(c, c.pos, "missing ',' between object members");
            }
            const size_t keyOffset = c.pos;
            std::string mkey;
            if (!extractString(c, mkey)) {
                --c.depth;
                return false;
            }
            // PJSON-PARSE-002: under the reject policy, report the duplicate
            // immediately after the second name is decoded, before parsing (and
            // allocating) its value subtree.
            const bool duplicate =
                pjsonImpl::_object(*obj).find(mkey) != pjsonImpl::_object(*obj).end();
            if (duplicate && c.duplicateKeys == pJsonParser::Options::RejectDuplicateKeys) {
                --c.depth;
                return fail(c, keyOffset, "duplicate object key");
            }
            pjson* val = nullptr;
            if (!skipColon(c) || !parseValue(c, val)) {
                pjsonImpl::_destroyNode(val);
                --c.depth;
                return false;
            }
            // Apply the remaining duplicate-key policy: keep the first or last
            // value deterministically (reject was already handled above).
            if (duplicate) {
                pjsonImpl::ObjectStorage::iterator it = pjsonImpl::_object(*obj).find(mkey);
                if (c.duplicateKeys == pJsonParser::Options::KeepLastDuplicate) {
                    pjsonImpl::_destroyNode(it->second);
                    it->second = val;
                } else {
                    pjsonImpl::_destroyNode(val); // KeepFirstDuplicate
                }
            } else {
                pjsonImpl::OwnedNode ownedVal(val);
                pjson*& slot = pjsonImpl::_object(*obj)[mkey];
                slot = ownedVal.release();
            }
            bAny = true;
            bExpectMember = false;
        } else {
            --c.depth;
            return fail(c, c.pos, "expected '\"' to start an object key");
        }
    }
    --c.depth;
    return fail(c, c.pos, "unterminated object");
}

//===----------------------------------------------------------------------===//
