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
// Author: Praveen Babu J D
// License: Apache 2.0
//
#include "pjson_internal.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iomanip>
#include <istream>
#include <limits>
#include <locale>
#include <new>
#include <ostream>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>

using namespace ByteDance;

namespace {
    // Returns the effective, stack-safe nesting limit for a configured maxDepth.
    inline int clampParseDepth(int aConfigured) {
        if (aConfigured <= 0)
            return 1;
        return aConfigured < kParseDepthHardLimit ? aConfigured : kParseDepthHardLimit;
    }
} // namespace

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

    // Maps a parser diagnostic message to a stable ParseError::Code. The exact
    // message wording may evolve; this keeps the machine-facing category stable
    // by classifying on the well-known phrases the parser emits.
    ParseError::Code classifyParseMessage(const std::string& message) {
        if (message.find("UTF-8") != std::string::npos ||
            message.find("surrogate") != std::string::npos ||
            message.find("escape") != std::string::npos || message.find("\\u") != std::string::npos)
            return ParseError::InvalidEncoding;
        if (message.find("duplicate object key") != std::string::npos)
            return ParseError::DuplicateKey;
        if (message.find("out of range") != std::string::npos ||
            message.find("number") != std::string::npos)
            return ParseError::NumberRange;
        if (message.find("nesting depth") != std::string::npos)
            return ParseError::DepthLimit;
        if (message.find("maxInputBytes") != std::string::npos)
            return ParseError::InputLimit;
        if (message.find("maxNodes") != std::string::npos ||
            message.find("node budget") != std::string::npos)
            return ParseError::NodeLimit;
        if (message.find("out of memory") != std::string::npos)
            return ParseError::AllocationFailure;
        if (message.find("stream read") != std::string::npos)
            return ParseError::StreamError;
        return ParseError::Syntax;
    }

    // Publishes a buffer-parser failure, deriving source coordinates from the
    // authoritative byte offset. A null destination intentionally discards it.
    // The code is classified from the message unless an explicit one is given.
    void setParseError(ParseError* err, const char* src, size_t size, size_t offset,
                       const std::string& message, ParseError::Code code = ParseError::None) {
        if (!err)
            return;
        err->ok = false;
        err->code = code == ParseError::None ? classifyParseMessage(message) : code;
        err->offset = offset;
        lineAndColumn(src, size, offset, err->line, err->column);
        err->message = message;
    }

    // Restores the public error object to its successful, start-of-input state.
    void resetParseError(ParseError* err) {
        if (!err)
            return;
        err->ok = true;
        err->code = ParseError::None;
        err->offset = 0;
        err->line = 1;
        err->column = 1;
        err->message.clear();
    }

    // Internal control-flow exception used to unwind immediately when a SAX
    // callback returns false; parseDocument converts it back into ParseError.
    class SaxParseCancelled : public std::exception {
    public:
        // Supplies a stable diagnostic if cancellation escapes an internal frame.
        const char* what() const noexcept override { return "SAX parse aborted"; }
    };

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
        SaxHandler& handler;
        const ParseOptions& opts;
        ParseError* err;
        size_t nodeCount;

        // Couples a cursor and event sink for one parse, with fresh node accounting.
        SaxParser(Cursor& aCur, SaxHandler& aHandler, const ParseOptions& aOpts, ParseError* aErr)
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
            while (cur.peek(ch) && pjsonImpl::_isWhitespace(ch)) {
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
            char ch = 0;
            if (!cur.peek(ch))
                return fail("unexpected end of input; expected a value");

            if (ch == '-') {
                if (!getChar(ch))
                    return false;
                text.push_back(ch);
                if (!cur.peek(ch))
                    return fail("invalid number: expected digit");
            }

            if (ch == '0') {
                if (!getChar(ch))
                    return false;
                text.push_back(ch);
            } else if (ch >= '1' && ch <= '9') {
                do {
                    if (!getChar(ch))
                        return false;
                    text.push_back(ch);
                } while (cur.peek(ch) && ch >= '0' && ch <= '9');
            } else {
                return fail("invalid number: expected digit");
            }

            bool isFloat = false;
            if (cur.peek(ch) && ch == '.') {
                isFloat = true;
                if (!getChar(ch))
                    return false;
                text.push_back(ch);
                if (!cur.peek(ch) || ch < '0' || ch > '9')
                    return fail("invalid number: '.' must be followed by a digit");
                do {
                    if (!getChar(ch))
                        return false;
                    text.push_back(ch);
                } while (cur.peek(ch) && ch >= '0' && ch <= '9');
            }

            if (cur.peek(ch) && (ch == 'e' || ch == 'E')) {
                isFloat = true;
                if (!getChar(ch))
                    return false;
                text.push_back(ch);
                if (cur.peek(ch) && (ch == '+' || ch == '-')) {
                    if (!getChar(ch))
                        return false;
                    text.push_back(ch);
                }
                if (!cur.peek(ch) || ch < '0' || ch > '9')
                    return fail("invalid number: exponent must have a digit");
                do {
                    if (!getChar(ch))
                        return false;
                    text.push_back(ch);
                } while (cur.peek(ch) && ch >= '0' && ch <= '9');
            }

            if (!reserveNode())
                return false;

            if (isFloat) {
                double d = 0.0;
                if (!pjsonImpl::_parseDouble(text, d) || !std::isfinite(d))
                    return fail("number out of range");
                return !emit || dispatch(handler.onDouble(d));
            }

            const bool negative = !text.empty() && text[0] == '-';
            const bool allowLossy = opts.numberPolicy == ParseOptions::AllowLossyNumbers;

            errno = 0;
            const long long llVal = strtoll(text.c_str(), nullptr, 10);
            if (errno != ERANGE)
                return !emit || dispatch(handler.onInt(static_cast<int64_t>(llVal)));

            if (!negative) {
                errno = 0;
                const unsigned long long ullVal = strtoull(text.c_str(), nullptr, 10);
                if (errno != ERANGE)
                    return !emit || dispatch(handler.onUInt(static_cast<uint64_t>(ullVal)));
            }

            if (!allowLossy)
                return fail("integer out of range; enable AllowLossyNumbers to store as double");
            double d = 0.0;
            if (!pjsonImpl::_parseDouble(text, d) || !std::isfinite(d))
                return fail("number out of range");
            return !emit || dispatch(handler.onDouble(d));
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
                if (opts.duplicateKeys != ParseOptions::KeepLastDuplicate) {
                    duplicate = seenKeys.find(key) != seenKeys.end();
                }
                if (duplicate && opts.duplicateKeys == ParseOptions::RejectDuplicateKeys) {
                    return failAt(keyOffset, keyLine, keyColumn, "duplicate object key");
                }
                if (!duplicate && opts.duplicateKeys != ParseOptions::KeepLastDuplicate)
                    seenKeys[key] = true;

                const bool emitValue =
                    emit && !(duplicate && opts.duplicateKeys == ParseOptions::KeepFirstDuplicate);
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
                                            pjsonImpl::_hex4(hex.c_str(), 0, low) &&
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
                            pjsonImpl::_appendUtf8(cp, out);
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
                err->code = ParseError::CallbackError;
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

//===----------------------------------------------------------------------===//
// Public configuration, diagnostics, and SAX defaults
//
// Constructors establish success-state diagnostics and conservative resource
// limits. The SAX base class accepts every event so clients can override only
// the callbacks they need; returning false from any override cancels parsing.
//===----------------------------------------------------------------------===//
/*static*/
// Returns the compile-time library version string without transferring ownership.
const char* pjson::getVersion() {
    return PJSON_VERSION;
}
// Establishes RFC 8259 parsing with bounded depth, node count, and input size.
pjson::ParseOptions::ParseOptions()
        : maxDepth(512)
        , maxNodes(1000000)
        , maxInputBytes(size_t(64) * 1024U * 1024U)
        , duplicateKeys(RejectDuplicateKeys)
        , numberPolicy(RejectUnrepresentableNumbers) {}
// Establishes compact, UTF-8-preserving, ascending-key serialization.
pjson::SerializeOptions::SerializeOptions()
        : pretty(false)
        , indentWidth(2)
        , indentCharacter(' ')
        , escapeNonAscii(false)
        , keyOrder(AscendingKeys)
        , nonFinite(RejectNonFinite)
        , maxOutputBytes(size_t(64) * 1024U * 1024U) {}
/*static*/
// Produces the default two-space pretty-printing preset.
pjson::SerializeOptions pjson::SerializeOptions::prettyPrinted() {
    SerializeOptions o;
    o.pretty = true;
    return o;
}
// Constructs a success-state parse diagnostic at the start of input.
pjson::ParseError::ParseError()
        : ok(true)
        , code(None)
        , offset(0)
        , line(1)
        , column(1) {}
// Constructs a success-state pointer diagnostic with no failing token.
pjson::PointerError::PointerError()
        : ok(true)
        , code(Ok)
        , pointer()
        , tokenIndex(0)
        , token()
        , message() {}
// Constructs a success-state patch diagnostic with no active operation.
pjson::PatchError::PatchError()
        : ok(true)
        , code(Ok)
        , opIndex(0)
        , op()
        , path()
        , from()
        , tokenIndex(0)
        , token()
        , message() {}
// Establishes finite amplification limits for both JSON Patch variants.
pjson::PatchOptions::PatchOptions()
        : maxOperations(10000)
        , maxClonedNodes(1000000)
        , maxClonedBytes(size_t(64) * 1024U * 1024U)
        , maxWork(1000000) {}
// Gives polymorphic SAX handlers a safe virtual destruction point.
pjson::SaxHandler::~SaxHandler() {}
// Accepts a null event by default.
bool pjson::SaxHandler::onNull() {
    return true;
}
// Accepts a boolean event by default.
bool pjson::SaxHandler::onBool(bool) {
    return true;
}
// Accepts an integer event by default.
bool pjson::SaxHandler::onInt(int64_t) {
    return true;
}
// Accepts an unsigned-integer event by default. The parser only emits this for
// tokens above INT64_MAX, so handlers that care solely about smaller integers
// can ignore it safely.
bool pjson::SaxHandler::onUInt(uint64_t) {
    return true;
}
// Accepts a floating-point event by default.
bool pjson::SaxHandler::onDouble(double) {
    return true;
}
// Accepts a decoded string event by default.
bool pjson::SaxHandler::onString(const std::string&) {
    return true;
}
// Accepts an array-opening event by default.
bool pjson::SaxHandler::onStartArray() {
    return true;
}
// Accepts an array-closing event by default.
bool pjson::SaxHandler::onEndArray() {
    return true;
}
// Accepts an object-opening event by default.
bool pjson::SaxHandler::onStartObject() {
    return true;
}
// Accepts a decoded object-key event by default.
bool pjson::SaxHandler::onKey(const std::string&) {
    return true;
}
// Accepts an object-closing event by default.
bool pjson::SaxHandler::onEndObject() {
    return true;
}
//===----------------------------------------------------------------------===//
// Allocator bridge and node ownership
//
// Containers and strings are constructed in allocator-provided storage. Nodes
// additionally remember whether their outer object came from that allocator so
// _destroyNode can also destroy ordinary `new pjson` roots safely.
//===----------------------------------------------------------------------===//
namespace {
    // Adapts the process-wide operator new/delete pair to the allocator API.
    class DefaultPjsonAllocator : public pjson::Allocator {
    public:
        // Allocates raw storage; size is the only parameter needed by operator new.
        void* allocate(size_t aSize, size_t, AllocationKind) override {
            return ::operator new(aSize);
        }

        // Releases storage previously obtained from allocate.
        void deallocate(void* aPtr, size_t, size_t, AllocationKind) noexcept override {
            ::operator delete(aPtr);
        }
    };

    template <typename T>
    // Constructs an internal DOM object and returns raw storage on constructor failure.
    T* allocateDomObject(pjson::Allocator& aAlloc, pjson::Allocator::AllocationKind aKind) {
        void* storage = aAlloc.allocate(sizeof(T), alignof(T), aKind);
        try {
            return new (storage) T();
        } catch (...) {
            aAlloc.deallocate(storage, sizeof(T), alignof(T), aKind);
            throw;
        }
    }

    template <typename T>
    // Runs an internal object's destructor before returning its exact allocation.
    void destroyDomObject(pjson::Allocator& aAlloc, T* aObject,
                          pjson::Allocator::AllocationKind aKind) noexcept {
        if (aObject == nullptr)
            return;
        aObject->~T();
        aAlloc.deallocate(aObject, sizeof(T), alignof(T), aKind);
    }
} // namespace
// Gives allocator implementations a safe virtual destruction point.
pjson::Allocator::~Allocator() {}
/*static*/
// Returns the stateless process-lifetime allocator used by ordinary values.
pjson::Allocator& pjsonImpl::_defaultAllocator() noexcept {
    static DefaultPjsonAllocator allocator;
    return allocator;
}
/*static*/
// Constructs a node in allocator storage and marks its outer allocation so the
// deleter never mismatches allocator storage with operator delete.
pjson* pjsonImpl::_allocateNode(pjson::Allocator& aAlloc) {
    void* storage =
        aAlloc.allocate(sizeof(pjson), alignof(pjson), pjson::Allocator::NodeAllocation);
    try {
        pjson* value = new (storage) pjson(aAlloc);
        value->_allocatorOwnedNode = true;
        return value;
    } catch (...) {
        aAlloc.deallocate(storage, sizeof(pjson), alignof(pjson), pjson::Allocator::NodeAllocation);
        throw;
    }
}
/*static*/
// Destroys a node through the mechanism that created its outer object. Child
// storage is released first by ~pjson using the node's retained allocator.
void pjsonImpl::_destroyNode(pjson* aValue) noexcept {
    if (aValue == nullptr)
        return;
    if (!aValue->_allocatorOwnedNode) {
        delete aValue;
        return;
    }
    pjson::Allocator& allocator = *aValue->_allocator;
    aValue->~pjson();
    allocator.deallocate(aValue, sizeof(pjson), alignof(pjson), pjson::Allocator::NodeAllocation);
}

//===----------------------------------------------------------------------===//
// DOM value lifetime, storage transfer, and type access
//
// A pjson's allocator identity is immutable. Contents may be transferred in
// constant time only between values using the same allocator; cross-allocator
// moves become deep copies so every descendant remains owned consistently.
//===----------------------------------------------------------------------===//

// Initializes the inactive union representation before a type is selected.
pjson::Storage::Storage()
        : _pValueRaw(nullptr) {}

// Constructs a non-allocator-owned null root using the default allocator.
pjson::pjson()
        : _allocator(&pjsonImpl::_defaultAllocator())
        , _allocatorOwnedNode(false)
        , _disposeNext(nullptr)
        , _eType(jsonType::jsonNull)
        , _uValue() {}
// Constructs a non-allocator-owned null root backed by a caller allocator.
pjson::pjson(Allocator& aAlloc) noexcept
        : _allocator(&aAlloc)
        , _allocatorOwnedNode(false)
        , _disposeNext(nullptr)
        , _eType(jsonType::jsonNull)
        , _uValue() {}
// Releases the active value and all descendants through their retained allocator.
pjson::~pjson() {
    reset();
}
// Deep-copies a value while preserving its allocator identity.
pjson::pjson(const pjson& aFrom)
        : _allocator(aFrom._allocator)
        , _allocatorOwnedNode(false)
        , _disposeNext(nullptr)
        , _eType(jsonType::jsonNull)
        , _uValue() {
    pjsonImpl::_copyContentsInto(*this, aFrom);
}
// Deep-copies a value into a specifically selected allocator domain.
pjson::pjson(const pjson& aFrom, Allocator& aAlloc)
        : _allocator(&aAlloc)
        , _allocatorOwnedNode(false)
        , _disposeNext(nullptr)
        , _eType(jsonType::jsonNull)
        , _uValue() {
    pjsonImpl::_copyContentsInto(*this, aFrom);
}
// Steals storage from a same-allocator source and leaves it as null.
pjson::pjson(pjson&& aFrom) noexcept
        : _allocator(aFrom._allocator)
        , _allocatorOwnedNode(false)
        , _disposeNext(nullptr)
        , _eType(jsonType::jsonNull)
        , _uValue() {
    static_assert(std::is_trivially_copyable<Storage>::value,
                  "pjson storage must remain safe for bytewise transfer");
    _eType = aFrom._eType;
    std::memcpy(&_uValue, &aFrom._uValue, sizeof(_uValue));
    aFrom._uValue._pValueRaw = nullptr;
    aFrom._eType = jsonType::jsonNull;
}
// Steals when allocator domains match; otherwise deep-copies into aAlloc and
// resets the source only after the copy succeeds.
pjson::pjson(pjson&& aFrom, Allocator& aAlloc)
        : _allocator(&aAlloc)
        , _allocatorOwnedNode(false)
        , _disposeNext(nullptr)
        , _eType(jsonType::jsonNull)
        , _uValue() {
    if (_allocator == aFrom._allocator) {
        _eType = aFrom._eType;
        std::memcpy(&_uValue, &aFrom._uValue, sizeof(_uValue));
        aFrom._uValue._pValueRaw = nullptr;
        aFrom._eType = jsonType::jsonNull;
    } else {
        pjsonImpl::_copyContentsInto(*this, aFrom);
        aFrom.reset();
    }
}
// Replaces this value from an rvalue, using constant-time transfer only when
// both allocator domains match. Self-move is a no-op.
//
// Aliasing safety (PJSON-COR-002): aFrom may be an ancestor or descendant of
// *this. The previous implementation called reset() before reading aFrom, which
// freed aFrom's storage when aFrom lived inside *this's subtree (heap
// use-after-free). Instead, first steal aFrom's inline storage into a local
// snapshot in O(1), then swap that snapshot into *this. Our previous contents
// end up in the snapshot and are released by its destructor, after aFrom's
// storage has already been safely adopted.
pjson& pjson::operator=(pjson&& aFrom) {
    if (&aFrom == this)
        return *this;

    if (_allocator == aFrom._allocator) {
        pjson snapshot(*_allocator);              // null placeholder in the same allocator domain
        pjsonImpl::_swapStorage(snapshot, aFrom); // snapshot adopts aFrom's storage; aFrom -> null
        pjsonImpl::_swapStorage(*this, snapshot); // *this adopts that storage
        // snapshot's destructor frees our previous contents, which may include the
        // now-null aFrom node when aFrom was one of our descendants.
    } else {
        pjson tmp(std::move(aFrom), *_allocator);
        pjsonImpl::_swapStorage(*this, tmp);
    }

    return *this;
}
// O(1) exchange of two nodes' contents (type tag + inline storage). noexcept,
// which is what lets the move operations and copy-and-swap assignment below
// offer their exception guarantees.
//
// Aliasing safety (PJSON-COR-002): swapping a node with one of its own
// ancestors or descendants would splice a container into its own child slot and
// create an ownership cycle. Such an overlapping swap is rejected as a safe
// no-op; callers that need it should copy instead. canSwap() already rejects
// cross-allocator pairs.
void pjson::swap(pjson& aOther) noexcept {
    if (this == &aOther || !canSwap(aOther))
        return;
    if (pjsonImpl::_containsNode(*this, &aOther) || pjsonImpl::_containsNode(aOther, this))
        return;
    pjsonImpl::_swapStorage(*this, aOther);
}
// Performs the raw storage exchange with no aliasing or allocator checks. Used
// internally where the caller has already established that the two nodes are
// distinct, non-overlapping, and share an allocator domain.
/*static*/
void pjsonImpl::_swapStorage(pjson& aLeft, pjson& aRight) noexcept {
    static_assert(std::is_trivially_copyable<pjson::Storage>::value,
                  "pjson storage must remain safe for bytewise swap");
    std::swap(aLeft._eType, aRight._eType);
    pjson::Storage temp;
    std::memcpy(&temp, &aLeft._uValue, sizeof(temp));
    std::memcpy(&aLeft._uValue, &aRight._uValue, sizeof(aLeft._uValue));
    std::memcpy(&aRight._uValue, &temp, sizeof(aRight._uValue));
}
// Reports whether aNode is aRoot or a descendant of it. The walk is iterative so
// it stays stack-safe on deep documents and never allocates on the hot path.
/*static*/
bool pjsonImpl::_containsNode(const pjson& aRoot, const pjson* aNode) noexcept {
    if (aNode == nullptr)
        return false;
    std::vector<const pjson*> work;
    work.push_back(&aRoot);
    while (!work.empty()) {
        const pjson* cur = work.back();
        work.pop_back();
        if (cur == aNode)
            return true;
        if (cur->_eType == jsonType::jsonArray) {
            const PJSONARRAY& arr = *cur->_uValue._pValueArray;
            for (size_t i = 0; i < arr.size(); ++i)
                work.push_back(arr[i]);
        } else if (cur->_eType == jsonType::jsonObject) {
            const PJSONMAP& obj = *cur->_uValue._pValueMap;
            for (PJSONMAP::const_iterator it = obj.begin(); it != obj.end(); ++it)
                work.push_back(it->second);
        }
    }
    return false;
}
// Returns the allocator permanently associated with this value and its descendants.
pjson::Allocator& pjson::getAllocator() const noexcept {
    return *_allocator;
}
// Reports whether contents can be exchanged without crossing allocator domains.
bool pjson::canSwap(const pjson& aOther) const noexcept {
    return _allocator == aOther._allocator;
}
// Copy assignment (copy-and-swap: safe even when aFrom aliases a child of
// this, because the deep copy completes before any of our storage is freed).
pjson& pjson::operator=(const pjson& aFrom) {
    if (&aFrom == this)
        return *this;

    pjson tmp(aFrom, *_allocator);
    swap(tmp);
    return *this;
}
// Returns the active storage tag.
pjson::jsonType pjson::getType() const {
    return _eType;
};
// Type predicates inspect the tag only and never coerce the stored value.
bool pjson::isNull() const {
    return _eType == jsonNull;
}
bool pjson::isString() const {
    return _eType == jsonString;
}
bool pjson::isNumber() const {
    return _eType == jsonNumberInt || _eType == jsonNumberUInt || _eType == jsonNumberDouble;
}
bool pjson::isInt() const {
    return _eType == jsonNumberInt;
}
bool pjson::isUInt() const {
    return _eType == jsonNumberUInt;
}
bool pjson::isInteger() const {
    return _eType == jsonNumberInt || _eType == jsonNumberUInt;
}
bool pjson::isDouble() const {
    return _eType == jsonNumberDouble;
}
bool pjson::isBool() const {
    return _eType == jsonBoolean;
}
bool pjson::isArray() const {
    return _eType == jsonArray;
}
bool pjson::isObject() const {
    return _eType == jsonObject;
}
// Constructs an empty non-owning view.
pjson::StringView::StringView() noexcept
        : _data(nullptr)
        , _size(0) {}
// Constructs a non-owning byte view; the caller controls the pointed-to lifetime.
pjson::StringView::StringView(const char* aData, size_t aSize) noexcept
        : _data(aData)
        , _size(aSize) {}
// Returns the first viewed byte, which may be null for an empty default view.
const char* pjson::StringView::data() const noexcept {
    return _data;
}
// Returns the number of viewed bytes.
size_t pjson::StringView::size() const noexcept {
    return _size;
}
// Reports whether the view contains no bytes.
bool pjson::StringView::empty() const noexcept {
    return _size == 0;
}
// Exact extraction overloads leave the destination unchanged on type mismatch.
// A signed read accepts an unsigned value only when it fits in int64_t; an
// unsigned read accepts a signed value only when it is non-negative; a double
// read widens either integer representation.
bool pjson::tryGet(int64_t& aResult) const noexcept {
    if (_eType == jsonType::jsonNumberInt) {
        aResult = _uValue._valueInt;
        return true;
    }
    if (_eType == jsonType::jsonNumberUInt &&
        _uValue._valueUInt <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        aResult = static_cast<int64_t>(_uValue._valueUInt);
        return true;
    }
    return false;
}
bool pjson::tryGet(uint64_t& aResult) const noexcept {
    if (_eType == jsonType::jsonNumberUInt) {
        aResult = _uValue._valueUInt;
        return true;
    }
    if (_eType == jsonType::jsonNumberInt && _uValue._valueInt >= 0) {
        aResult = static_cast<uint64_t>(_uValue._valueInt);
        return true;
    }
    return false;
}
bool pjson::tryGet(double& aResult) const noexcept {
    if (_eType == jsonType::jsonNumberInt) {
        aResult = static_cast<double>(_uValue._valueInt);
        return true;
    }
    if (_eType == jsonType::jsonNumberUInt) {
        aResult = static_cast<double>(_uValue._valueUInt);
        return true;
    }
    if (_eType != jsonType::jsonNumberDouble)
        return false;
    aResult = _uValue._valueDouble;
    return true;
}
bool pjson::tryGet(bool& aResult) const noexcept {
    if (_eType != jsonType::jsonBoolean)
        return false;
    aResult = _uValue._valueBool;
    return true;
}
bool pjson::tryGet(std::string& aResult) const {
    if (_eType != jsonType::jsonString)
        return false;
    aResult = *_uValue._pValueString;
    return true;
}
bool pjson::tryGet(StringView& aResult) const noexcept {
    if (_eType != jsonType::jsonString)
        return false;
    const std::string& value = *_uValue._pValueString;
    aResult = StringView(value.data(), value.size());
    return true;
}
// Resets to the canonical null state, releasing any owned subtree.
void pjson::reset() {
    resetTo(jsonType::jsonNull);
}
// Idempotent reset: rebuild as an empty value of aeType only when the node is
// not already that type, so an existing array/object keeps its contents.
void pjson::resetIfNeeded(jsonType aeType) {
    if (_eType != aeType) {
        resetTo(aeType);
    }
}
// Rebuilds storage for aeType and initializes its empty/default value. The new
// container/string allocation occurs before teardown to preserve validity on throw.
void pjson::resetTo(pjson::jsonType aeType) {
    // Reject forged enum values before allocation or teardown so the strong
    // exception guarantee also covers an invalid requested discriminator.
    // jsonNumberUInt is the highest-valued tag (see the header enum).
    if (aeType < jsonType::jsonNull || aeType > jsonType::jsonNumberUInt)
        throw std::invalid_argument("invalid pjson::jsonType");

    // Allocate the replacement before destroying the current value. If an
    // allocation fails, *this remains unchanged and internally valid.
    void* replacement = nullptr;
    switch (aeType) {
        case jsonType::jsonString:
            replacement = allocateDomObject<std::string>(*_allocator, Allocator::StringAllocation);
            break;
        case jsonType::jsonArray:
            replacement = allocateDomObject<PJSONARRAY>(*_allocator, Allocator::ArrayAllocation);
            break;
        case jsonType::jsonObject:
            replacement = allocateDomObject<PJSONMAP>(*_allocator, Allocator::ObjectAllocation);
            break;
        default:
            break;
    }

    switch (_eType) {
        case jsonType::jsonNull: {
            _uValue._pValueRaw = nullptr;
            break;
        }
        case jsonType::jsonString: {
            destroyDomObject(*_allocator, _uValue._pValueString, Allocator::StringAllocation);
            break;
        }
        case jsonType::jsonNumberInt:
        case jsonType::jsonNumberUInt:
        case jsonType::jsonNumberDouble:
        case jsonType::jsonBoolean:
            break;
        case jsonType::jsonArray: {
            // Free descendants iteratively (safe on deep trees), then the vector.
            pjsonImpl::_disposeChildren(*this);
            destroyDomObject(*_allocator, _uValue._pValueArray, Allocator::ArrayAllocation);
            break;
        }
        case jsonType::jsonObject: {
            pjsonImpl::_disposeChildren(*this);
            destroyDomObject(*_allocator, _uValue._pValueMap, Allocator::ObjectAllocation);
            break;
        }
    } // end switch
    _uValue._pValueRaw = nullptr;

    switch (aeType) {
        case jsonType::jsonNull: { /* _uValue._pValueRaw = nullptr; */
            break;
        }
        case jsonType::jsonString: {
            _uValue._pValueString = static_cast<std::string*>(replacement);
            break;
        }
        case jsonType::jsonNumberInt: {
            _uValue._valueInt = 0;
            break;
        }
        case jsonType::jsonNumberUInt: {
            _uValue._valueUInt = 0;
            break;
        }
        case jsonType::jsonNumberDouble: {
            _uValue._valueDouble = 0.0;
            break;
        }
        case jsonType::jsonBoolean: {
            _uValue._valueBool = false;
            break;
        }
        case jsonType::jsonArray: {
            _uValue._pValueArray = static_cast<PJSONARRAY*>(replacement);
            break;
        }
        case jsonType::jsonObject: {
            _uValue._pValueMap = static_cast<PJSONMAP*>(replacement);
            break;
        }
    } // end switch
    _eType = aeType;
}
// Replaces this value with a deep copy allocated in this value's allocator.
// Building the replacement first gives the operation a strong guarantee.
void pjson::copyFrom(const pjson& aFrom) {
    if (this == &aFrom)
        return;
    pjson replacement(aFrom, *_allocator);
    swap(replacement);
}
// Populates aDst from aFrom without recursion. If copying fails, partial
// descendants are reclaimed and aDst is reset to a valid null state.
/*static*/
void pjsonImpl::_copyContentsInto(pjson& aDst, const pjson& aFrom) {
    // Iterative deep copy. A recursive copy would overflow the stack on very
    // deep documents, so we walk with an explicit work-list: each item pairs a
    // source node with the destination node to populate from it. Scalars are
    // copied immediately; array/map children are queued.
    try {
        aDst.resetTo(aFrom.getType());
        if (aDst._eType != jsonType::jsonArray && aDst._eType != jsonType::jsonObject) {
            switch (aDst._eType) {
                case jsonType::jsonString:
                    *aDst._uValue._pValueString = *(aFrom._uValue._pValueString);
                    break;
                case jsonType::jsonNumberInt:
                    aDst._uValue._valueInt = aFrom._uValue._valueInt;
                    break;
                case jsonType::jsonNumberUInt:
                    aDst._uValue._valueUInt = aFrom._uValue._valueUInt;
                    break;
                case jsonType::jsonNumberDouble:
                    aDst._uValue._valueDouble = aFrom._uValue._valueDouble;
                    break;
                case jsonType::jsonBoolean:
                    aDst._uValue._valueBool = aFrom._uValue._valueBool;
                    break;
                default:
                    break; // null: nothing to copy
            }
            return;
        }

        struct Item {
            const pjson* src;
            pjson* dst;
        };
        std::vector<Item> work;
        Item start = {&aFrom, &aDst};
        work.push_back(start);

        while (!work.empty()) {
            Item cur = work.back();
            work.pop_back();
            const pjson& src = *cur.src;
            pjson& dst = *cur.dst;

            // dst has already been resetTo(src type) by the parent (or caller).
            if (src._eType == jsonType::jsonArray) {
                dst._uValue._pValueArray->reserve(src._uValue._pValueArray->size());
                for (const pjson* elem : *src._uValue._pValueArray) {
                    pjsonImpl::OwnedNode child = pjsonImpl::_makeNode(*dst._allocator);
                    child->resetTo(elem->getType());
                    dst._uValue._pValueArray->push_back(child.get());
                    pjson* attached = child.release();
                    if (elem->_eType == jsonType::jsonArray ||
                        elem->_eType == jsonType::jsonObject) {
                        Item it = {elem, attached};
                        work.push_back(it);
                    } else {
                        pjsonImpl::_copyContentsInto(*attached, *elem);
                    }
                }
            } else { // jsonObject
                for (const auto& kv : *src._uValue._pValueMap) {
                    pjsonImpl::OwnedNode child = pjsonImpl::_makeNode(*dst._allocator);
                    child->resetTo(kv.second->getType());
                    const std::pair<PJSONMAP::iterator, bool> inserted =
                        dst._uValue._pValueMap->insert(
                            std::make_pair(kv.first, static_cast<pjson*>(nullptr)));
                    if (!inserted.second)
                        throw std::logic_error("duplicate key while copying pjson object");
                    pjson* attached = child.release();
                    inserted.first->second = attached;
                    if (kv.second->_eType == jsonType::jsonArray ||
                        kv.second->_eType == jsonType::jsonObject) {
                        Item it = {kv.second, attached};
                        work.push_back(it);
                    } else {
                        pjsonImpl::_copyContentsInto(*attached, *kv.second);
                    }
                }
            }
        }
    } catch (...) {
        aDst.reset();
        throw;
    }
}
// Frees every descendant of node iteratively, so tearing down a very deep
// tree cannot overflow the call stack (as the recursive destructor would).
// Leaves node's own top-level array/map allocated but empty.
/*static*/
void pjsonImpl::_disposeChildren(pjson& node) noexcept {
    if (node._eType != jsonType::jsonArray && node._eType != jsonType::jsonObject) {
        return;
    }
    // Use an intrusive pending list so teardown never allocates and therefore
    // remains noexcept even for very deep trees or an exhausted heap.
    pjson* pending = nullptr;
    if (node._eType == jsonType::jsonArray) {
        for (pjson* c : *node._uValue._pValueArray) {
            c->_disposeNext = pending;
            pending = c;
        }
        node._uValue._pValueArray->clear();
    } else {
        for (const auto& kv : *node._uValue._pValueMap) {
            kv.second->_disposeNext = pending;
            pending = kv.second;
        }
        node._uValue._pValueMap->clear();
    }

    while (pending != nullptr) {
        pjson* p = pending;
        pending = p->_disposeNext;
        p->_disposeNext = nullptr;
        // Move this node's children into the work-list, then detach so its own
        // destructor has nothing left to recurse into.
        if (p->_eType == jsonType::jsonArray) {
            for (pjson* c : *p->_uValue._pValueArray) {
                c->_disposeNext = pending;
                pending = c;
            }
            p->_uValue._pValueArray->clear();
        } else if (p->_eType == jsonType::jsonObject) {
            for (const auto& kv : *p->_uValue._pValueMap) {
                kv.second->_disposeNext = pending;
                pending = kv.second;
            }
            p->_uValue._pValueMap->clear();
        }
        pjsonImpl::_destroyNode(p); // now a leaf (or emptied container)
    }
}
// Recognizes exactly the whitespace code points admitted by the JSON grammar.
bool pjsonImpl::_isWhitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}
// Encodes a Unicode code point as UTF-8 and appends it to aOut.
/*static*/
void pjsonImpl::_appendUtf8(uint32_t aCodePoint, std::string& aOut) {
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
bool pjsonImpl::_hex4(const char* aSrc, size_t aStart, uint32_t& aOut) {
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
int pjsonImpl::_utf8Len(const char* src, size_t pos, size_t end) {
    unsigned char c0 = static_cast<unsigned char>(src[pos]);
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
        return 0; // stray continuation / invalid lead
    if (pos + static_cast<size_t>(n) > end)
        return 0;
    for (int k = 1; k < n; ++k) {
        unsigned char ck = static_cast<unsigned char>(src[pos + k]);
        if ((ck & 0xC0) != 0x80)
            return 0; // not a continuation byte
        cp = (cp << 6) | (ck & 0x3F);
    }
    if (cp < lo)
        return 0; // overlong encoding
    if (cp > 0x10FFFF)
        return 0; // beyond Unicode
    if (cp >= 0xD800 && cp <= 0xDFFF)
        return 0; // surrogate half in UTF-8
    return n;
}
// Records the first parse error (byte offset + message) and returns false so
// callers can `return _fail(...)`.
/*static*/
bool pjsonImpl::_fail(ParseCtx& c, size_t aPos, const char* aMsg) {
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
pjson* pjsonImpl::_newNode(ParseCtx& c) {
    if (c.maxNodes != 0 && c.nodeCount >= c.maxNodes) {
        _fail(c, c.pos, "document too large (node budget exceeded)");
        return nullptr;
    }
    ++c.nodeCount;
    return pjsonImpl::_allocateNode(*c.allocator);
}
/*static*/
// Allocates a null node under the supplied allocator and wraps origin-aware cleanup.
pjsonImpl::OwnedNode pjsonImpl::_makeNode(pjson::Allocator& aAlloc) {
    return pjsonImpl::OwnedNode(pjsonImpl::_allocateNode(aAlloc));
}
/*static*/
// Deep-clones a complete subtree into the supplied allocator domain.
pjsonImpl::OwnedNode pjsonImpl::_cloneNode(const pjson& aValue, pjson::Allocator& aAlloc) {
    pjsonImpl::OwnedNode result = _makeNode(aAlloc);
    _copyContentsInto(*result, aValue);
    return result;
}
// Decodes a JSON string body from c.pos into aOut. With bStopAtQuote, decoding
// stops at (and consumes) the first unescaped '"'. RFC 8259-invalid escapes,
// control bytes, surrogate halves, and UTF-8 are rejected.
/*static*/
bool pjsonImpl::_decodeStringBody(ParseCtx& c, std::string& aOut, bool bStopAtQuote) {
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
                return _fail(c, c.pos, "dangling escape at end of input");
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
                    if (c.pos + 4 > c.end || !pjsonImpl::_hex4(c.src, c.pos, cp)) {
                        return _fail(c, c.pos, "invalid \\u escape");
                    }
                    c.pos += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        // High surrogate: look for a following low surrogate.
                        uint32_t low = 0;
                        if (c.pos + 6 <= c.end && c.src[c.pos] == '\\' && c.src[c.pos + 1] == 'u' &&
                            pjsonImpl::_hex4(c.src, c.pos + 2, low) && low >= 0xDC00 &&
                            low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            c.pos += 6;
                        } else {
                            return _fail(c, c.pos, "unpaired high surrogate");
                        }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return _fail(c, c.pos, "unpaired low surrogate");
                    }
                    _appendUtf8(cp, aOut);
                    break;
                }
                default:
                    return _fail(c, c.pos - 1, "invalid escape sequence");
            }
        } else if (ch < 0x20) {
            return _fail(c, c.pos, "unescaped control character in string");
        } else if (ch >= 0x80) {
            int n = pjsonImpl::_utf8Len(c.src, c.pos, c.end);
            if (n == 0) {
                return _fail(c, c.pos, "invalid UTF-8 sequence");
            }
            aOut.append(c.src + c.pos, static_cast<size_t>(n));
            c.pos += static_cast<size_t>(n);
        } else {
            aOut += static_cast<char>(ch);
            ++c.pos;
        }
    }
    if (bStopAtQuote) {
        return _fail(c, c.pos, "unterminated string");
    }
    return true;
}
// Formats a finite double with enough classic-locale precision to round-trip.
// A '.0' suffix is appended when the result would otherwise look like an
// integer, so the value re-parses into the double representation (type-stable).
/*static*/
std::string pjsonImpl::_formatDouble(double aValue) {
    if (!std::isfinite(aValue)) {
        // JSON has no representation for NaN/Infinity.
        return "null";
    }
    std::string result;
    for (int prec = 15; prec <= 17; ++prec) {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << std::setprecision(prec) << aValue;
        result = out.str();
        double parsed = 0.0;
        if (_parseDouble(result, parsed) && parsed == aValue) {
            break;
        }
    }
    if (result.find_first_of(".eE") == std::string::npos) {
        result += ".0";
    }
    return result;
}
// Parses an ASCII JSON number independently of the process LC_NUMERIC locale.
bool pjsonImpl::_parseDouble(const std::string& aText, double& aValue) {
    std::istringstream in(aText);
    in.imbue(std::locale::classic());
    in >> std::noskipws >> aValue;
    if (!in.fail())
        return in.peek() == std::char_traits<char>::eof();
    // libstdc++/libc++ set failbit as well as eofbit for both underflow and
    // overflow. Classify the range direction from the decimal exponent instead
    // of trusting the implementation-specific saturated result. A negative
    // effective decimal exponent cannot overflow binary64, so its finite zero or
    // subnormal result is valid; nonnegative range failures are overflow.
    if (!in.eof() || !std::isfinite(aValue))
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
    return effectiveExponent < 0;
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

// Streams with compact default options.
void pjson::write(std::ostream& aOut) const {
    write(aOut, SerializeOptions());
}

// Writes this complete DOM incrementally; callers inspect the stream state for
// output errors because the public streaming API reports through std::ostream.
void pjson::write(std::ostream& aOut, const SerializeOptions& aOpts) const {
    pjsonImpl::_writeValue(aOut, *this, aOpts);
}

//===----------------------------------------------------------------------===//
// Scalar assignment and vector-backed array mutation
//
// Scalar assignments reuse compatible storage. Array helpers allocate children
// under RAII and publish raw pointers only after container insertion succeeds.
// Multi-element mutation either swaps a complete replacement or rolls back to
// the original size, so allocation failures never leave a partial append.
//===----------------------------------------------------------------------===//

// Replaces the current value with a copied JSON string.
pjson& pjson::operator=(const std::string& aString) {
    resetIfNeeded(jsonType::jsonString);
    *_uValue._pValueString = aString;
    return *this;
}
// Replaces the current value with the null-terminated string's bytes.
pjson& pjson::operator=(const char* aCString) {
    if (aCString == nullptr)
        throw std::invalid_argument("pjson string assignment requires non-null input");
    resetIfNeeded(jsonType::jsonString);
    *_uValue._pValueString = aCString;
    return *this;
}
// Replaces the current value with a JSON boolean.
pjson& pjson::operator=(const bool aBool) {
    resetIfNeeded(jsonType::jsonBoolean);
    _uValue._valueBool = aBool;
    return *this;
}
// Replaces the current value with a JSON integer.
pjson& pjson::operator=(const int64_t aInt) {
    resetIfNeeded(jsonType::jsonNumberInt);
    _uValue._valueInt = aInt;
    return *this;
}
// Replaces the current value with an unsigned JSON integer, retaining unsigned
// type identity even when the value would also fit in int64_t.
pjson& pjson::operator=(const uint64_t aUInt) {
    resetIfNeeded(jsonType::jsonNumberUInt);
    _uValue._valueUInt = aUInt;
    return *this;
}
// Replaces the current value with a JSON double.
pjson& pjson::operator=(const double aDouble) {
    resetIfNeeded(jsonType::jsonNumberDouble);
    _uValue._valueDouble = aDouble;
    return *this;
}
namespace {
    // Appends one converted child. A non-array target is promoted atomically by
    // building and swapping a replacement; an array target changes only after
    // both node construction and vector growth have succeeded.
    template <typename Value> void appendDomValue(pjson& aTarget, const Value& aValue) {
        if (!aTarget.isArray()) {
            pjson replacement(aTarget.getAllocator());
            replacement.resetTo(pjson::jsonArray);
            pjsonImpl::OwnedNode child = pjsonImpl::_makeNode(aTarget.getAllocator());
            *child = aValue;
            pjsonImpl::_array(replacement).push_back(nullptr);
            pjsonImpl::_array(replacement).back() = child.release();
            aTarget.swap(replacement);
            return;
        }

        pjsonImpl::OwnedNode child = pjsonImpl::_makeNode(aTarget.getAllocator());
        *child = aValue;
        pjsonImpl::_array(aTarget).push_back(nullptr);
        pjsonImpl::_array(aTarget).back() = child.release();
    }

    // Replaces a target with a fully constructed array, providing a strong guarantee.
    template <typename Values> void assignDomArray(pjson& aTarget, const Values& aValues) {
        pjson replacement(aTarget.getAllocator());
        replacement.resetTo(pjson::jsonArray);
        for (const auto& value : aValues) {
            appendDomValue(replacement, value);
        }
        aTarget.swap(replacement);
    }

    // Appends a range atomically: newly attached children are reclaimed in
    // reverse order if any later conversion or allocation throws.
    template <typename Values> void appendDomArray(pjson& aTarget, const Values& aValues) {
        if (!aTarget.isArray()) {
            pjson replacement(aTarget.getAllocator());
            replacement.resetTo(pjson::jsonArray);
            for (const auto& value : aValues) {
                appendDomValue(replacement, value);
            }
            aTarget.swap(replacement);
            return;
        }

        PJSONARRAY& array = pjsonImpl::_array(aTarget);
        const size_t originalSize = array.size();
        try {
            for (const auto& value : aValues) {
                appendDomValue(aTarget, value);
            }
        } catch (...) {
            while (array.size() > originalSize) {
                pjsonImpl::OwnedNode rollback(array.back());
                array.pop_back();
            }
            throw;
        }
    }
} // namespace
// Vector assignment overloads delegate to the same strong-guarantee builder,
// but remain explicit so each supported public type is visible in this source.
pjson& pjson::operator=(const std::vector<bool>& aValueArray) {
    assignDomArray(*this, aValueArray);
    return *this;
}

pjson& pjson::operator=(const std::vector<std::string>& aValueArray) {
    assignDomArray(*this, aValueArray);
    return *this;
}

pjson& pjson::operator=(const std::vector<int64_t>& aValueArray) {
    assignDomArray(*this, aValueArray);
    return *this;
}

pjson& pjson::operator=(const std::vector<uint64_t>& aValueArray) {
    assignDomArray(*this, aValueArray);
    return *this;
}

pjson& pjson::operator=(const std::vector<double>& aValueArray) {
    assignDomArray(*this, aValueArray);
    return *this;
}

// Scalar append overloads promote non-arrays and publish one fully constructed child.
pjson& pjson::operator+=(const std::string& aValue) {
    appendDomValue(*this, aValue);
    return *this;
}
pjson& pjson::operator+=(const char* aValue) {
    if (aValue == nullptr)
        throw std::invalid_argument("pjson string append requires non-null input");
    appendDomValue(*this, aValue);
    return *this;
}
pjson& pjson::operator+=(const bool aValue) {
    appendDomValue(*this, aValue);
    return *this;
}
pjson& pjson::operator+=(const int64_t aValue) {
    appendDomValue(*this, aValue);
    return *this;
}
pjson& pjson::operator+=(const uint64_t aValue) {
    appendDomValue(*this, aValue);
    return *this;
}
pjson& pjson::operator+=(const double aValue) {
    appendDomValue(*this, aValue);
    return *this;
}
// Vector append overloads share the rollback semantics documented by appendDomArray.
pjson& pjson::operator+=(const std::vector<bool>& aValueArray) {
    appendDomArray(*this, aValueArray);
    return *this;
}

pjson& pjson::operator+=(const std::vector<std::string>& aValueArray) {
    appendDomArray(*this, aValueArray);
    return *this;
}

pjson& pjson::operator+=(const std::vector<int64_t>& aValueArray) {
    appendDomArray(*this, aValueArray);
    return *this;
}

pjson& pjson::operator+=(const std::vector<double>& aValueArray) {
    appendDomArray(*this, aValueArray);
    return *this;
}

pjson& pjson::operator+=(const std::vector<uint64_t>& aValueArray) {
    appendDomArray(*this, aValueArray);
    return *this;
}

//===----------------------------------------------------------------------===//
// Factories, checked access, and generic child insertion
//===----------------------------------------------------------------------===//

// Explicit typed factories. Each returns a default-allocator value of the
// requested kind so callers never depend on default construction's type.
pjson pjson::null() {
    return pjson();
}
pjson pjson::object() {
    pjson value;
    value.resetTo(jsonType::jsonObject);
    return value;
}
pjson pjson::array() {
    pjson value;
    value.resetTo(jsonType::jsonArray);
    return value;
}
// Assigning nullptr resets to JSON null, matching null().
pjson& pjson::operator=(std::nullptr_t) {
    reset();
    return *this;
}

// Checked, non-vivifying object access. Throws std::out_of_range on a missing
// key or a non-object receiver, distinguishing it from vivifying operator[].
pjson& pjson::at(const std::string& aKey) {
    pjson* child = find(aKey);
    if (child == nullptr)
        throw std::out_of_range("pjson::at: object key not found");
    return *child;
}
const pjson& pjson::at(const std::string& aKey) const {
    const pjson* child = find(aKey);
    if (child == nullptr)
        throw std::out_of_range("pjson::at: object key not found");
    return *child;
}
// Checked, non-vivifying array access using a non-negative index. Throws
// std::out_of_range for a non-array receiver or an out-of-range index.
pjson& pjson::at(size_t aIndex) {
    if (_eType != jsonType::jsonArray || aIndex >= _uValue._pValueArray->size())
        throw std::out_of_range("pjson::at: array index out of range");
    return *(*_uValue._pValueArray)[aIndex];
}
const pjson& pjson::at(size_t aIndex) const {
    if (_eType != jsonType::jsonArray || aIndex >= _uValue._pValueArray->size())
        throw std::out_of_range("pjson::at: array index out of range");
    return *(*_uValue._pValueArray)[aIndex];
}

// Generic child append. Promotes a non-array target to an array, then attaches
// a deep copy (copy overload) or a moved/cross-allocator-copied value.
pjson& pjson::pushBack(const pjson& aValue) {
    if (_eType != jsonType::jsonArray)
        resetTo(jsonType::jsonArray);
    pjsonImpl::OwnedNode child = pjsonImpl::_makeNode(*_allocator);
    pjsonImpl::_copyContentsInto(*child, aValue);
    _uValue._pValueArray->push_back(nullptr);
    _uValue._pValueArray->back() = child.release();
    return *this;
}
pjson& pjson::pushBack(pjson&& aValue) {
    if (_eType != jsonType::jsonArray)
        resetTo(jsonType::jsonArray);
    pjsonImpl::OwnedNode child = pjsonImpl::_makeNode(*_allocator);
    if (child->_allocator == aValue._allocator) {
        pjsonImpl::_swapStorage(*child, aValue);
        aValue.reset();
    } else {
        pjsonImpl::_copyContentsInto(*child, aValue);
        aValue.reset();
    }
    _uValue._pValueArray->push_back(nullptr);
    _uValue._pValueArray->back() = child.release();
    return *this;
}
// Insert-or-assign an object member from an arbitrary pjson value.
pjson& pjson::insertOrAssign(const std::string& aKey, const pjson& aValue) {
    if (_eType != jsonType::jsonObject)
        resetTo(jsonType::jsonObject);
    pjson& slot = (*this)[aKey];
    slot.copyFrom(aValue);
    return *this;
}
pjson& pjson::insertOrAssign(const std::string& aKey, pjson&& aValue) {
    if (_eType != jsonType::jsonObject)
        resetTo(jsonType::jsonObject);
    pjson& slot = (*this)[aKey];
    slot = std::move(aValue);
    return *this;
}
// Reserves array capacity. Promotes a non-array to an empty array first so the
// reservation is always meaningful; a no-op count of zero still normalizes type.
pjson& pjson::reserve(size_t aCount) {
    if (_eType != jsonType::jsonArray)
        resetTo(jsonType::jsonArray);
    _uValue._pValueArray->reserve(aCount);
    return *this;
}
// contains() is a readable alias for hasKey().
bool pjson::contains(const std::string& aKey) const {
    return hasKey(aKey);
}
bool pjson::contains(const char* aKey) const {
    return hasKey(aKey);
}

//===----------------------------------------------------------------------===//
// Non-allocating traversal (PJSON-API-001)
//
// Callback-style visitors keep the public header declaration-only and ABI
// stable. Each visits borrowed children directly with no key copy and no second
// lookup; aContext carries caller state because a plain function pointer cannot
// capture. Returning false stops early and propagates as the call's result.
//===----------------------------------------------------------------------===//
bool pjson::forEachMember(ConstMemberVisitor aVisitor, void* aContext) const {
    if (_eType != jsonType::jsonObject || aVisitor == nullptr)
        return true;
    for (PJSONMAP::const_iterator it = _uValue._pValueMap->begin(); it != _uValue._pValueMap->end();
         ++it) {
        StringView keyView(it->first.data(), it->first.size());
        if (!aVisitor(keyView, static_cast<const pjson&>(*it->second), aContext))
            return false;
    }
    return true;
}
bool pjson::forEachMember(MemberVisitor aVisitor, void* aContext) {
    if (_eType != jsonType::jsonObject || aVisitor == nullptr)
        return true;
    for (PJSONMAP::iterator it = _uValue._pValueMap->begin(); it != _uValue._pValueMap->end();
         ++it) {
        StringView keyView(it->first.data(), it->first.size());
        if (!aVisitor(keyView, *it->second, aContext))
            return false;
    }
    return true;
}
bool pjson::forEachElement(ConstElementVisitor aVisitor, void* aContext) const {
    if (_eType != jsonType::jsonArray || aVisitor == nullptr)
        return true;
    const PJSONARRAY& arr = *_uValue._pValueArray;
    for (size_t i = 0; i < arr.size(); ++i) {
        if (!aVisitor(static_cast<const pjson&>(*arr[i]), aContext))
            return false;
    }
    return true;
}
bool pjson::forEachElement(ElementVisitor aVisitor, void* aContext) {
    if (_eType != jsonType::jsonArray || aVisitor == nullptr)
        return true;
    PJSONARRAY& arr = *_uValue._pValueArray;
    for (size_t i = 0; i < arr.size(); ++i) {
        if (!aVisitor(*arr[i], aContext))
            return false;
    }
    return true;
}
//===----------------------------------------------------------------------===//
// Container access and lookup
//
// Mutating operator[] access auto-vivifies missing containers and children;
// find() is non-mutating. Negative array indices count from the end. Mutating
// indices before the beginning clamp to zero, while lookup indices simply miss.
//===----------------------------------------------------------------------===//

// Returns or creates an object member, atomically promoting non-object values.
// The std::string overload is the length-aware primary implementation so keys
// containing embedded U+0000 are preserved byte-for-byte; the const char*
// overload deliberately keeps conventional NUL-terminated semantics.
pjson& pjson::operator[](const std::string& aString) {
    if (_eType != jsonType::jsonObject) {
        pjson replacement(*_allocator);
        replacement.resetTo(jsonType::jsonObject);
        pjsonImpl::OwnedNode child = pjsonImpl::_makeNode(*_allocator);
        const std::pair<PJSONMAP::iterator, bool> inserted = replacement._uValue._pValueMap->insert(
            std::make_pair(aString, static_cast<pjson*>(nullptr)));
        pjson* result = child.release();
        inserted.first->second = result;
        swap(replacement);
        return *result;
    }
    PJSONMAP::iterator it = _uValue._pValueMap->find(aString);
    if (it != _uValue._pValueMap->end()) {
        return *(it->second);
    }
    pjsonImpl::OwnedNode child = pjsonImpl::_makeNode(*_allocator);
    const std::pair<PJSONMAP::iterator, bool> inserted =
        _uValue._pValueMap->insert(std::make_pair(aString, static_cast<pjson*>(nullptr)));
    pjson* result = inserted.first->second;
    if (inserted.second) {
        result = child.release();
        inserted.first->second = result;
    }
    return *result;
}
pjson& pjson::operator[](const char* aSkey) {
    if (aSkey == nullptr)
        throw std::invalid_argument("pjson object key requires non-null input");
    return (*this)[std::string(aSkey)];
}
// Returns or creates an array element, filling gaps with null nodes. Any failed
// growth destroys every node appended by this call before rethrowing.
pjson& pjson::operator[](int index) {
    if (_eType != jsonType::jsonArray) {
        pjson replacement(*_allocator);
        replacement.resetTo(jsonType::jsonArray);
        pjson& result = replacement[index];
        pjson* resultPtr = &result;
        swap(replacement);
        return *resultPtr;
    }
    PJSONARRAY& array = *_uValue._pValueArray;
    size_t position = 0;
    if (index < 0) {
        // Negative indexes count from the end: -1 is the last element.
        const size_t fromEnd = static_cast<size_t>(-(index + 1)) + size_t(1);
        position = fromEnd > array.size() ? size_t(0) : array.size() - fromEnd;
    } else {
        position = static_cast<size_t>(index);
    }

    if (position >= array.size()) {
        static const size_t kMaxAutoGrowth = size_t(1000000);
        const size_t growth = position - array.size() + size_t(1);
        if (growth > kMaxAutoGrowth)
            throw std::length_error("pjson array auto-growth exceeds safety limit");
        if (position == std::numeric_limits<size_t>::max() ||
            position + size_t(1) > array.max_size())
            throw std::length_error("pjson array index exceeds maximum size");
        const size_t requiredSize = position + size_t(1);
        const size_t originalSize = array.size();
        try {
            array.reserve(requiredSize);
            while (array.size() < requiredSize) {
                pjsonImpl::OwnedNode child = pjsonImpl::_makeNode(*_allocator);
                array.push_back(nullptr);
                array.back() = child.release();
            }
        } catch (...) {
            while (array.size() > originalSize) {
                pjsonImpl::OwnedNode rollback(array.back());
                array.pop_back();
            }
            throw;
        }
    }

    return *array[position];
}
// Length-aware object lookup: the std::string overload is the primary form so
// keys containing embedded U+0000 resolve on their full byte sequence. The
// const char* overloads keep conventional NUL-terminated behavior.
pjson* pjson::find(const std::string& aKey) {
    if (_eType == jsonType::jsonObject) {
        auto it = _uValue._pValueMap->find(aKey);
        if (it != _uValue._pValueMap->end()) {
            return it->second;
        }
    }
    return nullptr;
}
// Finds an object member without inserting or changing the receiver.
pjson* pjson::find(const char* aKey) {
    if (aKey != nullptr)
        return find(std::string(aKey));
    return nullptr;
}
const pjson* pjson::find(const std::string& aKey) const {
    return const_cast<pjson*>(this)->find(aKey);
}
const pjson* pjson::find(const char* aKey) const {
    if (aKey != nullptr)
        return find(std::string(aKey));
    return nullptr;
}
pjson* pjson::find(int aIndex) noexcept {
    return const_cast<pjson*>(static_cast<const pjson*>(this)->find(aIndex));
}
// Finds an array element with end-relative negative-index support.
const pjson* pjson::find(int aIndex) const noexcept {
    if (_eType != jsonType::jsonArray)
        return nullptr;

    const PJSONARRAY& values = *_uValue._pValueArray;
    size_t position = 0;
    if (aIndex >= 0) {
        position = static_cast<size_t>(aIndex);
        if (position >= values.size())
            return nullptr;
    } else {
        const size_t fromEnd = static_cast<size_t>(-(aIndex + 1)) + size_t(1);
        if (fromEnd > values.size())
            return nullptr;
        position = values.size() - fromEnd;
    }
    return values[position];
}

//===----------------------------------------------------------------------===//
// RFC 6901 JSON Pointer decoding and traversal
//
// Pointer syntax is decoded once into unescaped tokens, then traversed without
// mutating the DOM. Array tokens must be canonical unsigned decimals: no sign,
// leading zero, or size_t overflow. The '-' token is reserved for Patch add.
//===----------------------------------------------------------------------===//

namespace {
    // Separates malformed decimal syntax from arithmetic overflow so public
    // diagnostics can distinguish invalid and merely out-of-range indices.
    enum PointerIndexResult { PointerIndexOk, PointerIndexInvalid, PointerIndexOverflow };

    // Restores a reusable PointerError to its successful neutral state.
    void resetPointerError(pjson::PointerError& aError) {
        aError.ok = true;
        aError.code = pjson::PointerError::Ok;
        aError.pointer.clear();
        aError.tokenIndex = 0;
        aError.token.clear();
        aError.message.clear();
    }

    // Records the first-class pointer, token location, and failure category.
    bool failPointer(pjson::PointerError& aError, pjson::PointerError::Code aCode,
                     const std::string& aPointer, size_t aTokenIndex, const std::string& aToken,
                     const char* aMessage) {
        aError.ok = false;
        aError.code = aCode;
        aError.pointer = aPointer;
        aError.tokenIndex = aTokenIndex;
        aError.token = aToken;
        aError.message = aMessage;
        return false;
    }

    // Parses RFC 6901's canonical array-index subset without overflowing size_t.
    PointerIndexResult parsePointerIndex(const std::string& aToken, size_t& aIndex) {
        if (aToken.empty() || (aToken.size() > 1 && aToken[0] == '0'))
            return PointerIndexInvalid;

        size_t value = 0;
        for (size_t i = 0; i < aToken.size(); ++i) {
            const unsigned char ch = static_cast<unsigned char>(aToken[i]);
            if (ch < static_cast<unsigned char>('0') || ch > static_cast<unsigned char>('9'))
                return PointerIndexInvalid;
            const size_t digit = static_cast<size_t>(ch - static_cast<unsigned char>('0'));
            if (value > (std::numeric_limits<size_t>::max() - digit) / size_t(10))
                return PointerIndexOverflow;
            value = value * size_t(10) + digit;
        }
        aIndex = value;
        return PointerIndexOk;
    }

    // Splits a pointer and decodes ~0/~1 escapes. An empty pointer deliberately
    // yields no tokens because it identifies the document root.
    bool decodePointer(const std::string& aPointer, std::vector<std::string>& aTokens,
                       pjson::PointerError& aError) {
        resetPointerError(aError);
        aTokens.clear();
        if (aPointer.empty())
            return true;
        if (aPointer[0] != '/')
            return failPointer(aError, pjson::PointerError::InvalidSyntax, aPointer, 0,
                               std::string(), "JSON Pointer must be empty or begin with '/'");

        size_t tokenIndex = 0;
        size_t tokenStart = 1;
        while (true) {
            const size_t slash = aPointer.find('/', tokenStart);
            const size_t tokenEnd = slash == std::string::npos ? aPointer.size() : slash;
            std::string decoded;
            decoded.reserve(tokenEnd - tokenStart);
            for (size_t i = tokenStart; i < tokenEnd; ++i) {
                const char ch = aPointer[i];
                if (ch != '~') {
                    decoded += ch;
                    continue;
                }
                if (i + 1 >= tokenEnd || (aPointer[i + 1] != '0' && aPointer[i + 1] != '1')) {
                    return failPointer(aError, pjson::PointerError::InvalidEscape, aPointer,
                                       tokenIndex,
                                       aPointer.substr(tokenStart, tokenEnd - tokenStart),
                                       "JSON Pointer token contains an invalid '~' escape");
                }
                decoded += aPointer[i + 1] == '0' ? '~' : '/';
                ++i;
            }
            aTokens.push_back(std::move(decoded));
            if (slash == std::string::npos)
                break;
            tokenStart = slash + 1;
            ++tokenIndex;
        }
        return true;
    }

    // Traverses the first aCount decoded tokens and reports the exact failing
    // token. Patch reuses partial traversal to resolve a destination's parent.
    const pjson* resolvePointerTokens(const pjson& aRoot, const std::vector<std::string>& aTokens,
                                      size_t aCount, const std::string& aPointer,
                                      pjson::PointerError& aError) {
        const pjson* current = &aRoot;
        for (size_t i = 0; i < aCount; ++i) {
            const std::string& token = aTokens[i];
            if (current->isObject()) {
                const PJSONMAP* object = &pjsonImpl::_object(*current);
                PJSONMAP::const_iterator found = object->find(token);
                if (found == object->end()) {
                    failPointer(aError, pjson::PointerError::MissingTarget, aPointer, i, token,
                                "JSON Pointer object member does not exist");
                    return nullptr;
                }
                current = found->second;
                continue;
            }
            if (current->isArray()) {
                if (token == "-") {
                    failPointer(aError, pjson::PointerError::AppendTokenNotAllowed, aPointer, i,
                                token, "the '-' token is only valid for JSON Patch add");
                    return nullptr;
                }
                size_t index = 0;
                const PointerIndexResult indexResult = parsePointerIndex(token, index);
                if (indexResult == PointerIndexInvalid) {
                    failPointer(aError, pjson::PointerError::InvalidArrayIndex, aPointer, i, token,
                                "JSON Pointer array index is not canonical decimal");
                    return nullptr;
                }
                const PJSONARRAY* array = &pjsonImpl::_array(*current);
                if (indexResult == PointerIndexOverflow || index >= array->size()) {
                    failPointer(aError, pjson::PointerError::ArrayIndexOutOfRange, aPointer, i,
                                token, "JSON Pointer array index is out of range");
                    return nullptr;
                }
                current = (*array)[index];
                continue;
            }
            failPointer(aError, pjson::PointerError::ExpectedContainer, aPointer, i, token,
                        "JSON Pointer traversal reached a non-container value");
            return nullptr;
        }
        return current;
    }
} // namespace

/*static*/
// Encodes one object-key token for insertion into a JSON Pointer path.
std::string pjson::escapePointerToken(const std::string& aToken) {
    std::string escaped;
    escaped.reserve(aToken.size());
    for (size_t i = 0; i < aToken.size(); ++i) {
        if (aToken[i] == '~')
            escaped += "~0";
        else if (aToken[i] == '/')
            escaped += "~1";
        else
            escaped += aToken[i];
    }
    return escaped;
}
// Resolves a string pointer without throwing; exceptional failures are mapped
// to PointerError so nullptr always means a diagnosed failure.
const pjson* pjson::findPointer(const std::string& aPointer, PointerError& aError) const {
    try {
        std::vector<std::string> tokens;
        if (!decodePointer(aPointer, tokens, aError))
            return nullptr;
        return resolvePointerTokens(*this, tokens, tokens.size(), aPointer, aError);
    } catch (const std::bad_alloc&) {
        try {
            failPointer(aError, PointerError::AllocationFailure, std::string(), 0, std::string(),
                        "JSON Pointer ran out of memory");
        } catch (...) {
            aError.ok = false;
            aError.code = PointerError::AllocationFailure;
        }
        return nullptr;
    } catch (...) {
        try {
            failPointer(aError, PointerError::InternalError, std::string(), 0, std::string(),
                        "JSON Pointer failed with an internal exception");
        } catch (...) {
            aError.ok = false;
            aError.code = PointerError::InternalError;
        }
        return nullptr;
    }
}
// Mutable forwarding overload; traversal semantics remain non-creating.
pjson* pjson::findPointer(const std::string& aPointer, PointerError& aError) {
    return const_cast<pjson*>(static_cast<const pjson*>(this)->findPointer(aPointer, aError));
}
// Convenience overload that intentionally discards pointer diagnostics.
const pjson* pjson::findPointer(const std::string& aPointer) const {
    PointerError error;
    return findPointer(aPointer, error);
}
// Mutable convenience overload that intentionally discards diagnostics.
pjson* pjson::findPointer(const std::string& aPointer) {
    return const_cast<pjson*>(static_cast<const pjson*>(this)->findPointer(aPointer));
}
// Null-safe C-string overload; a null pointer is invalid syntax, not the root.
const pjson* pjson::findPointer(const char* aPointer, PointerError& aError) const {
    try {
        if (aPointer != nullptr)
            return findPointer(std::string(aPointer), aError);
        resetPointerError(aError);
        failPointer(aError, PointerError::InvalidSyntax, std::string(), 0, std::string(),
                    "JSON Pointer input is null");
        return nullptr;
    } catch (const std::bad_alloc&) {
        aError.ok = false;
        aError.code = PointerError::AllocationFailure;
        return nullptr;
    } catch (...) {
        aError.ok = false;
        aError.code = PointerError::InternalError;
        return nullptr;
    }
}
// Mutable null-safe C-string forwarding overload.
pjson* pjson::findPointer(const char* aPointer, PointerError& aError) {
    return const_cast<pjson*>(static_cast<const pjson*>(this)->findPointer(aPointer, aError));
}
// C-string convenience overload that intentionally discards diagnostics.
const pjson* pjson::findPointer(const char* aPointer) const {
    PointerError error;
    return findPointer(aPointer, error);
}
// Mutable C-string convenience overload that intentionally discards diagnostics.
pjson* pjson::findPointer(const char* aPointer) {
    return const_cast<pjson*>(static_cast<const pjson*>(this)->findPointer(aPointer));
}

//===----------------------------------------------------------------------===//
// RFC 6902 JSON Patch and RFC 7396 Merge Patch helpers
//
// Helpers accept ownership of values through pjsonImpl::OwnedNode and release only after
// attachment, so failed insertions cannot leak. Public entry points work on a
// full allocator-local clone and swap it into place only after every operation
// succeeds, giving both patch formats document-level atomicity.
//===----------------------------------------------------------------------===//

namespace {
    typedef pjson::PatchError PatchError;
    bool failPatch(PatchError& aError, PatchError::Code aCode, const char* aMessage);

    struct PatchBudget {
        size_t operations;
        size_t nodes;
        size_t bytes;
        size_t work;
        size_t operationLimit;
        size_t nodeLimit;
        size_t byteLimit;
        size_t workLimit;

        explicit PatchBudget(const pjson::PatchOptions& options)
                : operations(0)
                , nodes(0)
                , bytes(0)
                , work(0)
                , operationLimit(options.maxOperations == 0 ? size_t(10000) : options.maxOperations)
                , nodeLimit(options.maxClonedNodes == 0 ? size_t(1000000) : options.maxClonedNodes)
                , byteLimit(options.maxClonedBytes == 0 ? size_t(64) * 1024U * 1024U
                                                        : options.maxClonedBytes)
                , workLimit(options.maxWork == 0 ? size_t(1000000) : options.maxWork) {}
    };

    bool chargePatch(size_t& used, size_t limit, size_t amount, PatchError& error,
                     const char* message) {
        if (amount > limit - std::min(used, limit))
            return failPatch(error, PatchError::ResourceLimit, message);
        used += amount;
        return true;
    }

    bool measureClone(const pjson& value, PatchBudget& budget, PatchError& error) {
        std::vector<const pjson*> work;
        work.push_back(&value);
        while (!work.empty()) {
            if (!chargePatch(budget.work, budget.workLimit, 1, error,
                             "JSON patch work budget exceeded") ||
                !chargePatch(budget.nodes, budget.nodeLimit, 1, error,
                             "JSON patch cloned-node budget exceeded") ||
                !chargePatch(budget.bytes, budget.byteLimit, sizeof(pjson), error,
                             "JSON patch cloned-byte budget exceeded"))
                return false;
            const pjson* current = work.back();
            work.pop_back();
            if (current->isString()) {
                if (!chargePatch(budget.bytes, budget.byteLimit,
                                 pjsonImpl::_string(*current).size(), error,
                                 "JSON patch cloned-byte budget exceeded"))
                    return false;
            } else if (current->isArray()) {
                const PJSONARRAY& array = pjsonImpl::_array(*current);
                const size_t remainingWork =
                    budget.workLimit - std::min(budget.work, budget.workLimit);
                if (array.size() > remainingWork)
                    return failPatch(error, PatchError::ResourceLimit,
                                     "JSON patch work budget exceeded");
                work.insert(work.end(), array.begin(), array.end());
            } else if (current->isObject()) {
                const PJSONMAP& object = pjsonImpl::_object(*current);
                const size_t remainingWork =
                    budget.workLimit - std::min(budget.work, budget.workLimit);
                if (object.size() > remainingWork)
                    return failPatch(error, PatchError::ResourceLimit,
                                     "JSON patch work budget exceeded");
                for (PJSONMAP::const_iterator it = object.begin(); it != object.end(); ++it) {
                    if (!chargePatch(budget.bytes, budget.byteLimit, it->first.size(), error,
                                     "JSON patch cloned-byte budget exceeded"))
                        return false;
                    work.push_back(it->second);
                }
            }
        }
        return true;
    }

    // Patch `test` needs bounded structural equality so an adversarial value
    // cannot hide unbounded traversal behind a single operation.
    bool patchEqual(const pjson& left, const pjson& right, PatchBudget& budget, PatchError& error,
                    bool& equal) {
        struct Pair {
            const pjson* left;
            const pjson* right;
        };
        std::vector<Pair> pending;
        Pair root = {&left, &right};
        pending.push_back(root);
        equal = false;
        while (!pending.empty()) {
            if (!chargePatch(budget.work, budget.workLimit, 1, error,
                             "JSON Patch work budget exceeded"))
                return false;
            const Pair current = pending.back();
            pending.pop_back();
            const pjson& lhs = *current.left;
            const pjson& rhs = *current.right;
            if (lhs.isNumber() && rhs.isNumber()) {
                if (pjsonImpl::_compareNumbers(lhs, rhs) != 0)
                    return true;
                continue;
            }
            if (lhs.getType() != rhs.getType())
                return true;
            if (lhs.isString()) {
                const std::string& l = pjsonImpl::_string(lhs);
                const std::string& r = pjsonImpl::_string(rhs);
                if (!chargePatch(budget.work, budget.workLimit, std::max(l.size(), r.size()), error,
                                 "JSON Patch work budget exceeded"))
                    return false;
                if (l != r)
                    return true;
            } else if (lhs.isBool()) {
                if (pjsonImpl::_boolean(lhs) != pjsonImpl::_boolean(rhs))
                    return true;
            } else if (lhs.isArray()) {
                const PJSONARRAY& l = pjsonImpl::_array(lhs);
                const PJSONARRAY& r = pjsonImpl::_array(rhs);
                if (l.size() != r.size())
                    return true;
                for (size_t i = 0; i < l.size(); ++i) {
                    Pair child = {l[i], r[i]};
                    pending.push_back(child);
                }
            } else if (lhs.isObject()) {
                const PJSONMAP& l = pjsonImpl::_object(lhs);
                const PJSONMAP& r = pjsonImpl::_object(rhs);
                if (l.size() != r.size())
                    return true;
                PJSONMAP::const_iterator li = l.begin();
                PJSONMAP::const_iterator ri = r.begin();
                for (; li != l.end(); ++li, ++ri) {
                    if (!chargePatch(budget.work, budget.workLimit,
                                     std::max(li->first.size(), ri->first.size()) + size_t(1),
                                     error, "JSON Patch work budget exceeded"))
                        return false;
                    if (li->first != ri->first)
                        return true;
                    Pair child = {li->second, ri->second};
                    pending.push_back(child);
                }
            }
        }
        equal = true;
        return true;
    }

    // Restores a reusable PatchError before processing a new patch document.
    void resetPatchError(PatchError& aError) {
        aError.ok = true;
        aError.code = PatchError::Ok;
        aError.opIndex = 0;
        aError.op.clear();
        aError.path.clear();
        aError.from.clear();
        aError.tokenIndex = 0;
        aError.token.clear();
        aError.message.clear();
    }

    // Records a patch failure while preserving operation metadata set by the caller.
    bool failPatch(PatchError& aError, PatchError::Code aCode, const char* aMessage) {
        aError.ok = false;
        aError.code = aCode;
        aError.message = aMessage;
        return false;
    }

    // Records a patch failure associated with one decoded pointer token.
    bool failPatchAtToken(PatchError& aError, PatchError::Code aCode, size_t aTokenIndex,
                          const std::string& aToken, const char* aMessage) {
        aError.tokenIndex = aTokenIndex;
        aError.token = aToken;
        return failPatch(aError, aCode, aMessage);
    }

    // Best-effort noexcept diagnostic used while translating allocation or
    // unexpected exceptions out of the public patch API.
    void failPatchException(PatchError& aError, PatchError::Code aCode,
                            const char* aMessage) noexcept {
        aError.ok = false;
        aError.code = aCode;
        try {
            aError.message = aMessage;
        } catch (...) {
            aError.message.clear();
        }
    }

    // Maps traversal categories into the smaller PatchError vocabulary.
    PatchError::Code pointerCodeForPatch(pjson::PointerError::Code aCode) {
        switch (aCode) {
            case pjson::PointerError::InvalidArrayIndex:
            case pjson::PointerError::AppendTokenNotAllowed:
                return PatchError::InvalidArrayIndex;
            case pjson::PointerError::ArrayIndexOutOfRange:
                return PatchError::ArrayIndexOutOfRange;
            case pjson::PointerError::MissingTarget:
            case pjson::PointerError::ExpectedContainer:
                return PatchError::TargetMissing;
            case pjson::PointerError::AllocationFailure:
                return PatchError::AllocationFailure;
            case pjson::PointerError::InternalError:
                return PatchError::InternalError;
            default:
                return PatchError::TargetMissing;
        }
    }

    // Copies token context from a pointer failure into the active operation error.
    bool failPatchFromPointer(PatchError& aError, const pjson::PointerError& aPointerError) {
        aError.tokenIndex = aPointerError.tokenIndex;
        aError.token = aPointerError.token;
        return failPatch(aError, pointerCodeForPatch(aPointerError.code),
                         aPointerError.message.c_str());
    }

    // Decodes a Patch pointer and classifies syntax failures as path or from errors.
    bool decodePatchPointer(const std::string& aPointer, bool bFrom,
                            std::vector<std::string>& aTokens, PatchBudget& aBudget,
                            PatchError& aError) {
        if (!chargePatch(aBudget.work, aBudget.workLimit, aPointer.size() + size_t(1), aError,
                         "JSON patch work budget exceeded"))
            return false;
        pjson::PointerError pointerError;
        if (decodePointer(aPointer, aTokens, pointerError))
            return true;
        aError.tokenIndex = pointerError.tokenIndex;
        aError.token = pointerError.token;
        return failPatch(aError, bFrom ? PatchError::InvalidFrom : PatchError::InvalidPath,
                         pointerError.message.c_str());
    }

    // Resolves a mutable prefix and translates PointerError into PatchError.
    pjson* resolvePatchTokens(pjson& aRoot, const std::vector<std::string>& aTokens, size_t aCount,
                              const std::string& aPointer, PatchBudget& aBudget,
                              PatchError& aError) {
        if (!chargePatch(aBudget.work, aBudget.workLimit, aCount + size_t(1), aError,
                         "JSON patch work budget exceeded"))
            return nullptr;
        pjson::PointerError pointerError;
        const pjson* result = resolvePointerTokens(aRoot, aTokens, aCount, aPointer, pointerError);
        if (result == nullptr) {
            failPatchFromPointer(aError, pointerError);
            return nullptr;
        }
        return const_cast<pjson*>(result);
    }

    // Validates a destination/source array index. '-' denotes exactly size() and
    // is accepted only for add, whose insertion range includes the end position.
    bool patchArrayIndex(const pjson& aParent, const std::string& aToken, bool bAllowAppend,
                         size_t aTokenIndex, size_t& aIndex, bool& bAppend, PatchError& aError) {
        bAppend = false;
        if (aToken == "-") {
            if (bAllowAppend) {
                bAppend = true;
                aIndex = aParent.size();
                return true;
            }
            return failPatchAtToken(aError, PatchError::InvalidArrayIndex, aTokenIndex, aToken,
                                    "the '-' token is valid only for add destinations");
        }

        const PointerIndexResult result = parsePointerIndex(aToken, aIndex);
        if (result == PointerIndexInvalid)
            return failPatchAtToken(aError, PatchError::InvalidArrayIndex, aTokenIndex, aToken,
                                    "array index is not canonical decimal");
        const size_t size = aParent.size();
        if (result == PointerIndexOverflow || (bAllowAppend ? aIndex > size : aIndex >= size))
            return failPatchAtToken(aError, PatchError::ArrayIndexOutOfRange, aTokenIndex, aToken,
                                    "array index is out of range");
        return true;
    }

    // Consumes an allocator-compatible value and implements Patch add. Existing
    // object members are replaced; array insertion shifts following elements.
    bool addOwnedAtPointer(pjson& aRoot, const std::vector<std::string>& aTokens,
                           const std::string& aPointer, pjsonImpl::OwnedNode aValue,
                           PatchBudget& aBudget, PatchError& aError) {
        if (aTokens.empty()) {
            pjsonImpl::_swapStorage(aRoot, *aValue);
            return true;
        }

        const size_t finalIndex = aTokens.size() - 1;
        pjson* parent = resolvePatchTokens(aRoot, aTokens, finalIndex, aPointer, aBudget, aError);
        if (parent == nullptr)
            return false;
        const std::string& token = aTokens.back();

        if (parent->isObject()) {
            PJSONMAP* object = &pjsonImpl::_object(*parent);
            PJSONMAP::iterator existing = object->find(token);
            if (existing != object->end()) {
                pjsonImpl::_swapStorage(*existing->second, *aValue);
                return true;
            }
            if (!chargePatch(aBudget.bytes, aBudget.byteLimit, token.size(), aError,
                             "JSON Patch cloned-byte budget exceeded"))
                return false;
            const std::pair<PJSONMAP::iterator, bool> inserted =
                object->insert(std::make_pair(token, static_cast<pjson*>(nullptr)));
            if (!inserted.second)
                return failPatchAtToken(aError, PatchError::InternalError, finalIndex, token,
                                        "failed to insert object member");
            inserted.first->second = aValue.release();
            return true;
        }

        if (parent->isArray()) {
            size_t index = 0;
            bool append = false;
            if (!patchArrayIndex(*parent, token, true, finalIndex, index, append, aError))
                return false;
            PJSONARRAY* array = &pjsonImpl::_array(*parent);
            if (!chargePatch(aBudget.work, aBudget.workLimit, array->size() - index, aError,
                             "JSON Patch work budget exceeded"))
                return false;
            const PJSONARRAY::iterator inserted =
                array->insert(array->begin() + static_cast<std::ptrdiff_t>(index), nullptr);
            *inserted = aValue.release();
            return true;
        }

        return failPatchAtToken(aError, PatchError::TargetMissing, finalIndex, token,
                                "add destination parent is not a container");
    }

    // Consumes a replacement only after proving the complete target exists.
    bool replaceAtPointer(pjson& aRoot, const std::vector<std::string>& aTokens,
                          const std::string& aPointer, pjsonImpl::OwnedNode aValue,
                          PatchBudget& aBudget, PatchError& aError) {
        if (aTokens.empty()) {
            pjsonImpl::_swapStorage(aRoot, *aValue);
            return true;
        }

        const size_t finalIndex = aTokens.size() - 1;
        pjson* parent = resolvePatchTokens(aRoot, aTokens, finalIndex, aPointer, aBudget, aError);
        if (parent == nullptr)
            return false;
        const std::string& token = aTokens.back();

        if (parent->isObject()) {
            PJSONMAP* object = &pjsonImpl::_object(*parent);
            PJSONMAP::iterator existing = object->find(token);
            if (existing == object->end())
                return failPatchAtToken(aError, PatchError::TargetMissing, finalIndex, token,
                                        "replace target does not exist");
            pjsonImpl::_swapStorage(*existing->second, *aValue);
            return true;
        }

        if (parent->isArray()) {
            size_t index = 0;
            bool append = false;
            if (!patchArrayIndex(*parent, token, false, finalIndex, index, append, aError))
                return false;
            pjsonImpl::_swapStorage(*pjsonImpl::_array(*parent)[index], *aValue);
            return true;
        }

        return failPatchAtToken(aError, PatchError::TargetMissing, finalIndex, token,
                                "replace target parent is not a container");
    }

    // Detaches a target without destroying it. Removing the document root is
    // represented by replacing the still-addressable root value with JSON null.
    bool detachAtPointer(pjson& aRoot, const std::vector<std::string>& aTokens,
                         const std::string& aPointer, pjsonImpl::OwnedNode& aValue,
                         PatchBudget& aBudget, PatchError& aError) {
        if (aTokens.empty()) {
            if (!chargePatch(aBudget.nodes, aBudget.nodeLimit, 1, aError,
                             "JSON Patch cloned-node budget exceeded") ||
                !chargePatch(aBudget.bytes, aBudget.byteLimit, sizeof(pjson), aError,
                             "JSON Patch cloned-byte budget exceeded"))
                return false;
            pjsonImpl::OwnedNode replacement = pjsonImpl::_makeNode(aRoot.getAllocator());
            pjsonImpl::_swapStorage(aRoot, *replacement);
            aValue = std::move(replacement);
            return true;
        }

        const size_t finalIndex = aTokens.size() - 1;
        pjson* parent = resolvePatchTokens(aRoot, aTokens, finalIndex, aPointer, aBudget, aError);
        if (parent == nullptr)
            return false;
        const std::string& token = aTokens.back();

        if (parent->isObject()) {
            PJSONMAP* object = &pjsonImpl::_object(*parent);
            PJSONMAP::iterator existing = object->find(token);
            if (existing == object->end())
                return failPatchAtToken(aError, PatchError::TargetMissing, finalIndex, token,
                                        "remove source does not exist");
            aValue.reset(existing->second);
            object->erase(existing);
            return true;
        }

        if (parent->isArray()) {
            size_t index = 0;
            bool append = false;
            if (!patchArrayIndex(*parent, token, false, finalIndex, index, append, aError))
                return false;
            PJSONARRAY* array = &pjsonImpl::_array(*parent);
            if (!chargePatch(aBudget.work, aBudget.workLimit, array->size() - index - size_t(1),
                             aError, "JSON Patch work budget exceeded"))
                return false;
            aValue.reset((*array)[index]);
            array->erase(array->begin() + static_cast<std::ptrdiff_t>(index));
            return true;
        }

        return failPatchAtToken(aError, PatchError::TargetMissing, finalIndex, token,
                                "remove source parent is not a container");
    }

    // Compares decoded paths so alternate escape spellings cannot affect identity.
    bool samePointerTokens(const std::vector<std::string>& aLeft,
                           const std::vector<std::string>& aRight) {
        return aLeft == aRight;
    }

    // Detects a move into the source's own descendant, which would invalidate
    // the destination during detachment and is forbidden by JSON Patch.
    bool isProperPointerAncestor(const std::vector<std::string>& aAncestor,
                                 const std::vector<std::string>& aDescendant) {
        return aAncestor.size() < aDescendant.size() &&
               std::equal(aAncestor.begin(), aAncestor.end(), aDescendant.begin());
    }

    // Replaces or adopts an allocator-compatible object child without exposing
    // a null map entry if insertion fails.
    bool insertObjectChild(pjson& aObject, const std::string& aKey, pjsonImpl::OwnedNode aChild) {
        PJSONMAP* object = &pjsonImpl::_object(aObject);
        PJSONMAP::iterator existing = object->find(aKey);
        if (existing != object->end()) {
            pjsonImpl::_swapStorage(*existing->second, *aChild);
            return true;
        }
        const std::pair<PJSONMAP::iterator, bool> inserted =
            object->insert(std::make_pair(aKey, static_cast<pjson*>(nullptr)));
        if (!inserted.second)
            return false;
        inserted.first->second = aChild.release();
        return true;
    }

    // Applies Merge Patch iteratively to a private working document. Object
    // patches recurse, null members delete, and every other value replaces via
    // an allocator-local clone. Atomic publication is handled by the caller.
    bool applyMergePatchTo(pjson& aTarget, const pjson& aPatch, PatchBudget& aBudget,
                           PatchError& aError) {
        struct MergeItem {
            // target and patch describe one pending object/object merge.
            pjson* target;
            const pjson* patch;
        };

        if (!aPatch.isObject()) {
            if (!chargePatch(aBudget.operations, aBudget.operationLimit, 1, aError,
                             "JSON Merge Patch operation budget exceeded"))
                return false;
            if (!measureClone(aPatch, aBudget, aError))
                return false;
            pjson replacement(aPatch, aTarget.getAllocator());
            aTarget.swap(replacement);
            return true;
        }

        std::vector<MergeItem> work;
        MergeItem root = {&aTarget, &aPatch};
        work.push_back(root);
        while (!work.empty()) {
            const MergeItem item = work.back();
            work.pop_back();
            if (!item.target->isObject())
                item.target->resetTo(pjson::jsonObject);

            const PJSONMAP* patchObject = &pjsonImpl::_object(*item.patch);
            for (PJSONMAP::const_iterator it = patchObject->begin(); it != patchObject->end();
                 ++it) {
                if (!chargePatch(aBudget.operations, aBudget.operationLimit, 1, aError,
                                 "JSON Merge Patch operation budget exceeded") ||
                    !chargePatch(aBudget.work, aBudget.workLimit, 1, aError,
                                 "JSON Merge Patch work budget exceeded"))
                    return false;
                const std::string& key = it->first;
                const pjson& patchValue = *it->second;
                if (!chargePatch(aBudget.bytes, aBudget.byteLimit, key.size(), aError,
                                 "JSON Merge Patch cloned-byte budget exceeded"))
                    return false;
                if (patchValue.isNull()) {
                    item.target->erase(key);
                    continue;
                }

                pjson* targetValue = item.target->find(key);
                if (patchValue.isObject()) {
                    if (targetValue == nullptr) {
                        if (!chargePatch(aBudget.nodes, aBudget.nodeLimit, 1, aError,
                                         "JSON Merge Patch cloned-node budget exceeded") ||
                            !chargePatch(aBudget.bytes, aBudget.byteLimit, sizeof(pjson), aError,
                                         "JSON Merge Patch cloned-byte budget exceeded"))
                            return false;
                        pjsonImpl::OwnedNode child =
                            pjsonImpl::_makeNode(item.target->getAllocator());
                        child->resetTo(pjson::jsonObject);
                        targetValue = child.get();
                        if (!insertObjectChild(*item.target, key, std::move(child)))
                            return false;
                    } else if (!targetValue->isObject()) {
                        targetValue->resetTo(pjson::jsonObject);
                    }
                    MergeItem childItem = {targetValue, &patchValue};
                    work.push_back(childItem);
                    continue;
                }

                if (!measureClone(patchValue, aBudget, aError))
                    return false;
                pjsonImpl::OwnedNode replacement =
                    pjsonImpl::_cloneNode(patchValue, item.target->getAllocator());
                if (!insertObjectChild(*item.target, key, std::move(replacement)))
                    return false;
            }
        }
        return true;
    }
} // namespace

// Key/index extraction overloads combine non-mutating lookup with exact
// tryGet conversion and leave output parameters unchanged on any miss. The
// std::string forms are length-aware so embedded-NUL keys resolve correctly.
bool pjson::tryGet(const std::string& aKey, int64_t& aResult) const {
    const pjson* value = find(aKey);
    return value != nullptr && value->tryGet(aResult);
}
bool pjson::tryGet(const std::string& aKey, uint64_t& aResult) const {
    const pjson* value = find(aKey);
    return value != nullptr && value->tryGet(aResult);
}
bool pjson::tryGet(const std::string& aKey, double& aResult) const {
    const pjson* value = find(aKey);
    return value != nullptr && value->tryGet(aResult);
}
bool pjson::tryGet(const std::string& aKey, bool& aResult) const {
    const pjson* value = find(aKey);
    return value != nullptr && value->tryGet(aResult);
}
bool pjson::tryGet(const std::string& aKey, std::string& aResult) const {
    const pjson* value = find(aKey);
    return value != nullptr && value->tryGet(aResult);
}
bool pjson::tryGet(const std::string& aKey, StringView& aResult) const {
    const pjson* value = find(aKey);
    return value != nullptr && value->tryGet(aResult);
}
bool pjson::tryGet(const char* aKey, int64_t& aResult) const {
    const pjson* value = find(aKey);
    return value != nullptr && value->tryGet(aResult);
}
bool pjson::tryGet(const char* aKey, uint64_t& aResult) const {
    const pjson* value = find(aKey);
    return value != nullptr && value->tryGet(aResult);
}
bool pjson::tryGet(const char* aKey, double& aResult) const {
    const pjson* value = find(aKey);
    return value != nullptr && value->tryGet(aResult);
}
bool pjson::tryGet(const char* aKey, bool& aResult) const {
    const pjson* value = find(aKey);
    return value != nullptr && value->tryGet(aResult);
}
bool pjson::tryGet(const char* aKey, std::string& aResult) const {
    const pjson* value = find(aKey);
    return value != nullptr && value->tryGet(aResult);
}
bool pjson::tryGet(const char* aKey, StringView& aResult) const {
    const pjson* value = find(aKey);
    return value != nullptr && value->tryGet(aResult);
}
bool pjson::tryGet(int aIndex, int64_t& aResult) const noexcept {
    const pjson* value = find(aIndex);
    return value != nullptr && value->tryGet(aResult);
}
bool pjson::tryGet(int aIndex, uint64_t& aResult) const noexcept {
    const pjson* value = find(aIndex);
    return value != nullptr && value->tryGet(aResult);
}
bool pjson::tryGet(int aIndex, double& aResult) const noexcept {
    const pjson* value = find(aIndex);
    return value != nullptr && value->tryGet(aResult);
}
bool pjson::tryGet(int aIndex, bool& aResult) const noexcept {
    const pjson* value = find(aIndex);
    return value != nullptr && value->tryGet(aResult);
}
bool pjson::tryGet(int aIndex, std::string& aResult) const {
    const pjson* value = find(aIndex);
    return value != nullptr && value->tryGet(aResult);
}
bool pjson::tryGet(int aIndex, StringView& aResult) const noexcept {
    const pjson* value = find(aIndex);
    return value != nullptr && value->tryGet(aResult);
}

//===----------------------------------------------------------------------===//
// Public DOM and SAX parse API families
//
// Overloads differ only in input source and diagnostics. Every DOM parse returns
// the origin-aware pjsonImpl::OwnedNode, including roots from the default allocator.
//===----------------------------------------------------------------------===//

/*static*/
// Parses string-owned bytes with default allocation and omitted diagnostics.
pjson pjson::parse(const std::string& aStr, const ParseOptions& aOpts) {
    return pjsonImpl::_parseTop(aStr.c_str(), aStr.length(), aOpts, nullptr,
                                pjsonImpl::_defaultAllocator());
}
/*static*/
// Parses an explicit byte span with default allocation and omitted diagnostics.
pjson pjson::parse(const char* aSrc, size_t aSize, const ParseOptions& aOpts) {
    return pjsonImpl::_parseTop(aSrc, aSize, aOpts, nullptr, pjsonImpl::_defaultAllocator());
}
/*static*/
// Emits SAX events from string-owned bytes and discards detailed diagnostics.
bool pjson::parseSax(const std::string& aStr, SaxHandler& aHandler, const ParseOptions& aOpts) {
    return pjsonImpl::_parseSaxTop(aStr.c_str(), aStr.length(), aHandler, aOpts, nullptr);
}
/*static*/
// Emits SAX events from an explicit byte span and discards detailed diagnostics.
bool pjson::parseSax(const char* aSrc, size_t aSize, SaxHandler& aHandler,
                     const ParseOptions& aOpts) {
    return pjsonImpl::_parseSaxTop(aSrc, aSize, aHandler, aOpts, nullptr);
}
/*static*/
// Parses string-owned bytes and fills a caller-visible ParseError.
pjson pjson::parse(const std::string& aStr, ParseError& aError, const ParseOptions& aOpts) {
    return pjsonImpl::_parseTop(aStr.c_str(), aStr.length(), aOpts, &aError,
                                pjsonImpl::_defaultAllocator());
}
/*static*/
// Emits SAX events from a string and fills a caller-visible ParseError.
bool pjson::parseSax(const std::string& aStr, SaxHandler& aHandler, ParseError& aError,
                     const ParseOptions& aOpts) {
    return pjsonImpl::_parseSaxTop(aStr.c_str(), aStr.length(), aHandler, aOpts, &aError);
}
/*static*/
// Parses an explicit byte span and fills a caller-visible ParseError.
pjson pjson::parse(const char* aSrc, size_t aSize, ParseError& aError, const ParseOptions& aOpts) {
    return pjsonImpl::_parseTop(aSrc, aSize, aOpts, &aError, pjsonImpl::_defaultAllocator());
}
/*static*/
// Emits SAX events from a byte span and fills a caller-visible ParseError.
bool pjson::parseSax(const char* aSrc, size_t aSize, SaxHandler& aHandler, ParseError& aError,
                     const ParseOptions& aOpts) {
    return pjsonImpl::_parseSaxTop(aSrc, aSize, aHandler, aOpts, &aError);
}
/*static*/
// Parses a stream with default allocation and omitted diagnostics.
pjson pjson::parseStream(std::istream& aIn, const ParseOptions& aOpts) {
    return pjsonImpl::_parseStream(aIn, aOpts, nullptr, pjsonImpl::_defaultAllocator());
}
/*static*/
// Parses a stream with default allocation and caller-visible diagnostics.
pjson pjson::parseStream(std::istream& aIn, ParseError& aError, const ParseOptions& aOpts) {
    return pjsonImpl::_parseStream(aIn, aOpts, &aError, pjsonImpl::_defaultAllocator());
}
/*static*/
// Parses string-owned bytes with nodes and wrapper objects from aAlloc.
pjson pjson::parse(const std::string& aStr, Allocator& aAlloc, const ParseOptions& aOpts) {
    return pjsonImpl::_parseTop(aStr.c_str(), aStr.length(), aOpts, nullptr, aAlloc);
}
/*static*/
// Parses a byte span with nodes and wrapper objects from aAlloc.
pjson pjson::parse(const char* aSrc, size_t aSize, Allocator& aAlloc, const ParseOptions& aOpts) {
    return pjsonImpl::_parseTop(aSrc, aSize, aOpts, nullptr, aAlloc);
}
/*static*/
// Parses string-owned bytes with custom allocation and detailed diagnostics.
pjson pjson::parse(const std::string& aStr, ParseError& aError, Allocator& aAlloc,
                   const ParseOptions& aOpts) {
    return pjsonImpl::_parseTop(aStr.c_str(), aStr.length(), aOpts, &aError, aAlloc);
}
/*static*/
// Parses a byte span with custom allocation and detailed diagnostics.
pjson pjson::parse(const char* aSrc, size_t aSize, ParseError& aError, Allocator& aAlloc,
                   const ParseOptions& aOpts) {
    return pjsonImpl::_parseTop(aSrc, aSize, aOpts, &aError, aAlloc);
}
/*static*/
// Parses a stream with nodes and wrapper objects from aAlloc.
pjson pjson::parseStream(std::istream& aIn, Allocator& aAlloc, const ParseOptions& aOpts) {
    return pjsonImpl::_parseStream(aIn, aOpts, nullptr, aAlloc);
}
/*static*/
// Parses a stream with custom allocation and detailed diagnostics.
pjson pjson::parseStream(std::istream& aIn, ParseError& aError, Allocator& aAlloc,
                         const ParseOptions& aOpts) {
    return pjsonImpl::_parseStream(aIn, aOpts, &aError, aAlloc);
}
/*static*/
// Emits SAX events directly from a stream and discards detailed diagnostics.
bool pjson::parseSaxStream(std::istream& aIn, SaxHandler& aHandler, const ParseOptions& aOpts) {
    return pjsonImpl::_parseSaxStream(aIn, aHandler, aOpts, nullptr);
}
/*static*/
// Emits SAX events directly from a stream with caller-visible diagnostics.
bool pjson::parseSaxStream(std::istream& aIn, SaxHandler& aHandler, ParseError& aError,
                           const ParseOptions& aOpts) {
    return pjsonImpl::_parseSaxStream(aIn, aHandler, aOpts, &aError);
}
// Reads incrementally so maxInputBytes bounds memory before the complete stream
// has been materialized. Returns the parsed document by value (null on failure).
/*static*/
pjson pjsonImpl::_parseStream(std::istream& aIn, const ParseOptions& aOpts, ParseError* aErr,
                              pjson::Allocator& aAlloc) {
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
                          "input exceeds maxInputBytes", ParseError::InputLimit);
            return pjson(aAlloc);
        }
        content.append(buffer, chunk);
    }
    if (aIn.bad()) {
        setParseError(aErr, content.data(), content.size(), content.size(), "stream read failed",
                      ParseError::StreamError);
        return pjson(aAlloc);
    }
    return _parseTop(content.c_str(), content.length(), aOpts, aErr, aAlloc);
}
/*static*/
bool pjsonImpl::_parseSaxTop(const char* aSrc, size_t aSize, SaxHandler& aHandler,
                             const ParseOptions& aOpts, ParseError* aErr) {
    resetParseError(aErr);
    if (aSrc == nullptr) {
        setParseError(aErr, "", 0, 0, "null input");
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
bool pjsonImpl::_parseSaxStream(std::istream& aIn, SaxHandler& aHandler, const ParseOptions& aOpts,
                                ParseError* aErr) {
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
// whitespace, and report success/failure through the optional ParseError.
/*static*/
pjson pjsonImpl::_parseTop(const char* aSrc, size_t aSize, const ParseOptions& aOpts,
                           ParseError* aErr, pjson::Allocator& aAlloc) {
    resetParseError(aErr);
    if (aSrc == nullptr) {
        setParseError(aErr, "", 0, 0, "null input", ParseError::InvalidArgument);
        return pjson(aAlloc);
    }

    // Reject an over-large input up front (cheap DoS guard before any work).
    if (aOpts.maxInputBytes != 0 && aSize > aOpts.maxInputBytes) {
        setParseError(aErr, aSrc, aSize, aOpts.maxInputBytes, "input exceeds maxInputBytes",
                      ParseError::InputLimit);
        return pjson(aAlloc);
    }

    ParseCtx c;
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
        if (!_parseValue(c, parsed)) {
            pjsonImpl::_destroyNode(parsed);
            setParseError(aErr, aSrc, aSize, c.errPos, c.errMsg.empty() ? "parse error" : c.errMsg);
            return pjson(aAlloc);
        }
        // Own the parsed node so it is freed even if the trailing check throws.
        OwnedNode owned(parsed);

        // A valid document is a single value; only trailing whitespace may follow.
        char trailing;
        if (_peek(c, trailing)) {
            setParseError(aErr, aSrc, aSize, c.pos, "trailing characters after JSON value",
                          ParseError::Syntax);
            return pjson(aAlloc);
        }
        // Move the parsed node's storage into a value bound to the same allocator.
        // O(1): the value adopts the node's inline storage; the node wrapper is
        // then freed empty by OwnedNode, so no smart pointer escapes to the caller.
        pjson result(aAlloc);
        _swapStorage(result, *parsed);
        return result;
    } catch (const std::bad_alloc&) {
        setParseError(aErr, aSrc, aSize, c.pos, "parse ran out of memory",
                      ParseError::AllocationFailure);
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
bool pjsonImpl::_peek(ParseCtx& c, char& aOut) {
    while (c.pos < c.end) {
        aOut = c.src[c.pos];
        if (_isWhitespace(aOut)) {
            ++c.pos;
        } else {
            return true;
        }
    }
    return false;
}
// Consumes the ':' separating an object key from its value (skipping ws).
/*static*/
bool pjsonImpl::_skipColon(ParseCtx& c) {
    while (c.pos < c.end) {
        char ch = c.src[c.pos++];
        if (ch == ':') {
            return true;
        } else if (_isWhitespace(ch)) {
            // ignore
        } else {
            return _fail(c, c.pos - 1, "expected ':' after object key");
        }
    }
    return _fail(c, c.pos, "expected ':' after object key");
}
// Dispatches on the next non-whitespace character to the right sub-parser.
/*static*/
bool pjsonImpl::_parseValue(ParseCtx& c, pjson*& aOut) {
    char ch;
    if (!_peek(c, ch)) {
        return _fail(c, c.pos, "unexpected end of input; expected a value");
    }
    if (ch == '\"') {
        return _parseString(c, aOut);
    } else if (ch == '{') {
        return _parseObject(c, aOut);
    } else if (ch == '[') {
        return _parseArray(c, aOut);
    } else if (ch == '-' || (ch >= '0' && ch <= '9')) {
        return _parseNumber(c, aOut);
    } else {
        // RFC 8259 null / true / false literals.
        return _parseKeyword(c, aOut);
    }
}
// Matches a keyword literal using the exact lowercase RFC spelling.
/*static*/
bool pjsonImpl::_parseKeyword(ParseCtx& c, pjson*& aOut) {
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
            pjsonImpl::OwnedNode value(_newNode(c));
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
    return _fail(c, c.pos, "invalid JSON value");
}
// Reads a quoted string body starting at the opening '"'.
/*static*/
bool pjsonImpl::_extractString(ParseCtx& c, std::string& aOut) {
    if (c.pos >= c.end || c.src[c.pos] != '\"') {
        return _fail(c, c.pos, "expected '\"' to start a string");
    }
    ++c.pos; // consume opening quote
    return pjsonImpl::_decodeStringBody(c, aOut, /*bStopAtQuote=*/true);
}
/*static*/
// Parses and allocates one string value after decoding its complete token.
bool pjsonImpl::_parseString(ParseCtx& c, pjson*& aOut) {
    std::string s;
    if (!_extractString(c, s)) {
        return false;
    }
    pjsonImpl::OwnedNode value(_newNode(c));
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
bool pjsonImpl::_parseNumber(ParseCtx& c, pjson*& aOut) {
    const size_t begin = c.pos;
    size_t i = c.pos;
    bool bFloat = false;
    const bool negative = (i < c.end && c.src[i] == '-');

    if (i < c.end && c.src[i] == '-')
        ++i;

    // integer part: 0 or [1-9][0-9]*
    if (i < c.end && c.src[i] == '0') {
        ++i;
    } else if (i < c.end && c.src[i] >= '1' && c.src[i] <= '9') {
        while (i < c.end && c.src[i] >= '0' && c.src[i] <= '9')
            ++i;
    } else {
        return _fail(c, i, "invalid number: expected digit");
    }

    // fractional part
    if (i < c.end && c.src[i] == '.') {
        bFloat = true;
        ++i;
        if (!(i < c.end && c.src[i] >= '0' && c.src[i] <= '9')) {
            return _fail(c, i, "invalid number: '.' must be followed by a digit");
        }
        while (i < c.end && c.src[i] >= '0' && c.src[i] <= '9')
            ++i;
    }

    // exponent part
    if (i < c.end && (c.src[i] == 'e' || c.src[i] == 'E')) {
        bFloat = true;
        ++i;
        if (i < c.end && (c.src[i] == '+' || c.src[i] == '-'))
            ++i;
        if (!(i < c.end && c.src[i] >= '0' && c.src[i] <= '9')) {
            return _fail(c, i, "invalid number: exponent must have a digit");
        }
        while (i < c.end && c.src[i] >= '0' && c.src[i] <= '9')
            ++i;
    }

    std::string sTemp(c.src + begin, i - begin);
    const bool allowLossy = c.numberPolicy == pjson::ParseOptions::AllowLossyNumbers;
    if (bFloat) {
        double d = 0.0;
        if (!_parseDouble(sTemp, d) || !std::isfinite(d)) {
            return _fail(c, begin, "number out of range");
        }
        pjsonImpl::OwnedNode value(_newNode(c));
        if (!value)
            return false;
        *value = d;
        aOut = value.release();
        c.pos = i;
        return true;
    }

    // Integer token. Try signed first, then unsigned for positive values above
    // INT64_MAX, so the full 64-bit range is represented exactly.
    errno = 0;
    long long llVal = strtoll(sTemp.c_str(), nullptr, 10);
    if (errno != ERANGE) {
        pjsonImpl::OwnedNode value(_newNode(c));
        if (!value)
            return false;
        *value = static_cast<int64_t>(llVal);
        aOut = value.release();
        c.pos = i;
        return true;
    }

    if (!negative) {
        errno = 0;
        unsigned long long ullVal = strtoull(sTemp.c_str(), nullptr, 10);
        if (errno != ERANGE) {
            pjsonImpl::OwnedNode value(_newNode(c));
            if (!value)
                return false;
            *value = static_cast<uint64_t>(ullVal);
            aOut = value.release();
            c.pos = i;
            return true;
        }
    }

    // Beyond the exact 64-bit integer range. Reject by default, or fall back to
    // a lossy double when the caller opts in.
    if (!allowLossy) {
        return _fail(c, begin, "integer out of range; enable AllowLossyNumbers to store as double");
    }
    double d = 0.0;
    if (!_parseDouble(sTemp, d) || !std::isfinite(d)) {
        return _fail(c, begin, "number out of range");
    }
    pjsonImpl::OwnedNode value(_newNode(c));
    if (!value)
        return false;
    *value = d;
    aOut = value.release();
    c.pos = i;
    return true;
}
// Parses one array under a balanced depth charge. A child remains RAII-owned
// until vector growth succeeds, preventing leaks on allocation failure.
/*static*/
bool pjsonImpl::_parseArray(ParseCtx& c, pjson*& aOut) {
    if (++c.depth > c.maxDepth) {
        --c.depth;
        return _fail(c, c.pos, "maximum nesting depth exceeded");
    }
    pjsonImpl::OwnedNode arr(_newNode(c));
    if (!arr) {
        --c.depth;
        return false;
    }
    arr->resetTo(jsonType::jsonArray);
    ++c.pos; // consume '['

    bool bExpectValue = false; // a comma was seen, a value must follow
    bool bAny = false;         // at least one value parsed
    char ch;
    while (_peek(c, ch)) {
        if (ch == ']') {
            if (bExpectValue) {
                --c.depth;
                return _fail(c, c.pos, "trailing comma in array");
            }
            ++c.pos;
            --c.depth;
            aOut = arr.release();
            return true;
        } else if (ch == ',') {
            if (!bAny || bExpectValue) {
                --c.depth;
                return _fail(c, c.pos, "unexpected ',' in array");
            }
            ++c.pos;
            bExpectValue = true;
        } else {
            if (bAny && !bExpectValue) {
                --c.depth;
                return _fail(c, c.pos, "missing ',' between array elements");
            }
            pjson* elem = nullptr;
            if (!_parseValue(c, elem)) {
                pjsonImpl::_destroyNode(elem);
                --c.depth;
                return false;
            }
            pjsonImpl::OwnedNode ownedElem(elem);
            arr->_uValue._pValueArray->push_back(nullptr);
            arr->_uValue._pValueArray->back() = ownedElem.release();
            bAny = true;
            bExpectValue = false;
        }
    }
    --c.depth;
    return _fail(c, c.pos, "unterminated array");
}
// Parses one object under a balanced depth charge and applies duplicate policy
// only after the replacement value is fully parsed and owned.
/*static*/
bool pjsonImpl::_parseObject(ParseCtx& c, pjson*& aOut) {
    if (++c.depth > c.maxDepth) {
        --c.depth;
        return _fail(c, c.pos, "maximum nesting depth exceeded");
    }
    pjsonImpl::OwnedNode obj(_newNode(c));
    if (!obj) {
        --c.depth;
        return false;
    }
    obj->resetTo(jsonType::jsonObject);
    ++c.pos; // consume '{'

    bool bExpectMember = false; // a comma was seen, a member must follow
    bool bAny = false;          // at least one member parsed
    char ch;
    while (_peek(c, ch)) {
        if (ch == '}') {
            if (bExpectMember) {
                --c.depth;
                return _fail(c, c.pos, "trailing comma in object");
            }
            ++c.pos;
            --c.depth;
            aOut = obj.release();
            return true;
        } else if (ch == ',') {
            if (!bAny || bExpectMember) {
                --c.depth;
                return _fail(c, c.pos, "unexpected ',' in object");
            }
            ++c.pos;
            bExpectMember = true;
        } else if (ch == '\"') {
            if (bAny && !bExpectMember) {
                --c.depth;
                return _fail(c, c.pos, "missing ',' between object members");
            }
            const size_t keyOffset = c.pos;
            std::string mkey;
            if (!_extractString(c, mkey)) {
                --c.depth;
                return false;
            }
            // PJSON-PARSE-002: under the reject policy, report the duplicate
            // immediately after the second name is decoded, before parsing (and
            // allocating) its value subtree.
            const bool duplicate =
                obj->_uValue._pValueMap->find(mkey) != obj->_uValue._pValueMap->end();
            if (duplicate && c.duplicateKeys == ParseOptions::RejectDuplicateKeys) {
                --c.depth;
                return _fail(c, keyOffset, "duplicate object key");
            }
            pjson* val = nullptr;
            if (!_skipColon(c) || !_parseValue(c, val)) {
                pjsonImpl::_destroyNode(val);
                --c.depth;
                return false;
            }
            // Apply the remaining duplicate-key policy: keep the first or last
            // value deterministically (reject was already handled above).
            if (duplicate) {
                auto it = obj->_uValue._pValueMap->find(mkey);
                if (c.duplicateKeys == ParseOptions::KeepLastDuplicate) {
                    pjsonImpl::_destroyNode(it->second);
                    it->second = val;
                } else {
                    pjsonImpl::_destroyNode(val); // KeepFirstDuplicate
                }
            } else {
                pjsonImpl::OwnedNode ownedVal(val);
                pjson*& slot = (*(obj->_uValue._pValueMap))[mkey];
                slot = ownedVal.release();
            }
            bAny = true;
            bExpectMember = false;
        } else {
            --c.depth;
            return _fail(c, c.pos, "expected '\"' to start an object key");
        }
    }
    --c.depth;
    return _fail(c, c.pos, "unterminated object");
}

//===----------------------------------------------------------------------===//
// Container queries and mutation
//===----------------------------------------------------------------------===//

bool pjson::hasKey(const std::string& aKey) const {
    if (_eType == jsonType::jsonObject) {
        auto it = _uValue._pValueMap->find(aKey);
        return (it != _uValue._pValueMap->end());
    }
    return false;
}
// Reports whether an object contains a non-null C-string key.
bool pjson::hasKey(const char* cStr) const {
    if (cStr != nullptr)
        return hasKey(std::string(cStr));
    return false;
}
// Reports whether an array index resolves under find()'s negative-index rules.
bool pjson::hasIndex(int aIndex) const noexcept {
    return find(aIndex) != nullptr;
}
// Returns the member/element count for containers and zero for scalars.
size_t pjson::size() const {
    if (_eType == jsonType::jsonArray) {
        return _uValue._pValueArray->size();
    }
    if (_eType == jsonType::jsonObject) {
        return _uValue._pValueMap->size();
    }
    return 0;
}
// Reports whether size() is zero, so every scalar is considered empty.
bool pjson::empty() const {
    return size() == 0;
}
// Clears container children without changing container type; scalars become null.
void pjson::clear() {
    // Arrays and maps become empty containers of the same type; anything else
    // resets to null.
    switch (_eType) {
        case jsonType::jsonArray: {
            pjsonImpl::_disposeChildren(*this);
            _uValue._pValueArray->clear();
            break;
        }
        case jsonType::jsonObject: {
            pjsonImpl::_disposeChildren(*this);
            _uValue._pValueMap->clear();
            break;
        }
        default:
            reset();
            break;
    }
}
// Returns object keys in the map's deterministic sorted iteration order.
std::vector<std::string> pjson::keys() const {
    std::vector<std::string> result;
    if (_eType == jsonType::jsonObject) {
        result.reserve(_uValue._pValueMap->size());
        for (const auto& kv : *_uValue._pValueMap) {
            result.push_back(kv.first);
        }
    }
    return result;
}
bool pjson::erase(const std::string& aKey) {
    if (_eType == jsonType::jsonObject) {
        auto it = _uValue._pValueMap->find(aKey);
        if (it != _uValue._pValueMap->end()) {
            pjsonImpl::_destroyNode(it->second);
            _uValue._pValueMap->erase(it);
            return true;
        }
    }
    return false;
}
// Removes an object member and destroys its owned subtree.
bool pjson::erase(const char* aKey) {
    if (aKey != nullptr)
        return erase(std::string(aKey));
    return false;
}
// Removes an array element and destroys its owned subtree, shifting later indices.
bool pjson::erase(size_t aIndex) {
    if (_eType == jsonType::jsonArray && aIndex < _uValue._pValueArray->size()) {
        pjsonImpl::_destroyNode((*_uValue._pValueArray)[aIndex]);
        _uValue._pValueArray->erase(_uValue._pValueArray->begin() +
                                    static_cast<std::ptrdiff_t>(aIndex));
        return true;
    }
    return false;
}
// Applies JSON Patch while intentionally discarding detailed diagnostics.
bool pjson::applyPatch(const pjson& aPatch, const PatchOptions& aOpts) noexcept {
    PatchError error;
    return applyPatch(aPatch, error, aOpts);
}
// Applies an RFC 6902 operation sequence atomically. Validation and mutation
// happen on scratch; only a completely successful sequence is published by swap.
bool pjson::applyPatch(const pjson& aPatch, PatchError& aError,
                       const PatchOptions& aOpts) noexcept {
    resetPatchError(aError);
    try {
        if (!aPatch.isArray())
            return failPatch(aError, PatchError::InvalidPatchDocument,
                             "JSON Patch document must be an array");

        PatchBudget budget(aOpts);
        const PJSONARRAY& operations = pjsonImpl::_array(aPatch);
        if (!chargePatch(budget.operations, budget.operationLimit, operations.size(), aError,
                         "JSON Patch operation budget exceeded") ||
            !measureClone(*this, budget, aError))
            return false;

        // The scratch copy is both the rollback boundary and the allocator domain
        // into which every add/copy/replace value must be cloned.
        pjson scratch(*this, *_allocator);
        for (size_t operationIndex = 0; operationIndex < operations.size(); ++operationIndex) {
            const pjson& operation = *operations[operationIndex];
            aError.opIndex = operationIndex;
            aError.op.clear();
            aError.path.clear();
            aError.from.clear();
            aError.tokenIndex = 0;
            aError.token.clear();
            aError.message.clear();

            if (!operation.isObject())
                return failPatch(aError, PatchError::OperationNotObject,
                                 "JSON Patch operation must be an object");

            const pjson* opNode = operation.find("op");
            if (opNode == nullptr || !opNode->isString())
                return failPatch(aError, PatchError::MissingOp,
                                 "JSON Patch operation requires string member 'op'");
            aError.op = pjsonImpl::_string(*opNode);

            const bool knownOperation = aError.op == "add" || aError.op == "remove" ||
                                        aError.op == "replace" || aError.op == "move" ||
                                        aError.op == "copy" || aError.op == "test";
            if (!knownOperation)
                return failPatch(aError, PatchError::InvalidOp,
                                 "JSON Patch operation name is not supported");

            const pjson* pathNode = operation.find("path");
            if (pathNode == nullptr || !pathNode->isString())
                return failPatch(aError, PatchError::MissingPath,
                                 "JSON Patch operation requires string member 'path'");
            aError.path = pjsonImpl::_string(*pathNode);
            std::vector<std::string> pathTokens;
            if (!decodePatchPointer(aError.path, false, pathTokens, budget, aError))
                return false;

            // Validate and decode this operation's metadata before mutating the
            // private scratch tree. Later operations are processed only after
            // earlier ones succeed; the public target is still untouched.
            const bool needsFrom = aError.op == "move" || aError.op == "copy";
            std::vector<std::string> fromTokens;
            if (needsFrom) {
                const pjson* fromNode = operation.find("from");
                if (fromNode == nullptr || !fromNode->isString())
                    return failPatch(aError, PatchError::MissingFrom,
                                     "move and copy require string member 'from'");
                aError.from = pjsonImpl::_string(*fromNode);
                if (!decodePatchPointer(aError.from, true, fromTokens, budget, aError))
                    return false;
            }

            const bool needsValue =
                aError.op == "add" || aError.op == "replace" || aError.op == "test";
            const pjson* valueNode = operation.find("value");
            if (needsValue && valueNode == nullptr)
                return failPatch(aError, PatchError::MissingValue,
                                 "add, replace, and test require member 'value'");

            if (aError.op == "add") {
                if (!measureClone(*valueNode, budget, aError))
                    return false;
                pjsonImpl::OwnedNode value =
                    pjsonImpl::_cloneNode(*valueNode, scratch.getAllocator());
                if (!addOwnedAtPointer(scratch, pathTokens, aError.path, std::move(value), budget,
                                       aError))
                    return false;
                continue;
            }

            if (aError.op == "remove") {
                pjsonImpl::OwnedNode removed;
                if (!detachAtPointer(scratch, pathTokens, aError.path, removed, budget, aError))
                    return false;
                continue;
            }

            if (aError.op == "replace") {
                if (!measureClone(*valueNode, budget, aError))
                    return false;
                pjsonImpl::OwnedNode value =
                    pjsonImpl::_cloneNode(*valueNode, scratch.getAllocator());
                if (!replaceAtPointer(scratch, pathTokens, aError.path, std::move(value), budget,
                                      aError))
                    return false;
                continue;
            }

            if (aError.op == "test") {
                const pjson* target = resolvePatchTokens(scratch, pathTokens, pathTokens.size(),
                                                         aError.path, budget, aError);
                if (target == nullptr)
                    return false;
                bool equal = false;
                if (!patchEqual(*target, *valueNode, budget, aError, equal))
                    return false;
                if (!equal)
                    return failPatch(aError, PatchError::TestFailed,
                                     "JSON Patch test value does not match target");
                continue;
            }

            const pjson* source = resolvePatchTokens(scratch, fromTokens, fromTokens.size(),
                                                     aError.from, budget, aError);
            if (source == nullptr)
                return false;

            if (aError.op == "copy") {
                if (!measureClone(*source, budget, aError))
                    return false;
                pjsonImpl::OwnedNode value = pjsonImpl::_cloneNode(*source, scratch.getAllocator());
                if (!addOwnedAtPointer(scratch, pathTokens, aError.path, std::move(value), budget,
                                       aError))
                    return false;
                continue;
            }

            if (samePointerTokens(fromTokens, pathTokens))
                continue;
            if (fromTokens.empty()) {
                // Moving the root can only succeed when the destination is
                // also root (handled above); every other path is a descendant.
                return failPatch(aError, PatchError::MoveRootNotAllowed,
                                 "cannot move the document root below itself");
            }
            if (isProperPointerAncestor(fromTokens, pathTokens))
                return failPatch(aError, PatchError::MoveIntoDescendant,
                                 "cannot move a value into one of its descendants");

            pjsonImpl::OwnedNode moved;
            if (!detachAtPointer(scratch, fromTokens, aError.from, moved, budget, aError))
                return false;
            if (!addOwnedAtPointer(scratch, pathTokens, aError.path, std::move(moved), budget,
                                   aError))
                return false;
        }

        // This is the sole publication point; all earlier exits leave *this intact.
        swap(scratch);
        resetPatchError(aError);
        return true;
    } catch (const std::bad_alloc&) {
        failPatchException(aError, PatchError::AllocationFailure, "JSON Patch ran out of memory");
        return false;
    } catch (const std::exception&) {
        failPatchException(aError, PatchError::InternalError,
                           "JSON Patch failed with an internal exception");
        return false;
    } catch (...) {
        failPatchException(aError, PatchError::InternalError,
                           "JSON Patch failed with an unknown exception");
        return false;
    }
}
// Applies Merge Patch while intentionally discarding detailed diagnostics.
bool pjson::applyMergePatch(const pjson& aPatch, const PatchOptions& aOpts) noexcept {
    PatchError error;
    return applyMergePatch(aPatch, error, aOpts);
}
// Applies RFC 7396 atomically by mutating a private deep copy and publishing it
// only after the iterative merge has completed.
bool pjson::applyMergePatch(const pjson& aPatch, PatchError& aError,
                            const PatchOptions& aOpts) noexcept {
    resetPatchError(aError);
    try {
        PatchBudget budget(aOpts);
        if (!measureClone(*this, budget, aError))
            return false;
        pjson scratch(*this, *_allocator);
        if (!applyMergePatchTo(scratch, aPatch, budget, aError)) {
            if (!aError.ok)
                return false;
            return failPatch(aError, PatchError::InternalError,
                             "JSON Merge Patch could not update an object member");
        }
        swap(scratch);
        resetPatchError(aError);
        return true;
    } catch (const std::bad_alloc&) {
        failPatchException(aError, PatchError::AllocationFailure,
                           "JSON Merge Patch ran out of memory");
        return false;
    } catch (const std::exception&) {
        failPatchException(aError, PatchError::InternalError,
                           "JSON Merge Patch failed with an internal exception");
        return false;
    } catch (...) {
        failPatchException(aError, PatchError::InternalError,
                           "JSON Merge Patch failed with an unknown exception");
        return false;
    }
}
/*static*/
// Compares stored JSON numbers exactly across signed, unsigned, and double
// representations without rounding an integer through binary64. The result is
// -1/0/1, or 2 when a NaN makes the ordering unordered.
int pjsonImpl::_compareNumbers(const pjson& aLeft, const pjson& aRight) {
    const jsonType lt = aLeft._eType;
    const jsonType rt = aRight._eType;

    // ---- integer vs integer (any signedness) ----
    if (lt != pjson::jsonNumberDouble && rt != pjson::jsonNumberDouble) {
        const bool lu = lt == pjson::jsonNumberUInt;
        const bool ru = rt == pjson::jsonNumberUInt;
        if (!lu && !ru) {
            const int64_t l = aLeft._uValue._valueInt;
            const int64_t r = aRight._uValue._valueInt;
            return l < r ? -1 : (l > r ? 1 : 0);
        }
        if (lu && ru) {
            const uint64_t l = aLeft._uValue._valueUInt;
            const uint64_t r = aRight._uValue._valueUInt;
            return l < r ? -1 : (l > r ? 1 : 0);
        }
        // One signed, one unsigned. A negative signed value is always smaller.
        const int64_t s = lu ? aRight._uValue._valueInt : aLeft._uValue._valueInt;
        const uint64_t u = lu ? aLeft._uValue._valueUInt : aRight._uValue._valueUInt;
        int cmp;
        if (s < 0) {
            cmp = -1; // signed < unsigned
        } else {
            const uint64_t su = static_cast<uint64_t>(s);
            cmp = su < u ? -1 : (su > u ? 1 : 0);
        }
        // cmp expresses (signed operand) vs (unsigned operand); flip if the
        // unsigned operand was on the left.
        return lu ? -cmp : cmp;
    }

    // ---- double vs double ----
    if (lt == pjson::jsonNumberDouble && rt == pjson::jsonNumberDouble) {
        const double left = aLeft._uValue._valueDouble;
        const double right = aRight._uValue._valueDouble;
        if (std::isnan(left) || std::isnan(right))
            return 2;
        if (left < right)
            return -1;
        return left > right ? 1 : 0;
    }

    // ---- integer vs double ----
    const bool intOnLeft = lt != pjson::jsonNumberDouble;
    const pjson& intNode = intOnLeft ? aLeft : aRight;
    const double floating = intOnLeft ? aRight._uValue._valueDouble : aLeft._uValue._valueDouble;
    if (std::isnan(floating))
        return 2;

    // Compare the integer against the double exactly. Represent the integer's
    // value and compare via a double truncation plus fractional tiebreak.
    int intVsDouble = 0;
    if (intNode._eType == pjson::jsonNumberUInt) {
        const uint64_t integer = intNode._uValue._valueUInt;
        if (floating >= 18446744073709551616.0) { // 2^64
            intVsDouble = -1;
        } else if (floating < 0.0) {
            intVsDouble = 1;
        } else {
            const uint64_t truncated = static_cast<uint64_t>(floating);
            if (integer != truncated) {
                intVsDouble = integer < truncated ? -1 : 1;
            } else {
                const double integralPart = static_cast<double>(truncated);
                if (floating != integralPart)
                    intVsDouble = floating > integralPart ? -1 : 1;
            }
        }
    } else {
        const int64_t integer = intNode._uValue._valueInt;
        if (floating >= 9223372036854775808.0) { // exact 2^63
            intVsDouble = -1;
        } else if (floating < -9223372036854775808.0) {
            intVsDouble = 1;
        } else {
            const int64_t truncated = static_cast<int64_t>(floating);
            if (integer != truncated) {
                intVsDouble = integer < truncated ? -1 : 1;
            } else {
                const double integralPart = static_cast<double>(truncated);
                if (floating != integralPart)
                    intVsDouble = floating > integralPart ? -1 : 1;
            }
        }
    }
    return intOnLeft ? intVsDouble : -intVsDouble;
}
// Public exact numeric comparison. Delegates to the internal comparator, which
// returns 2 for a NaN-involved (unordered) comparison; that and any non-numeric
// operand are reported as "no ordering" so callers never see a rounded result.
bool pjson::tryCompareNumber(const pjson& aOther, int& aOrder) const noexcept {
    if (!isNumber() || !aOther.isNumber())
        return false;
    const int order = pjsonImpl::_compareNumbers(*this, aOther);
    if (order == 2) // unordered (NaN)
        return false;
    aOrder = order;
    return true;
}
// Deep structural equality. Numbers compare by value across int/double
// (1 == 1.0); arrays element-wise in order; objects by key/value. The walk is
// iterative (an explicit pair work-list) so it never overflows the call stack
// on deeply nested documents.
bool pjson::operator==(const pjson& aOther) const {
    struct Pair {
        const pjson* a;
        const pjson* b;
    };
    std::vector<Pair> work;
    Pair root = {this, &aOther};
    work.push_back(root);

    while (!work.empty()) {
        Pair cur = work.back();
        work.pop_back();
        const pjson& lhs = *cur.a;
        const pjson& rhs = *cur.b;

        // Numbers compare across int/uint/double as one family.
        bool lNum = lhs.isNumber();
        bool rNum = rhs.isNumber();
        if (lNum && rNum) {
            if (pjsonImpl::_compareNumbers(lhs, rhs) != 0) {
                return false;
            }
            continue;
        }

        if (lhs._eType != rhs._eType) {
            return false;
        }

        switch (lhs._eType) {
            case jsonType::jsonNull:
                break;
            case jsonType::jsonString:
                if (*lhs._uValue._pValueString != *rhs._uValue._pValueString)
                    return false;
                break;
            case jsonType::jsonBoolean:
                if (lhs._uValue._valueBool != rhs._uValue._valueBool)
                    return false;
                break;
            case jsonType::jsonNumberInt:
                if (lhs._uValue._valueInt != rhs._uValue._valueInt)
                    return false;
                break;
            case jsonType::jsonNumberUInt:
                if (lhs._uValue._valueUInt != rhs._uValue._valueUInt)
                    return false;
                break;
            case jsonType::jsonNumberDouble:
                if (lhs._uValue._valueDouble != rhs._uValue._valueDouble)
                    return false;
                break;
            case jsonType::jsonArray: {
                if (lhs._uValue._pValueArray->size() != rhs._uValue._pValueArray->size()) {
                    return false;
                }
                for (size_t i = 0; i < lhs._uValue._pValueArray->size(); ++i) {
                    Pair p = {(*lhs._uValue._pValueArray)[i], (*rhs._uValue._pValueArray)[i]};
                    work.push_back(p);
                }
                break;
            }
            case jsonType::jsonObject: {
                if (lhs._uValue._pValueMap->size() != rhs._uValue._pValueMap->size()) {
                    return false;
                }
                auto a = lhs._uValue._pValueMap->begin();
                auto b = rhs._uValue._pValueMap->begin();
                for (; a != lhs._uValue._pValueMap->end(); ++a, ++b) {
                    if (a->first != b->first) {
                        return false; // keys (sorted) differ
                    }
                    Pair p = {a->second, b->second};
                    work.push_back(p);
                }
                break;
            }
        }
    }
    return true;
}
// Implements inequality as the exact complement of structural equality.
bool pjson::operator!=(const pjson& aOther) const {
    return !(*this == aOther);
}
