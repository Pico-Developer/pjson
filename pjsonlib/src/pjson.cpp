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
#include "pjson.h"

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

//===----------------------------------------------------------------------===//
// pjsonImpl — all parsing, schema-validation, and encoding helpers.
//
// Keeping implementation-only operations in one friend struct leaves pjson.h
// declaration-focused while allowing these helpers to maintain DOM invariants.
//===----------------------------------------------------------------------===//
struct ByteDance::pjsonImpl {
    // Public APIs deliberately hide the owning container representation.
    typedef std::vector<pjson*> ArrayStorage;
    typedef std::map<std::string, pjson*> ObjectStorage;

    // Parser state threaded through the recursive-descent scanner: the input
    // buffer, cursor, options, current/maximum nesting depth, a running count
    // of allocated nodes (bounded by maxNodes to stop memory-amplification
    // attacks), and the first error encountered (if any).
    struct ParseCtx {
        const char* src;
        size_t pos;
        size_t end;
        pjson::ParseOptions::DuplicateKeyPolicy duplicateKeys;
        int depth;
        int maxDepth;
        size_t nodeCount;
        size_t maxNodes; // 0 = unlimited
        pjson::Allocator* allocator;
        bool failed;
        size_t errPos;
        std::string errMsg;
    };

    // One suspended container in the iterative serializer. Exactly one of
    // array/object is active according to isObject; the associated cursor
    // always denotes the next child to emit.
    struct SerializeFrame {
        bool isObject;
        size_t depth;
        bool first;
        const ArrayStorage* array;
        size_t arrayIndex;
        const ObjectStorage* object;
        ObjectStorage::const_iterator objectIt;
        ObjectStorage::const_reverse_iterator objectReverseIt;
    };

    // One compiled schema regex or a cached policy/syntax rejection. Keeping
    // failures in the cache is as important as caching successful compilation:
    // patternProperties must not repeatedly parse an invalid expression.
    struct RegexCacheEntry {
        enum State { Uninitialized, Ready, PatternTooLarge, UnsafePattern, InvalidPattern };

        State state;
        std::regex expression;

        RegexCacheEntry()
                : state(Uninitialized) {}
    };

    // Mutable limits and recursion state shared by one schema-validation run.
    // activeRefs tracks (instance, schema) pairs rather than schema nodes alone:
    // revisiting a schema at a different instance is legitimate, while the same
    // pair indicates a cyclic $ref evaluation.
    struct SchemaValidationCtx {
        const pjson& rootSchema;
        const pjson::SchemaOptions& options;
        std::vector<pjson::SchemaError>* publicErrors;
        size_t depth;
        size_t refResolutions;
        size_t workUsed;
        size_t errorsUsed;
        size_t publicErrorStart;
        bool aborted;
        std::vector<std::pair<const pjson*, const pjson*>> activeRefs;
        std::map<std::string, RegexCacheEntry> regexCache;

        // Starts a validation run with no active recursion or resolved references.
        SchemaValidationCtx(const pjson& aRootSchema, const pjson::SchemaOptions& aOptions,
                            std::vector<pjson::SchemaError>* aPublicErrors)
                : rootSchema(aRootSchema)
                , options(aOptions)
                , publicErrors(aPublicErrors)
                , depth(0)
                , refResolutions(0)
                , workUsed(0)
                , errorsUsed(0)
                , publicErrorStart(aPublicErrors == nullptr ? 0 : aPublicErrors->size())
                , aborted(false) {}
    };

    struct SchemaBudgetExceeded {};

    // Facade over a caller or speculative error vector that enforces one shared
    // per-validation diagnostic budget without exposing a public container type.
    struct SchemaErrorSink {
        std::vector<pjson::SchemaError>& values;
        SchemaValidationCtx& ctx;
        bool reported;
        size_t discardedFailures;

        SchemaErrorSink(std::vector<pjson::SchemaError>& aValues, SchemaValidationCtx& aCtx,
                        bool aReported = true)
                : values(aValues)
                , ctx(aCtx)
                , reported(aReported)
                , discardedFailures(0) {}

        size_t size() const { return reported ? values.size() : discardedFailures; }

        void push_back(const pjson::SchemaError& error) {
            if (ctx.aborted)
                return;
            if (!reported) {
                // Speculative anyOf/oneOf/not branches need only a pass/fail
                // signal. Retaining every hidden diagnostic would let an
                // attacker amplify a compact schema into large scratch vectors.
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
                        ctx.publicErrors->push_back(pjson::SchemaError(
                            error.path, "schema validation error budget exceeded"));
                    } catch (...) {
                        // The validation result remains a safe failure even if
                        // the best-effort terminal diagnostic cannot allocate.
                        ctx.publicErrors = nullptr;
                    }
                }
                throw SchemaBudgetExceeded();
            }
            values.push_back(error);
            ++ctx.errorsUsed;
        }
    };

    static bool _isWhitespace(char c);
    static void _appendUtf8(uint32_t aCodePoint, std::string& aOut);
    static bool _hex4(const char* aSrc, size_t aStart, uint32_t& aOut);
    static int _utf8Len(const char* src, size_t pos, size_t end);
    static std::string _formatDouble(double aValue);
    static bool _parseDouble(const std::string& aText, double& aValue);

    static bool _fail(ParseCtx& c, size_t aPos, const char* aMsg);
    static pjson* _newNode(ParseCtx& c); // budget-checked allocation (nullptr on overflow)
    static bool _peek(ParseCtx& c, char& aOut);
    static bool _skipColon(ParseCtx& c);
    static bool _parseValue(ParseCtx& c, pjson*& aOut);
    static bool _parseString(ParseCtx& c, pjson*& aOut);
    static bool _extractString(ParseCtx& c, std::string& aOut);
    static bool _decodeStringBody(ParseCtx& c, std::string& aOut, bool bStopAtQuote);
    static bool _parseKeyword(ParseCtx& c, pjson*& aOut);
    static bool _parseNumber(ParseCtx& c, pjson*& aOut);
    static bool _parseArray(ParseCtx& c, pjson*& aOut);
    static bool _parseObject(ParseCtx& c, pjson*& aOut);
    static pjson::unique_ptr _parseTop(const char* aSrc, size_t aSize,
                                       const pjson::ParseOptions& aOpts, pjson::ParseError* aErr,
                                       pjson::Allocator& aAlloc);
    static pjson::unique_ptr _parseStream(std::istream& aIn, const pjson::ParseOptions& aOpts,
                                          pjson::ParseError* aErr, pjson::Allocator& aAlloc);
    template <typename Sink>
    static bool _writeEscapedTo(Sink& aOut, const std::string& aIn, bool bEscapeNonAscii);
    template <typename Sink>
    static bool _openOrEmit(Sink& aOut, const pjson* aValue, size_t aDepth,
                            const pjson::SerializeOptions& aOpts,
                            std::vector<SerializeFrame>& aFrames);
    template <typename Sink>
    static bool _writeValueTo(Sink& aOut, const pjson& aValue,
                              const pjson::SerializeOptions& aOpts);
    static void _appendValue(std::string& aOut, const pjson& aValue,
                             const pjson::SerializeOptions& aOpts);
    static bool _writeValue(std::ostream& aOut, const pjson& aValue,
                            const pjson::SerializeOptions& aOpts);
    static bool _parseSaxTop(const char* aSrc, size_t aSize, pjson::SaxHandler& aHandler,
                             const pjson::ParseOptions& aOpts, pjson::ParseError* aErr);
    static bool _parseSaxStream(std::istream& aIn, pjson::SaxHandler& aHandler,
                                const pjson::ParseOptions& aOpts, pjson::ParseError* aErr);

    static std::string _pointerAppend(const std::string& aBase, const std::string& aToken);
    static bool _validateCtx(const pjson& aNode, const pjson& aSchema, const std::string& aPath,
                             SchemaErrorSink& aErrors, SchemaValidationCtx& aCtx);
    static bool _validate(const pjson& aNode, const pjson& aSchema, const std::string& aPath,
                          std::vector<pjson::SchemaError>& aErrors,
                          const pjson::SchemaOptions& aOpts) noexcept;
    static bool _typeMatches(const pjson& aNode, const std::string& aTypeName);
    static std::string _typeName(const pjson& aNode);
    static bool _isSafeRegex(const std::string& aPattern);

    // Internal typed/storage access keeps representation and permissive
    // conversion helpers out of the public API. Callers first establish type.
    static ArrayStorage& _array(pjson& aValue) { return *aValue._uValue._pValueArray; }
    static const ArrayStorage& _array(const pjson& aValue) { return *aValue._uValue._pValueArray; }
    static ObjectStorage& _object(pjson& aValue) { return *aValue._uValue._pValueMap; }
    static const ObjectStorage& _object(const pjson& aValue) { return *aValue._uValue._pValueMap; }
    static int64_t _integer(const pjson& aValue) { return aValue._uValue._valueInt; }
    static double _floating(const pjson& aValue) { return aValue._uValue._valueDouble; }
    static double _numberAsDouble(const pjson& aValue) {
        return aValue._eType == pjson::jsonNumberInt ? static_cast<double>(aValue._uValue._valueInt)
                                                     : aValue._uValue._valueDouble;
    }
    static bool _boolean(const pjson& aValue) { return aValue._uValue._valueBool; }
    static const std::string& _string(const pjson& aValue) { return *aValue._uValue._pValueString; }
    // Returns -1, 0, or 1, and 2 when either floating operand is NaN.
    static int _compareNumbers(const pjson& aLeft, const pjson& aRight);
    static bool _equalWithBudget(const pjson& aLeft, const pjson& aRight, SchemaValidationCtx& aCtx,
                                 SchemaErrorSink& aErrors, const std::string& aPath, bool& aEqual);

    // Iteratively frees every descendant pjson of node's array/map, leaving the
    // node's own top-level container allocated but empty (a no-op for scalars).
    // Using an explicit work-list instead of the recursive destructor keeps
    // teardown safe on arbitrarily deep documents. Marked noexcept: it is
    // reached from ~pjson, so an allocation failure here terminates rather than
    // escaping a destructor.
    static void _disposeChildren(pjson& node) noexcept;
    static pjson::Allocator& _defaultAllocator() noexcept;
    static pjson* _allocateNode(pjson::Allocator& aAlloc);
    static void _destroyNode(pjson* aValue) noexcept;
    static pjson::unique_ptr _makeNode(pjson::Allocator& aAlloc);
    static pjson::unique_ptr _cloneNode(const pjson& aValue, pjson::Allocator& aAlloc);
};

// File-scope aliases keep internal type names concise without exposing the
// owning containers in the public header.
typedef pjson::jsonType jsonType;
typedef pjsonImpl::ArrayStorage PJSONARRAY;
typedef pjsonImpl::ObjectStorage PJSONMAP;
typedef pjson::SchemaError SchemaError;
typedef pjson::SchemaOptions SchemaOptions;
typedef pjson::ParseOptions ParseOptions;
typedef pjson::ParseError ParseError;
typedef pjson::SaxHandler SaxHandler;
typedef pjsonImpl::ParseCtx ParseCtx;

// Schema validation still uses native recursion for applicator keywords. Keep
// its logical depth below a conservative stack-safe ceiling even when callers
// request a larger value. Consecutive local references are resolved iteratively
// but continue to consume this same logical-depth budget.
static const size_t kSchemaValidationDepthHardLimit = 128;

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

    // Publishes a buffer-parser failure, deriving source coordinates from the
    // authoritative byte offset. A null destination intentionally discards it.
    void setParseError(ParseError* err, const char* src, size_t size, size_t offset,
                       const std::string& message) {
        if (!err)
            return;
        err->ok = false;
        err->offset = offset;
        lineAndColumn(src, size, offset, err->line, err->column);
        err->message = message;
    }

    // Restores the public error object to its successful, start-of-input state.
    void resetParseError(ParseError* err) {
        if (!err)
            return;
        err->ok = true;
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

            errno = 0;
            const long long llVal = strtoll(text.c_str(), nullptr, 10);
            if (errno == ERANGE) {
                double d = 0.0;
                if (!pjsonImpl::_parseDouble(text, d) || !std::isfinite(d))
                    return fail("number out of range");
                return !emit || dispatch(handler.onDouble(d));
            }
            return !emit || dispatch(handler.onInt(static_cast<int64_t>(llVal)));
        }

        // Parses an array while explicitly tracking comma state so leading,
        // repeated, missing, and trailing commas receive deterministic errors.
        bool parseArray(size_t depth, bool emit) {
            const size_t maxDepth = opts.maxDepth > 0 ? static_cast<size_t>(opts.maxDepth) : 1U;
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
            const size_t maxDepth = opts.maxDepth > 0 ? static_cast<size_t>(opts.maxDepth) : 1U;
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
        , duplicateKeys(RejectDuplicateKeys) {}
// Establishes compact, UTF-8-preserving, ascending-key serialization.
pjson::SerializeOptions::SerializeOptions()
        : pretty(false)
        , indentWidth(2)
        , indentCharacter(' ')
        , escapeNonAscii(false)
        , keyOrder(AscendingKeys)
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
// Constructs an empty schema diagnostic.
pjson::SchemaError::SchemaError() {}
// Captures one validation failure at its instance JSON Pointer.
pjson::SchemaError::SchemaError(const std::string& aPath, const std::string& aMsg)
        : path(aPath)
        , message(aMsg) {}
// Establishes bounded regex, recursion, and reference work with format checks enabled.
pjson::SchemaOptions::SchemaOptions()
        : maxRegexPatternBytes(256)
        , maxRegexSubjectBytes(4096)
        , allowUnsafeRegex(false)
        , maxValidationDepth(kSchemaValidationDepthHardLimit)
        , maxRefResolutions(1024)
        , maxValidationWork(1000000)
        , maxErrors(100)
        , validateFormats(true) {}
/*static*/
// Removes regex size/safety restrictions for schemas from a trusted source;
// unrelated validation limits retain their defaults.
pjson::SchemaOptions pjson::SchemaOptions::trustedRegex() {
    SchemaOptions o;
    o.maxRegexPatternBytes = 0;
    o.maxRegexSubjectBytes = 0;
    o.allowUnsafeRegex = true;
    return o;
}
//===----------------------------------------------------------------------===//
// Allocator bridge and node ownership
//
// Containers and strings are constructed in allocator-provided storage. Nodes
// additionally remember whether their outer object came from that allocator so
// the uniform ValueDeleter can also destroy ordinary `new pjson` roots safely.
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
// Provides unique_ptr with the same origin-aware destruction used by DOM owners.
void pjson::ValueDeleter::operator()(pjson* aValue) const noexcept {
    pjsonImpl::_destroyNode(aValue);
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
    copyContentsFrom(aFrom);
}
// Deep-copies a value into a specifically selected allocator domain.
pjson::pjson(const pjson& aFrom, Allocator& aAlloc)
        : _allocator(&aAlloc)
        , _allocatorOwnedNode(false)
        , _disposeNext(nullptr)
        , _eType(jsonType::jsonNull)
        , _uValue() {
    copyContentsFrom(aFrom);
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
        copyContentsFrom(aFrom);
        aFrom.reset();
    }
}
// Replaces this value from an rvalue, using constant-time transfer only when
// both allocator domains match. Self-move is a no-op.
pjson& pjson::operator=(pjson&& aFrom) {
    if (&aFrom == this)
        return *this;

    if (_allocator == aFrom._allocator) {
        reset();
        _eType = aFrom._eType;
        std::memcpy(&_uValue, &aFrom._uValue, sizeof(_uValue));
        aFrom._eType = jsonType::jsonNull;
        aFrom._uValue._pValueRaw = nullptr;
    } else {
        pjson tmp(std::move(aFrom), *_allocator);
        swap(tmp);
    }

    return *this;
}
// O(1) exchange of two nodes' contents (type tag + inline storage). noexcept,
// which is what lets the move operations and copy-and-swap assignment below
// offer their exception guarantees.
void pjson::swap(pjson& aOther) noexcept {
    static_assert(std::is_trivially_copyable<Storage>::value,
                  "pjson storage must remain safe for bytewise swap");
    if (this == &aOther || !canSwap(aOther))
        return;
    std::swap(_eType, aOther._eType);
    Storage temp;
    std::memcpy(&temp, &_uValue, sizeof(temp));
    std::memcpy(&_uValue, &aOther._uValue, sizeof(_uValue));
    std::memcpy(&aOther._uValue, &temp, sizeof(aOther._uValue));
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
    return _eType == jsonNumberInt || _eType == jsonNumberDouble;
}
bool pjson::isInt() const {
    return _eType == jsonNumberInt;
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
// Exact extraction overloads leave the destination unchanged on type mismatch;
// only double extraction also accepts an integer through widening conversion.
bool pjson::tryGet(int64_t& aResult) const noexcept {
    if (_eType != jsonType::jsonNumberInt)
        return false;
    aResult = _uValue._valueInt;
    return true;
}
bool pjson::tryGet(double& aResult) const noexcept {
    if (_eType == jsonType::jsonNumberInt) {
        aResult = static_cast<double>(_uValue._valueInt);
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
    if (aeType < jsonType::jsonNull || aeType > jsonType::jsonObject)
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
// Populates this node from aFrom without recursion. If copying fails, partial
// descendants are reclaimed and this node is reset to a valid null state.
void pjson::copyContentsFrom(const pjson& aFrom) {
    // Iterative deep copy. A recursive copy would overflow the stack on very
    // deep documents, so we walk with an explicit work-list: each item pairs a
    // source node with the destination node to populate from it. Scalars are
    // copied immediately; array/map children are queued.
    try {
        resetTo(aFrom.getType());
        if (_eType != jsonType::jsonArray && _eType != jsonType::jsonObject) {
            switch (_eType) {
                case jsonType::jsonString:
                    *_uValue._pValueString = *(aFrom._uValue._pValueString);
                    break;
                case jsonType::jsonNumberInt:
                    _uValue._valueInt = aFrom._uValue._valueInt;
                    break;
                case jsonType::jsonNumberDouble:
                    _uValue._valueDouble = aFrom._uValue._valueDouble;
                    break;
                case jsonType::jsonBoolean:
                    _uValue._valueBool = aFrom._uValue._valueBool;
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
        Item start = {&aFrom, this};
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
                    unique_ptr child = pjsonImpl::_makeNode(*_allocator);
                    child->resetTo(elem->getType());
                    dst._uValue._pValueArray->push_back(child.get());
                    pjson* attached = child.release();
                    if (elem->_eType == jsonType::jsonArray ||
                        elem->_eType == jsonType::jsonObject) {
                        Item it = {elem, attached};
                        work.push_back(it);
                    } else {
                        attached->copyContentsFrom(*elem);
                    }
                }
            } else { // jsonObject
                for (const auto& kv : *src._uValue._pValueMap) {
                    unique_ptr child = pjsonImpl::_makeNode(*_allocator);
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
                        attached->copyContentsFrom(*kv.second);
                    }
                }
            }
        }
    } catch (...) {
        reset();
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
pjson::unique_ptr pjsonImpl::_makeNode(pjson::Allocator& aAlloc) {
    return pjson::unique_ptr(pjsonImpl::_allocateNode(aAlloc));
}
/*static*/
// Deep-clones a complete subtree into the supplied allocator domain.
pjson::unique_ptr pjsonImpl::_cloneNode(const pjson& aValue, pjson::Allocator& aAlloc) {
    pjson::unique_ptr result = _makeNode(aAlloc);
    result->copyContentsFrom(aValue);
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
                , _invalidUtf8(false) {}

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
        explicit operator bool() const { return _valid; }
        size_t size() const { return _written; }
        bool hasInvalidUtf8() const { return _invalidUtf8; }

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
        case jsonType::jsonNumberDouble: {
            const std::string text = _formatDouble(aValue->_uValue._valueDouble);
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
            pjson::unique_ptr child = pjsonImpl::_makeNode(aTarget.getAllocator());
            *child = aValue;
            pjsonImpl::_array(replacement).push_back(nullptr);
            pjsonImpl::_array(replacement).back() = child.release();
            aTarget.swap(replacement);
            return;
        }

        pjson::unique_ptr child = pjsonImpl::_makeNode(aTarget.getAllocator());
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
                pjson::unique_ptr rollback(array.back());
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
//===----------------------------------------------------------------------===//
// Container access and lookup
//
// Mutating operator[] access auto-vivifies missing containers and children;
// find() is non-mutating. Negative array indices count from the end. Mutating
// indices before the beginning clamp to zero, while lookup indices simply miss.
//===----------------------------------------------------------------------===//

// Returns or creates an object member, atomically promoting non-object values.
pjson& pjson::operator[](const char* aSkey) {
    if (aSkey == nullptr)
        throw std::invalid_argument("pjson object key requires non-null input");
    if (_eType != jsonType::jsonObject) {
        pjson replacement(*_allocator);
        replacement.resetTo(jsonType::jsonObject);
        unique_ptr child = pjsonImpl::_makeNode(*_allocator);
        const std::pair<PJSONMAP::iterator, bool> inserted = replacement._uValue._pValueMap->insert(
            std::make_pair(std::string(aSkey), static_cast<pjson*>(nullptr)));
        pjson* result = child.release();
        inserted.first->second = result;
        swap(replacement);
        return *result;
    }
    PJSONMAP::iterator it = _uValue._pValueMap->find(aSkey);
    if (it != _uValue._pValueMap->end()) {
        return *(it->second);
    }
    unique_ptr child = pjsonImpl::_makeNode(*_allocator);
    const std::pair<PJSONMAP::iterator, bool> inserted = _uValue._pValueMap->insert(
        std::make_pair(std::string(aSkey), static_cast<pjson*>(nullptr)));
    pjson* result = inserted.first->second;
    if (inserted.second) {
        result = child.release();
        inserted.first->second = result;
    }
    return *result;
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
                unique_ptr child = pjsonImpl::_makeNode(*_allocator);
                array.push_back(nullptr);
                array.back() = child.release();
            }
        } catch (...) {
            while (array.size() > originalSize) {
                unique_ptr rollback(array.back());
                array.pop_back();
            }
            throw;
        }
    }

    return *array[position];
}
pjson& pjson::operator[](const std::string& aString) {
    return (*this)[aString.c_str()];
}
pjson* pjson::find(const std::string& aKey) {
    return find(aKey.c_str());
}
// Finds an object member without inserting or changing the receiver.
pjson* pjson::find(const char* aKey) {
    if (aKey != nullptr && _eType == jsonType::jsonObject) {
        auto it = _uValue._pValueMap->find(aKey);
        if (it != _uValue._pValueMap->end()) {
            return it->second;
        }
    }
    return nullptr;
}
const pjson* pjson::find(const std::string& aKey) const {
    return find(aKey.c_str());
}
const pjson* pjson::find(const char* aKey) const {
    if (aKey != nullptr && _eType == jsonType::jsonObject) {
        auto it = _uValue._pValueMap->find(aKey);
        if (it != _uValue._pValueMap->end()) {
            return it->second;
        }
    }
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
// Helpers accept ownership of values through unique_ptr and release only after
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
                           const std::string& aPointer, pjson::unique_ptr aValue,
                           PatchBudget& aBudget, PatchError& aError) {
        if (aTokens.empty()) {
            aRoot.swap(*aValue);
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
                existing->second->swap(*aValue);
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
                          const std::string& aPointer, pjson::unique_ptr aValue,
                          PatchBudget& aBudget, PatchError& aError) {
        if (aTokens.empty()) {
            aRoot.swap(*aValue);
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
            existing->second->swap(*aValue);
            return true;
        }

        if (parent->isArray()) {
            size_t index = 0;
            bool append = false;
            if (!patchArrayIndex(*parent, token, false, finalIndex, index, append, aError))
                return false;
            pjsonImpl::_array(*parent)[index]->swap(*aValue);
            return true;
        }

        return failPatchAtToken(aError, PatchError::TargetMissing, finalIndex, token,
                                "replace target parent is not a container");
    }

    // Detaches a target without destroying it. Removing the document root is
    // represented by replacing the still-addressable root value with JSON null.
    bool detachAtPointer(pjson& aRoot, const std::vector<std::string>& aTokens,
                         const std::string& aPointer, pjson::unique_ptr& aValue,
                         PatchBudget& aBudget, PatchError& aError) {
        if (aTokens.empty()) {
            if (!chargePatch(aBudget.nodes, aBudget.nodeLimit, 1, aError,
                             "JSON Patch cloned-node budget exceeded") ||
                !chargePatch(aBudget.bytes, aBudget.byteLimit, sizeof(pjson), aError,
                             "JSON Patch cloned-byte budget exceeded"))
                return false;
            pjson::unique_ptr replacement = pjsonImpl::_makeNode(aRoot.getAllocator());
            aRoot.swap(*replacement);
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
    bool insertObjectChild(pjson& aObject, const std::string& aKey, pjson::unique_ptr aChild) {
        PJSONMAP* object = &pjsonImpl::_object(aObject);
        PJSONMAP::iterator existing = object->find(aKey);
        if (existing != object->end()) {
            existing->second->swap(*aChild);
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
                        pjson::unique_ptr child = pjsonImpl::_makeNode(item.target->getAllocator());
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
                pjson::unique_ptr replacement =
                    pjsonImpl::_cloneNode(patchValue, item.target->getAllocator());
                if (!insertObjectChild(*item.target, key, std::move(replacement)))
                    return false;
            }
        }
        return true;
    }
} // namespace

// Key/index extraction overloads combine non-mutating lookup with exact
// tryGet conversion and leave output parameters unchanged on any miss.
bool pjson::tryGet(const std::string& aKey, int64_t& aResult) const {
    return tryGet(aKey.c_str(), aResult);
}
bool pjson::tryGet(const std::string& aKey, double& aResult) const {
    return tryGet(aKey.c_str(), aResult);
}
bool pjson::tryGet(const std::string& aKey, bool& aResult) const {
    return tryGet(aKey.c_str(), aResult);
}
bool pjson::tryGet(const std::string& aKey, std::string& aResult) const {
    return tryGet(aKey.c_str(), aResult);
}
bool pjson::tryGet(const std::string& aKey, StringView& aResult) const {
    return tryGet(aKey.c_str(), aResult);
}
bool pjson::tryGet(const char* aKey, int64_t& aResult) const {
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
// the origin-aware pjson::unique_ptr, including roots from the default allocator.
//===----------------------------------------------------------------------===//

/*static*/
// Parses string-owned bytes with default allocation and omitted diagnostics.
pjson::unique_ptr pjson::parse(const std::string& aStr, const ParseOptions& aOpts) {
    return pjsonImpl::_parseTop(aStr.c_str(), aStr.length(), aOpts, nullptr,
                                pjsonImpl::_defaultAllocator());
}
/*static*/
// Parses an explicit byte span with default allocation and omitted diagnostics.
pjson::unique_ptr pjson::parse(const char* aSrc, size_t aSize, const ParseOptions& aOpts) {
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
pjson::unique_ptr pjson::parse(const std::string& aStr, ParseError& aError,
                               const ParseOptions& aOpts) {
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
pjson::unique_ptr pjson::parse(const char* aSrc, size_t aSize, ParseError& aError,
                               const ParseOptions& aOpts) {
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
pjson::unique_ptr pjson::parseStream(std::istream& aIn, const ParseOptions& aOpts) {
    return pjsonImpl::_parseStream(aIn, aOpts, nullptr, pjsonImpl::_defaultAllocator());
}
/*static*/
// Parses a stream with default allocation and caller-visible diagnostics.
pjson::unique_ptr pjson::parseStream(std::istream& aIn, ParseError& aError,
                                     const ParseOptions& aOpts) {
    return pjsonImpl::_parseStream(aIn, aOpts, &aError, pjsonImpl::_defaultAllocator());
}
/*static*/
// Parses string-owned bytes with nodes and wrapper objects from aAlloc.
pjson::unique_ptr pjson::parse(const std::string& aStr, Allocator& aAlloc,
                               const ParseOptions& aOpts) {
    return pjsonImpl::_parseTop(aStr.c_str(), aStr.length(), aOpts, nullptr, aAlloc);
}
/*static*/
// Parses a byte span with nodes and wrapper objects from aAlloc.
pjson::unique_ptr pjson::parse(const char* aSrc, size_t aSize, Allocator& aAlloc,
                               const ParseOptions& aOpts) {
    return pjsonImpl::_parseTop(aSrc, aSize, aOpts, nullptr, aAlloc);
}
/*static*/
// Parses string-owned bytes with custom allocation and detailed diagnostics.
pjson::unique_ptr pjson::parse(const std::string& aStr, ParseError& aError, Allocator& aAlloc,
                               const ParseOptions& aOpts) {
    return pjsonImpl::_parseTop(aStr.c_str(), aStr.length(), aOpts, &aError, aAlloc);
}
/*static*/
// Parses a byte span with custom allocation and detailed diagnostics.
pjson::unique_ptr pjson::parse(const char* aSrc, size_t aSize, ParseError& aError,
                               Allocator& aAlloc, const ParseOptions& aOpts) {
    return pjsonImpl::_parseTop(aSrc, aSize, aOpts, &aError, aAlloc);
}
/*static*/
// Parses a stream with nodes and wrapper objects from aAlloc.
pjson::unique_ptr pjson::parseStream(std::istream& aIn, Allocator& aAlloc,
                                     const ParseOptions& aOpts) {
    return pjsonImpl::_parseStream(aIn, aOpts, nullptr, aAlloc);
}
/*static*/
// Parses a stream with custom allocation and detailed diagnostics.
pjson::unique_ptr pjson::parseStream(std::istream& aIn, ParseError& aError, Allocator& aAlloc,
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
// has been materialized.
/*static*/
pjson::unique_ptr pjsonImpl::_parseStream(std::istream& aIn, const ParseOptions& aOpts,
                                          ParseError* aErr, pjson::Allocator& aAlloc) {
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
                          "input exceeds maxInputBytes");
            return pjson::unique_ptr();
        }
        content.append(buffer, chunk);
    }
    if (aIn.bad()) {
        setParseError(aErr, content.data(), content.size(), content.size(), "stream read failed");
        return pjson::unique_ptr();
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
// consumes the shared node budget, and local unique_ptr guards retain ownership
// until a child is attached. The first grammar error remains authoritative.
//===----------------------------------------------------------------------===//

// Shared driver: parse a single top-level value, require only trailing
// whitespace, and report success/failure through the optional ParseError.
/*static*/
pjson::unique_ptr pjsonImpl::_parseTop(const char* aSrc, size_t aSize, const ParseOptions& aOpts,
                                       ParseError* aErr, pjson::Allocator& aAlloc) {
    resetParseError(aErr);
    if (aSrc == nullptr) {
        setParseError(aErr, "", 0, 0, "null input");
        return pjson::unique_ptr();
    }

    // Reject an over-large input up front (cheap DoS guard before any work).
    if (aOpts.maxInputBytes != 0 && aSize > aOpts.maxInputBytes) {
        setParseError(aErr, aSrc, aSize, aOpts.maxInputBytes, "input exceeds maxInputBytes");
        return pjson::unique_ptr();
    }

    ParseCtx c;
    c.src = aSrc;
    c.pos = 0;
    c.end = aSize;
    c.duplicateKeys = aOpts.duplicateKeys;
    c.depth = 0;
    c.maxDepth = aOpts.maxDepth > 0 ? aOpts.maxDepth : 1;
    c.nodeCount = 0;
    c.maxNodes = aOpts.maxNodes;
    c.allocator = &aAlloc;
    c.failed = false;
    c.errPos = 0;

    pjson::unique_ptr result;
    try {
        pjson* parsed = nullptr;
        if (!_parseValue(c, parsed)) {
            pjsonImpl::_destroyNode(parsed);
            setParseError(aErr, aSrc, aSize, c.errPos, c.errMsg.empty() ? "parse error" : c.errMsg);
            return pjson::unique_ptr();
        }
        result.reset(parsed);

        // A valid document is a single value; only trailing whitespace may follow.
        char trailing;
        if (_peek(c, trailing)) {
            setParseError(aErr, aSrc, aSize, c.pos, "trailing characters after JSON value");
            return pjson::unique_ptr();
        }
        return result;
    } catch (const std::bad_alloc&) {
        setParseError(aErr, aSrc, aSize, c.pos, "parse ran out of memory");
    } catch (const std::exception& ex) {
        setParseError(aErr, aSrc, aSize, c.pos,
                      std::string("parse failed with exception: ") + ex.what());
    } catch (...) {
        setParseError(aErr, aSrc, aSize, c.pos, "parse failed with exception");
    }
    return pjson::unique_ptr();
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
            pjson::unique_ptr value(_newNode(c));
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
    pjson::unique_ptr value(_newNode(c));
    if (!value)
        return false;
    *value = s;
    aOut = value.release();
    return true;
}
// Parses a JSON number following the grammar
//   -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
// Integers are stored as int64; anything with a fraction/exponent (or an
// integer that overflows int64) is stored as a double. Overflow to a
// non-finite double is rejected. Never throws.
/*static*/
bool pjsonImpl::_parseNumber(ParseCtx& c, pjson*& aOut) {
    const size_t begin = c.pos;
    size_t i = c.pos;
    bool bFloat = false;

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
    if (bFloat) {
        double d = 0.0;
        if (!_parseDouble(sTemp, d) || !std::isfinite(d)) {
            return _fail(c, begin, "number out of range");
        }
        pjson::unique_ptr value(_newNode(c));
        if (!value)
            return false;
        *value = d;
        aOut = value.release();
    } else {
        errno = 0;
        long long llVal = strtoll(sTemp.c_str(), nullptr, 10);
        if (errno == ERANGE) {
            // Too large for int64: fall back to double to avoid data loss.
            double d = 0.0;
            if (!_parseDouble(sTemp, d) || !std::isfinite(d)) {
                return _fail(c, begin, "number out of range");
            }
            pjson::unique_ptr value(_newNode(c));
            if (!value)
                return false;
            *value = d;
            aOut = value.release();
        } else {
            pjson::unique_ptr value(_newNode(c));
            if (!value)
                return false;
            *value = static_cast<int64_t>(llVal);
            aOut = value.release();
        }
    }
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
    pjson::unique_ptr arr(_newNode(c));
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
            pjson::unique_ptr ownedElem(elem);
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
    pjson::unique_ptr obj(_newNode(c));
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
            pjson* val = nullptr;
            if (!_extractString(c, mkey) || !_skipColon(c) || !_parseValue(c, val)) {
                pjsonImpl::_destroyNode(val);
                --c.depth;
                return false;
            }
            // Apply the caller's duplicate-key policy: reject the duplicate or
            // deterministically keep its first or last value.
            auto it = obj->_uValue._pValueMap->find(mkey);
            if (it != obj->_uValue._pValueMap->end()) {
                if (c.duplicateKeys == ParseOptions::RejectDuplicateKeys) {
                    pjsonImpl::_destroyNode(val);
                    --c.depth;
                    return _fail(c, keyOffset, "duplicate object key");
                }
                if (c.duplicateKeys == ParseOptions::KeepLastDuplicate) {
                    pjsonImpl::_destroyNode(it->second);
                    it->second = val;
                } else {
                    pjsonImpl::_destroyNode(val); // KeepFirstDuplicate
                }
            } else {
                pjson::unique_ptr ownedVal(val);
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
    return hasKey(aKey.c_str());
}
// Reports whether an object contains a non-null C-string key.
bool pjson::hasKey(const char* cStr) const {
    if (cStr != nullptr && _eType == jsonType::jsonObject) {
        auto it = _uValue._pValueMap->find(cStr);
        return (it != _uValue._pValueMap->end());
    }
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
    return erase(aKey.c_str());
}
// Removes an object member and destroys its owned subtree.
bool pjson::erase(const char* aKey) {
    if (aKey != nullptr && _eType == jsonType::jsonObject) {
        auto it = _uValue._pValueMap->find(aKey);
        if (it != _uValue._pValueMap->end()) {
            pjsonImpl::_destroyNode(it->second);
            _uValue._pValueMap->erase(it);
            return true;
        }
    }
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
                unique_ptr value = pjsonImpl::_cloneNode(*valueNode, scratch.getAllocator());
                if (!addOwnedAtPointer(scratch, pathTokens, aError.path, std::move(value), budget,
                                       aError))
                    return false;
                continue;
            }

            if (aError.op == "remove") {
                unique_ptr removed;
                if (!detachAtPointer(scratch, pathTokens, aError.path, removed, budget, aError))
                    return false;
                continue;
            }

            if (aError.op == "replace") {
                if (!measureClone(*valueNode, budget, aError))
                    return false;
                unique_ptr value = pjsonImpl::_cloneNode(*valueNode, scratch.getAllocator());
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
                unique_ptr value = pjsonImpl::_cloneNode(*source, scratch.getAllocator());
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

            unique_ptr moved;
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
// Compares stored JSON numbers without rounding an int64_t through binary64.
// The result is -1/0/1, or 2 when a NaN makes the ordering unordered.
int pjsonImpl::_compareNumbers(const pjson& aLeft, const pjson& aRight) {
    if (aLeft._eType == pjson::jsonNumberInt && aRight._eType == pjson::jsonNumberInt) {
        if (aLeft._uValue._valueInt < aRight._uValue._valueInt)
            return -1;
        return aLeft._uValue._valueInt > aRight._uValue._valueInt ? 1 : 0;
    }
    if (aLeft._eType == pjson::jsonNumberDouble && aRight._eType == pjson::jsonNumberDouble) {
        const double left = aLeft._uValue._valueDouble;
        const double right = aRight._uValue._valueDouble;
        if (std::isnan(left) || std::isnan(right))
            return 2;
        if (left < right)
            return -1;
        return left > right ? 1 : 0;
    }

    const bool intOnLeft = aLeft._eType == pjson::jsonNumberInt;
    const int64_t integer = intOnLeft ? aLeft._uValue._valueInt : aRight._uValue._valueInt;
    const double floating = intOnLeft ? aRight._uValue._valueDouble : aLeft._uValue._valueDouble;
    int intVsDouble = 0;
    if (std::isnan(floating)) {
        return 2;
    } else if (floating >= 9223372036854775808.0) { // exact 2^63
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
    return intOnLeft ? intVsDouble : -intVsDouble;
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

        // Numbers compare across int/double as one family.
        bool lNum = (lhs._eType == jsonNumberInt || lhs._eType == jsonNumberDouble);
        bool rNum = (rhs._eType == jsonNumberInt || rhs._eType == jsonNumberDouble);
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

//===----------------------------------------------------------------------===//
// JSON Schema draft-07 subset validation
//
// Validation accumulates ordinary keyword failures but aborts on configured
// depth/reference limits. Combinators evaluate branches into temporary error
// vectors, committing diagnostics only according to the combinator's outcome so
// failed exploratory branches do not leak spurious public errors.
//===----------------------------------------------------------------------===//
// The JSON Schema type name for a value.
/*static*/
std::string pjsonImpl::_typeName(const pjson& aNode) {
    switch (aNode._eType) {
        case jsonType::jsonNull:
            return "null";
        case jsonType::jsonString:
            return "string";
        case jsonType::jsonNumberInt:
            return "integer";
        case jsonType::jsonNumberDouble:
            return "number";
        case jsonType::jsonBoolean:
            return "boolean";
        case jsonType::jsonArray:
            return "array";
        case jsonType::jsonObject:
            return "object";
    }
    return "unknown";
}
// Implements the "type" keyword. "number" accepts integers too; "integer"
// accepts a whole-valued double (e.g. 2.0) as JSON Schema does.
/*static*/
bool pjsonImpl::_typeMatches(const pjson& aNode, const std::string& aTypeName) {
    if (aTypeName == "null")
        return aNode.isNull();
    if (aTypeName == "string")
        return aNode.isString();
    if (aTypeName == "boolean")
        return aNode.isBool();
    if (aTypeName == "array")
        return aNode.isArray();
    if (aTypeName == "object")
        return aNode.isObject();
    if (aTypeName == "number")
        return aNode.isNumber();
    if (aTypeName == "integer") {
        if (aNode.isInt())
            return true;
        // A double with no fractional part counts as an integer.
        if (aNode.isDouble()) {
            double d = pjsonImpl::_floating(aNode);
            return std::floor(d) == d && std::isfinite(d);
        }
        return false;
    }
    return false; // unknown type name never matches
}
// Appends "/token" to a JSON Pointer path, escaping '~' and '/' per RFC 6901.
std::string pjsonImpl::_pointerAppend(const std::string& aBase, const std::string& aToken) {
    std::string escaped;
    escaped.reserve(aToken.size());
    for (char c : aToken) {
        if (c == '~')
            escaped += "~0";
        else if (c == '/')
            escaped += "~1";
        else
            escaped += c;
    }
    return aBase + "/" + escaped;
}
// Conservative single-pass screen for constructs that are especially prone to
// catastrophic backtracking in std::regex. This is intentionally fail-closed:
// unrestricted ECMAScript regex remains available through trustedRegex().
bool pjsonImpl::_isSafeRegex(const std::string& aPattern) {
    bool escaped = false;
    bool inClass = false;
    int groups = 0;
    int quantifiers = 0;
    struct Group {
        bool hasQuantifier;
        bool hasAlternation;
    };
    std::vector<Group> stack;

    for (size_t i = 0; i < aPattern.size(); ++i) {
        const char c = aPattern[i];
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
            // Even apparently simple alternation can become ambiguous when
            // combined with repetition, so the safe subset excludes it.
            return false;
        } else if (c == '*' || c == '+' || c == '?' || c == '{') {
            if (++quantifiers > 1)
                return false;
            if (c == '{') {
                // Keep counted repetitions bounded. Scan only the numeric
                // bounds; malformed syntax is still diagnosed by std::regex.
                size_t j = i + 1;
                size_t first = 0;
                size_t second = 0;
                bool haveFirst = false;
                bool haveSecond = false;
                while (j < aPattern.size() && aPattern[j] >= '0' && aPattern[j] <= '9') {
                    haveFirst = true;
                    if (first > 1000)
                        return false;
                    first = first * 10 + static_cast<size_t>(aPattern[j] - '0');
                    ++j;
                }
                if (j < aPattern.size() && aPattern[j] == ',') {
                    ++j;
                    while (j < aPattern.size() && aPattern[j] >= '0' && aPattern[j] <= '9') {
                        haveSecond = true;
                        if (second > 1000)
                            return false;
                        second = second * 10 + static_cast<size_t>(aPattern[j] - '0');
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
                next < aPattern.size() && (aPattern[next] == '*' || aPattern[next] == '+' ||
                                           aPattern[next] == '?' || aPattern[next] == '{');
            if (quantified && (closed.hasQuantifier || closed.hasAlternation))
                return false;
            if (!stack.empty()) {
                stack.back().hasQuantifier =
                    stack.back().hasQuantifier || quantified || closed.hasQuantifier;
                stack.back().hasAlternation = stack.back().hasAlternation || closed.hasAlternation;
            }
        }
    }
    return true;
}
namespace {
    //===------------------------------------------------------------------===//
    // Exact numeric constraints and format validators
    //===------------------------------------------------------------------===//

    // Normalized decimal magnitude: coefficient * 10^exponent10. Trailing
    // decimal zeroes are folded into the exponent so divisibility can be tested
    // with integer arithmetic rather than floating-point tolerance.
    struct ExactDecimal {
        uint64_t coefficient;
        int exponent10;
    };

    // Computes an int64 magnitude without overflowing on INT64_MIN.
    uint64_t magnitudeOf(int64_t value) {
        return value < 0 ? uint64_t(-(value + 1)) + uint64_t(1) : uint64_t(value);
    }

    // Parses the serializer's finite decimal notation into normalized form;
    // returns false if its bounded coefficient/exponent representation overflows.
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

    // Converts either internal numeric representation to normalized decimal form.
    bool decimalFromNumber(const pjson& value, ExactDecimal& result) {
        if (value.isInt()) {
            result.coefficient = magnitudeOf(pjsonImpl::_integer(value));
            result.exponent10 = 0;
            if (result.coefficient == 0)
                return true;
            while (result.coefficient % uint64_t(10) == 0) {
                result.coefficient /= uint64_t(10);
                ++result.exponent10;
            }
            return true;
        }
        if (!value.isDouble() || !std::isfinite(pjsonImpl::_floating(value)))
            return false;
        return decimalFromText(pjsonImpl::_formatDouble(pjsonImpl::_floating(value)), result);
    }

    // Produces a diagnostic representation without losing integer precision.
    std::string formatNumber(const pjson& value) {
        return value.isInt() ? std::to_string(pjsonImpl::_integer(value))
                             : pjsonImpl::_formatDouble(pjsonImpl::_floating(value));
    }

    // Decodes nonnegative integral size keywords without truncation. When the
    // mathematical value exceeds size_t, aboveRange distinguishes it from an
    // invalid keyword shape so min constraints can still be evaluated exactly.
    bool schemaSize(const pjson& value, size_t& result, bool& aboveRange) {
        aboveRange = false;
        if (value.isInt()) {
            const int64_t integer = pjsonImpl::_integer(value);
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
        const double number = pjsonImpl::_floating(value);
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

    // Implements multipleOf from integers or canonical decimal text generated
    // for doubles. Powers of ten are reduced through their prime factors,
    // avoiding fixed-epsilon comparisons.
    bool isExactMultiple(const pjson& value, const pjson& divisor) {
        // JSON Schema requires a strictly positive divisor. Consistent with
        // the library's tolerant handling of malformed schemas, non-positive
        // values are ignored instead of being treated as assertions.
        if (pjsonImpl::_numberAsDouble(divisor) <= 0.0)
            return true;
        if (value.isInt() && divisor.isInt()) {
            const uint64_t d = magnitudeOf(pjsonImpl::_integer(divisor));
            return magnitudeOf(pjsonImpl::_integer(value)) % d == 0;
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

    // Locale-independent character predicates used by schema format parsers.
    bool isAsciiDigit(char ch) {
        return ch >= '0' && ch <= '9';
    }
    bool isAsciiHex(char ch) {
        return isAsciiDigit(ch) || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
    }

    // Parses exactly count decimal digits at offset into a small integer.
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

    // Applies Gregorian leap-year rules.
    bool isLeapYear(int year) {
        return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    }

    // Validates an RFC 3339 full-date, including month-specific day limits.
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

    // Validates an RFC 3339 full-time and permits leap second 60 only when the
    // represented UTC minute is 23:59.
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

    // Validates an RFC 3339 date-time joined by T/t.
    bool validDateTime(const std::string& value) {
        return value.size() > 11 && (value[10] == 'T' || value[10] == 't') &&
               validDate(value.substr(0, 10)) && validTime(value.substr(11));
    }

    // Validates four canonical decimal IPv4 octets with no leading zeroes.
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

    // Counts 16-bit units on one side of ::, optionally accepting a final IPv4
    // address as two units. Empty sides are valid only as compression operands.
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

    // Validates an IPv6 address with at most one compression marker and exactly
    // eight units after expanding it.
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
        // An embedded IPv4 address may appear only as the final component,
        // which is necessarily on the right side when :: compression is used.
        if (!parseIPv6Side(left, false, units) || !parseIPv6Side(right, true, units))
            return false;
        return units < 8;
    }

    // Validates the canonical 8-4-4-4-12 hexadecimal UUID text shape.
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

    // Dispatches supported format assertions. Unknown names are annotations and
    // therefore succeed with known=false, as required by JSON Schema.
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

    // Percent-decodes a same-document URI fragment and accepts it only when the
    // result is empty or has JSON Pointer syntax. Token unescaping happens later.
    bool decodeSchemaFragment(const std::string& fragment, std::string& pointer) {
        pointer.clear();
        for (size_t i = 0; i < fragment.size(); ++i) {
            if (fragment[i] != '%') {
                pointer += fragment[i];
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
            pointer += static_cast<char>((high << 4) | low);
            i += 2;
        }
        return pointer.empty() || pointer[0] == '/';
    }

    // Adds a diagnostic without allowing allocation failure to escape a noexcept API.
    void bestEffortSchemaError(std::vector<SchemaError>& errors, const std::string& path,
                               const std::string& message) noexcept {
        try {
            errors.push_back(SchemaError(path, message));
        } catch (...) { // Best effort: this path must remain noexcept.
            return;
        }
    }

    // Literal-string overload for exception paths that should avoid extra temporaries.
    void bestEffortSchemaError(std::vector<SchemaError>& errors, const char* path,
                               const char* message) noexcept {
        try {
            errors.push_back(SchemaError(path, message));
        } catch (...) { // Best effort: this path must remain noexcept.
            return;
        }
    }

    // Balances the shared recursion counter across every return and exception.
    struct SchemaDepthGuard {
        pjsonImpl::SchemaValidationCtx& ctx;
        size_t levels;
        explicit SchemaDepthGuard(pjsonImpl::SchemaValidationCtx& aCtx)
                : ctx(aCtx)
                , levels(1) {
            ++ctx.depth;
        }
        void enterResolvedReference() {
            ++ctx.depth;
            ++levels;
        }
        ~SchemaDepthGuard() { ctx.depth -= levels; }
    };

    // Keeps every iteratively resolved (instance, schema) pair active until the
    // terminal schema has been evaluated, matching nested-call cycle semantics.
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

    // Aborts all remaining branches and ensures a budget failure reaches the
    // public error vector even when discovered inside combinator scratch errors.
    void failValidationBudget(pjsonImpl::SchemaValidationCtx& ctx,
                              pjsonImpl::SchemaErrorSink& errors, const std::string& path,
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

    size_t validationDepthLimit(const SchemaOptions& options) {
        const size_t requested = options.maxValidationDepth == 0 ? kSchemaValidationDepthHardLimit
                                                                 : options.maxValidationDepth;
        return std::min(requested, kSchemaValidationDepthHardLimit);
    }

    size_t validationRefLimit(const SchemaOptions& options) {
        return options.maxRefResolutions == 0 ? size_t(1024) : options.maxRefResolutions;
    }

    size_t validationWorkLimit(const SchemaOptions& options) {
        return options.maxValidationWork == 0 ? size_t(1000000) : options.maxValidationWork;
    }

    // Charges bounded validation work before potentially expensive traversal.
    bool chargeValidationWork(pjsonImpl::SchemaValidationCtx& ctx,
                              pjsonImpl::SchemaErrorSink& errors, const std::string& path,
                              size_t amount = 1) {
        const size_t limit = validationWorkLimit(ctx.options);
        if (amount > limit - std::min(ctx.workUsed, limit)) {
            failValidationBudget(ctx, errors, path, "schema validation work budget exceeded");
            return false;
        }
        ctx.workUsed += amount;
        return true;
    }

    // Loop-heavy keywords charge separately from recursive schema evaluations.
    bool chargeLoopWork(pjsonImpl::SchemaValidationCtx& ctx, pjsonImpl::SchemaErrorSink& errors,
                        const std::string& path, size_t amount = 1) {
        return chargeValidationWork(ctx, errors, path, amount);
    }

    // Counts Unicode code points, charging the bytes examined. Parsed strings
    // are valid UTF-8; malformed programmatic bytes count individually here.
    bool unicodeLength(const std::string& value, pjsonImpl::SchemaValidationCtx& ctx,
                       pjsonImpl::SchemaErrorSink& errors, const std::string& path, size_t& count) {
        count = 0;
        for (size_t offset = 0; offset < value.size(); ++count) {
            const int bytes = pjsonImpl::_utf8Len(value.data(), offset, value.size());
            const size_t consumed = bytes > 0 ? static_cast<size_t>(bytes) : size_t(1);
            if (!chargeLoopWork(ctx, errors, path, consumed))
                return false;
            offset += consumed;
        }
        return true;
    }

    // Records ordinary keyword failures through one shared per-run quota. The
    // terminal budget diagnostic bypasses this quota via failValidationBudget.
    bool addSchemaError(pjsonImpl::SchemaValidationCtx&, pjsonImpl::SchemaErrorSink& errors,
                        const std::string& path, const std::string& message) {
        errors.push_back(SchemaError(path, message));
        return !errors.ctx.aborted;
    }

    // Applies configured size/complexity gates before ECMAScript regex_search.
    // Policy or syntax failures are validation errors, distinct from no match.
    bool evaluateRegex(const std::string& subject, const std::string& pattern,
                       const std::string& path, pjsonImpl::SchemaErrorSink& errors,
                       pjsonImpl::SchemaValidationCtx& ctx, bool& matches) {
        matches = false;
        if (ctx.options.maxRegexSubjectBytes != 0 &&
            subject.size() > ctx.options.maxRegexSubjectBytes) {
            errors.push_back(
                SchemaError(path, "string exceeds regex safety limit (" +
                                      std::to_string(subject.size()) + " bytes, limit " +
                                      std::to_string(ctx.options.maxRegexSubjectBytes) + ")"));
            return false;
        }

        pjsonImpl::RegexCacheEntry& cached = ctx.regexCache[pattern];
        if (cached.state == pjsonImpl::RegexCacheEntry::Uninitialized) {
            if (!chargeLoopWork(ctx, errors, path, pattern.size() + size_t(1)))
                return false;
            if (ctx.options.maxRegexPatternBytes != 0 &&
                pattern.size() > ctx.options.maxRegexPatternBytes) {
                cached.state = pjsonImpl::RegexCacheEntry::PatternTooLarge;
            } else if (!ctx.options.allowUnsafeRegex && !pjsonImpl::_isSafeRegex(pattern)) {
                cached.state = pjsonImpl::RegexCacheEntry::UnsafePattern;
            } else {
                try {
                    cached.expression.assign(pattern, std::regex::ECMAScript);
                    cached.state = pjsonImpl::RegexCacheEntry::Ready;
                } catch (const std::regex_error&) {
                    cached.state = pjsonImpl::RegexCacheEntry::InvalidPattern;
                }
            }
        }

        if (cached.state == pjsonImpl::RegexCacheEntry::PatternTooLarge) {
            errors.push_back(SchemaError(path, "schema regex pattern exceeds safety limit"));
            return false;
        }
        if (cached.state == pjsonImpl::RegexCacheEntry::UnsafePattern) {
            errors.push_back(SchemaError(path, "schema regex pattern rejected by safety policy"));
            return false;
        }
        if (cached.state == pjsonImpl::RegexCacheEntry::InvalidPattern) {
            errors.push_back(SchemaError(path, "schema has an invalid regex pattern"));
            return false;
        }
        if (!chargeLoopWork(ctx, errors, path, subject.size() + size_t(1)))
            return false;
        matches = std::regex_search(subject, cached.expression);
        return true;
    }
} // namespace
/*static*/
// Schema equality mirrors public structural equality while charging every
// visited value and compared string/key against the validation work budget.
bool pjsonImpl::_equalWithBudget(const pjson& aLeft, const pjson& aRight, SchemaValidationCtx& aCtx,
                                 SchemaErrorSink& aErrors, const std::string& aPath, bool& aEqual) {
    struct Pair {
        const pjson* left;
        const pjson* right;
    };
    std::vector<Pair> work;
    Pair root = {&aLeft, &aRight};
    work.push_back(root);
    aEqual = false;

    while (!work.empty()) {
        if (!chargeLoopWork(aCtx, aErrors, aPath))
            return false;
        const Pair current = work.back();
        work.pop_back();
        const pjson& left = *current.left;
        const pjson& right = *current.right;
        const bool leftNumber = left.isNumber();
        const bool rightNumber = right.isNumber();
        if (leftNumber && rightNumber) {
            if (_compareNumbers(left, right) != 0)
                return true;
            continue;
        }
        if (left._eType != right._eType)
            return true;

        switch (left._eType) {
            case jsonType::jsonNull:
                break;
            case jsonType::jsonString: {
                const size_t bytes = std::max(left._uValue._pValueString->size(),
                                              right._uValue._pValueString->size());
                if (!chargeLoopWork(aCtx, aErrors, aPath, bytes))
                    return false;
                if (*left._uValue._pValueString != *right._uValue._pValueString)
                    return true;
                break;
            }
            case jsonType::jsonNumberInt:
            case jsonType::jsonNumberDouble:
                break; // numeric pairs were handled above
            case jsonType::jsonBoolean:
                if (left._uValue._valueBool != right._uValue._valueBool)
                    return true;
                break;
            case jsonType::jsonArray:
                if (left._uValue._pValueArray->size() != right._uValue._pValueArray->size())
                    return true;
                for (size_t i = 0; i < left._uValue._pValueArray->size(); ++i) {
                    Pair child = {(*left._uValue._pValueArray)[i],
                                  (*right._uValue._pValueArray)[i]};
                    work.push_back(child);
                }
                break;
            case jsonType::jsonObject: {
                if (left._uValue._pValueMap->size() != right._uValue._pValueMap->size())
                    return true;
                ObjectStorage::const_iterator l = left._uValue._pValueMap->begin();
                ObjectStorage::const_iterator r = right._uValue._pValueMap->begin();
                for (; l != left._uValue._pValueMap->end(); ++l, ++r) {
                    if (!chargeLoopWork(aCtx, aErrors, aPath,
                                        std::max(l->first.size(), r->first.size()) + size_t(1)))
                        return false;
                    if (l->first != r->first)
                        return true;
                    Pair child = {l->second, r->second};
                    work.push_back(child);
                }
                break;
            }
        }
    }
    aEqual = true;
    return true;
}
// Validates aNode against aSchema while sharing reference, recursion, and
// failure-budget state across every recursive branch.
/*static*/
bool pjsonImpl::_validateCtx(const pjson& aNode, const pjson& aSchema, const std::string& aPath,
                             SchemaErrorSink& aErrors, SchemaValidationCtx& aCtx) {
    if (aCtx.aborted)
        return false;
    if (!chargeValidationWork(aCtx, aErrors, aPath))
        return false;
    if (aCtx.depth >= validationDepthLimit(aCtx.options)) {
        failValidationBudget(aCtx, aErrors, aPath, "schema validation depth budget exceeded");
        return false;
    }
    SchemaDepthGuard depthGuard(aCtx);
    ActiveRefGuard activeRefGuard(aCtx.activeRefs);
    const pjson* currentSchema = &aSchema;

    // Resolve consecutive local references without consuming native stack.
    // Each hop still behaves like a logical _validateCtx invocation: it charges
    // work, enters the depth budget, and keeps its active pair until the final
    // target has been evaluated. Draft-07 reference objects ignore siblings.
    for (;;) {
        // A boolean schema accepts (true) or rejects (false) everything.
        if (currentSchema->isBool()) {
            if (!pjsonImpl::_boolean(*currentSchema)) {
                aErrors.push_back(SchemaError(aPath, "schema is false; no value is valid here"));
                return false;
            }
            return true;
        }
        // Only object schemas carry keywords; anything else is treated as "accept".
        if (!currentSchema->isObject())
            return true;

        // Draft-07 treats an object containing a string $ref as a reference
        // object: all sibling keywords are ignored. Only same-document fragment
        // references are supported; percent-decoding precedes RFC 6901 decoding.
        const pjson* ref = currentSchema->find("$ref");
        if (ref == nullptr || !ref->isString())
            break;

        const std::string refText = pjsonImpl::_string(*ref);
        if (!refText.empty() && refText[0] != '#') {
            aErrors.push_back(SchemaError(aPath, "non-local $ref is not supported: " + refText));
            return false;
        }
        if (aCtx.refResolutions >= validationRefLimit(aCtx.options)) {
            failValidationBudget(aCtx, aErrors, aPath, "schema $ref resolution budget exceeded");
            return false;
        }
        ++aCtx.refResolutions;

        std::string pointer;
        const std::string fragment = refText.empty() ? std::string() : refText.substr(1);
        if (!decodeSchemaFragment(fragment, pointer)) {
            aErrors.push_back(SchemaError(aPath, "malformed local $ref fragment: " + refText));
            return false;
        }

        pjson::PointerError pointerError;
        const pjson* target = aCtx.rootSchema.findPointer(pointer, pointerError);
        if (target == nullptr) {
            const bool malformed = pointerError.code == pjson::PointerError::InvalidSyntax ||
                                   pointerError.code == pjson::PointerError::InvalidEscape ||
                                   pointerError.code == pjson::PointerError::InvalidArrayIndex ||
                                   pointerError.code == pjson::PointerError::AppendTokenNotAllowed;
            aErrors.push_back(
                SchemaError(aPath, std::string(malformed ? "malformed" : "unresolved") +
                                       " local $ref: " + refText));
            return false;
        }

        const std::pair<const pjson*, const pjson*> active(&aNode, target);
        if (std::find(aCtx.activeRefs.begin(), aCtx.activeRefs.end(), active) !=
            aCtx.activeRefs.end()) {
            aErrors.push_back(SchemaError(aPath, "local $ref cycle detected: " + refText));
            return false;
        }
        activeRefGuard.push(&aNode, target);

        if (!chargeValidationWork(aCtx, aErrors, aPath))
            return false;
        if (aCtx.depth >= validationDepthLimit(aCtx.options)) {
            failValidationBudget(aCtx, aErrors, aPath, "schema validation depth budget exceeded");
            return false;
        }
        depthGuard.enterResolvedReference();
        currentSchema = target;
    }

    const pjson& schema = *currentSchema;
    const size_t before = aErrors.size();

    // ---- type ----
    if (const pjson* t = schema.find("type")) {
        if (t->isString()) {
            if (!_typeMatches(aNode, pjsonImpl::_string(*t))) {
                aErrors.push_back(SchemaError(aPath, "expected type " + pjsonImpl::_string(*t) +
                                                         ", got " + _typeName(aNode)));
            }
        } else if (t->isArray()) {
            bool matched = false;
            std::string names;
            for (const pjson* e : pjsonImpl::_array(*t)) {
                if (!chargeLoopWork(aCtx, aErrors, aPath))
                    return false;
                if (e->isString()) {
                    if (!names.empty())
                        names += ", ";
                    names += pjsonImpl::_string(*e);
                    if (_typeMatches(aNode, pjsonImpl::_string(*e))) {
                        matched = true;
                        break;
                    }
                }
            }
            if (!matched) {
                aErrors.push_back(SchemaError(aPath, "expected one of type [" + names + "], got " +
                                                         _typeName(aNode)));
            }
        }
        // A malformed "type" (neither string nor array) is ignored.
    }

    // ---- const ----
    if (const pjson* cst = schema.find("const")) {
        bool equal = false;
        if (!_equalWithBudget(aNode, *cst, aCtx, aErrors, aPath, equal))
            return false;
        if (!equal) {
            aErrors.push_back(SchemaError(aPath, "value does not equal the required const"));
        }
    }

    // ---- enum ----
    if (const pjson* en = schema.find("enum")) {
        if (en->isArray()) {
            bool found = false;
            for (const pjson* opt : pjsonImpl::_array(*en)) {
                bool equal = false;
                if (!_equalWithBudget(aNode, *opt, aCtx, aErrors, aPath, equal))
                    return false;
                if (equal) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                aErrors.push_back(SchemaError(aPath, "value is not in the allowed enum"));
            }
        }
    }

    // ---- numeric constraints ----
    if (aNode.isNumber()) {
        if (const pjson* m = schema.find("minimum")) {
            if (m->isNumber() && _compareNumbers(aNode, *m) < 0) {
                addSchemaError(aCtx, aErrors, aPath,
                               "value " + formatNumber(aNode) + " is below minimum " +
                                   formatNumber(*m));
            }
        }
        if (const pjson* m = schema.find("maximum")) {
            const int comparison = m->isNumber() ? _compareNumbers(aNode, *m) : 2;
            if (comparison != 2 && comparison > 0) {
                addSchemaError(aCtx, aErrors, aPath,
                               "value " + formatNumber(aNode) + " is above maximum " +
                                   formatNumber(*m));
            }
        }
        if (const pjson* m = schema.find("exclusiveMinimum")) {
            const int comparison = m->isNumber() ? _compareNumbers(aNode, *m) : 2;
            if (comparison <= 0) {
                addSchemaError(aCtx, aErrors, aPath,
                               "value " + formatNumber(aNode) +
                                   " is not greater than exclusiveMinimum " + formatNumber(*m));
            }
        }
        if (const pjson* m = schema.find("exclusiveMaximum")) {
            const int comparison = m->isNumber() ? _compareNumbers(aNode, *m) : 2;
            if (comparison != 2 && comparison >= 0) {
                addSchemaError(aCtx, aErrors, aPath,
                               "value " + formatNumber(aNode) +
                                   " is not less than exclusiveMaximum " + formatNumber(*m));
            }
        }
        if (const pjson* m = schema.find("multipleOf")) {
            if (m->isNumber() && !isExactMultiple(aNode, *m)) {
                addSchemaError(aCtx, aErrors, aPath,
                               "value " + formatNumber(aNode) + " is not a multiple of " +
                                   formatNumber(*m));
            }
        }
    }

    // ---- string constraints ----
    if (aNode.isString()) {
        const std::string& s = *aNode._uValue._pValueString;
        size_t length = 0;
        if (!unicodeLength(s, aCtx, aErrors, aPath, length))
            return false;
        if (const pjson* m = schema.find("minLength")) {
            size_t bound = 0;
            bool aboveRange = false;
            if (schemaSize(*m, bound, aboveRange) && (aboveRange || length < bound))
                addSchemaError(aCtx, aErrors, aPath,
                               "string length " + std::to_string(length) + " is below minLength " +
                                   formatNumber(*m));
        }
        if (const pjson* m = schema.find("maxLength")) {
            size_t bound = 0;
            bool aboveRange = false;
            if (schemaSize(*m, bound, aboveRange) && !aboveRange && length > bound)
                addSchemaError(aCtx, aErrors, aPath,
                               "string length " + std::to_string(length) + " is above maxLength " +
                                   formatNumber(*m));
        }
        if (const pjson* p = schema.find("pattern")) {
            if (p->isString()) {
                const std::string pattern = pjsonImpl::_string(*p);
                bool matches = false;
                if (evaluateRegex(s, pattern, aPath, aErrors, aCtx, matches) && !matches)
                    aErrors.push_back(
                        SchemaError(aPath, "string does not match pattern /" + pattern + "/"));
            }
        }
        if (aCtx.options.validateFormats) {
            if (const pjson* format = schema.find("format")) {
                if (format->isString()) {
                    bool known = false;
                    if (!knownFormatValid(pjsonImpl::_string(*format), s, known) && known)
                        aErrors.push_back(SchemaError(aPath, "string is not a valid " +
                                                                 pjsonImpl::_string(*format) +
                                                                 " format"));
                }
            }
        }
    }

    // ---- array constraints ----
    if (aNode.isArray()) {
        const PJSONARRAY& arr = *aNode._uValue._pValueArray;
        if (const pjson* m = schema.find("minItems")) {
            size_t bound = 0;
            bool aboveRange = false;
            if (schemaSize(*m, bound, aboveRange) && (aboveRange || arr.size() < bound))
                addSchemaError(aCtx, aErrors, aPath,
                               "array has " + std::to_string(arr.size()) +
                                   " items, below minItems " + formatNumber(*m));
        }
        if (const pjson* m = schema.find("maxItems")) {
            size_t bound = 0;
            bool aboveRange = false;
            if (schemaSize(*m, bound, aboveRange) && !aboveRange && arr.size() > bound)
                addSchemaError(aCtx, aErrors, aPath,
                               "array has " + std::to_string(arr.size()) +
                                   " items, above maxItems " + formatNumber(*m));
        }
        if (const pjson* u = schema.find("uniqueItems")) {
            if (u->isBool() && pjsonImpl::_boolean(*u)) {
                bool dup = false;
                for (size_t i = 0; i < arr.size() && !dup; ++i) {
                    for (size_t j = i + 1; j < arr.size(); ++j) {
                        bool equal = false;
                        if (!_equalWithBudget(*arr[i], *arr[j], aCtx, aErrors, aPath, equal))
                            return false;
                        if (equal) {
                            dup = true;
                            break;
                        }
                    }
                }
                if (dup) {
                    aErrors.push_back(SchemaError(aPath, "array items are not unique"));
                }
            }
        }
        if (const pjson* items = schema.find("items")) {
            if (items->isArray()) {
                const PJSONARRAY& tuple = pjsonImpl::_array(*items);
                const size_t count = std::min(arr.size(), tuple.size());
                for (size_t i = 0; i < count && !aCtx.aborted; ++i) {
                    if (!chargeLoopWork(aCtx, aErrors, aPath))
                        return false;
                    _validateCtx(*arr[i], *tuple[i], _pointerAppend(aPath, std::to_string(i)),
                                 aErrors, aCtx);
                }
            } else {
                for (size_t i = 0; i < arr.size() && !aCtx.aborted; ++i) {
                    if (!chargeLoopWork(aCtx, aErrors, aPath))
                        return false;
                    _validateCtx(*arr[i], *items, _pointerAppend(aPath, std::to_string(i)), aErrors,
                                 aCtx);
                }
            }
        }
    }

    // ---- object constraints ----
    if (aNode.isObject()) {
        const PJSONMAP& obj = *aNode._uValue._pValueMap;

        if (const pjson* req = schema.find("required")) {
            if (req->isArray()) {
                for (const pjson* k : pjsonImpl::_array(*req)) {
                    if (!chargeLoopWork(aCtx, aErrors, aPath))
                        return false;
                    if (k->isString() && obj.find(pjsonImpl::_string(*k)) == obj.end()) {
                        aErrors.push_back(SchemaError(aPath, "missing required property \"" +
                                                                 pjsonImpl::_string(*k) + "\""));
                    }
                }
            }
        }
        if (const pjson* m = schema.find("minProperties")) {
            size_t bound = 0;
            bool aboveRange = false;
            if (schemaSize(*m, bound, aboveRange) && (aboveRange || obj.size() < bound))
                addSchemaError(aCtx, aErrors, aPath,
                               "object has " + std::to_string(obj.size()) +
                                   " properties, below minProperties " + formatNumber(*m));
        }
        if (const pjson* m = schema.find("maxProperties")) {
            size_t bound = 0;
            bool aboveRange = false;
            if (schemaSize(*m, bound, aboveRange) && !aboveRange && obj.size() > bound)
                addSchemaError(aCtx, aErrors, aPath,
                               "object has " + std::to_string(obj.size()) +
                                   " properties, above maxProperties " + formatNumber(*m));
        }

        const pjson* props = schema.find("properties");
        if (props && props->isObject()) {
            for (const auto& kv : pjsonImpl::_object(*props)) {
                if (!chargeLoopWork(aCtx, aErrors, aPath))
                    return false;
                auto it = obj.find(kv.first);
                if (it != obj.end()) {
                    _validateCtx(*it->second, *kv.second, _pointerAppend(aPath, kv.first), aErrors,
                                 aCtx);
                }
                if (aCtx.aborted)
                    return false;
            }
        }

        const pjson* patternProps = schema.find("patternProperties");
        // A set avoids the prior O(properties * matches) membership scan when
        // additionalProperties is evaluated after patternProperties.
        std::set<std::string> patternMatched;
        if (patternProps && patternProps->isObject()) {
            for (const auto& patternSchema : pjsonImpl::_object(*patternProps)) {
                for (const auto& kv : obj) {
                    if (!chargeLoopWork(aCtx, aErrors, aPath))
                        return false;
                    bool matches = false;
                    if (evaluateRegex(kv.first, patternSchema.first,
                                      _pointerAppend(aPath, kv.first), aErrors, aCtx, matches) &&
                        matches) {
                        patternMatched.insert(kv.first);
                        _validateCtx(*kv.second, *patternSchema.second,
                                     _pointerAppend(aPath, kv.first), aErrors, aCtx);
                    }
                    if (aCtx.aborted)
                        return false;
                }
            }
        }

        if (const pjson* propertyNames = schema.find("propertyNames")) {
            for (const auto& kv : obj) {
                if (!chargeLoopWork(aCtx, aErrors, aPath))
                    return false;
                pjson propertyName;
                propertyName = kv.first;
                _validateCtx(propertyName, *propertyNames, _pointerAppend(aPath, kv.first), aErrors,
                             aCtx);
                if (aCtx.aborted)
                    return false;
            }
        }

        const pjson* dependentRequired = schema.find("dependentRequired");
        if (dependentRequired && dependentRequired->isObject()) {
            for (const auto& dependency : pjsonImpl::_object(*dependentRequired)) {
                if (!chargeLoopWork(aCtx, aErrors, aPath))
                    return false;
                if (obj.find(dependency.first) == obj.end() || !dependency.second->isArray())
                    continue;
                for (const pjson* required : pjsonImpl::_array(*dependency.second)) {
                    if (!chargeLoopWork(aCtx, aErrors, aPath))
                        return false;
                    if (required->isString() &&
                        obj.find(pjsonImpl::_string(*required)) == obj.end()) {
                        aErrors.push_back(SchemaError(
                            aPath, "property \"" + dependency.first + "\" requires property \"" +
                                       pjsonImpl::_string(*required) + "\""));
                    }
                }
            }
        }

        const pjson* dependencies = schema.find("dependencies");
        if (dependencies && dependencies->isObject()) {
            for (const auto& dependency : pjsonImpl::_object(*dependencies)) {
                if (!chargeLoopWork(aCtx, aErrors, aPath))
                    return false;
                if (obj.find(dependency.first) == obj.end())
                    continue;
                if (dependency.second->isArray()) {
                    for (const pjson* required : pjsonImpl::_array(*dependency.second)) {
                        if (!chargeLoopWork(aCtx, aErrors, aPath))
                            return false;
                        if (required->isString() &&
                            obj.find(pjsonImpl::_string(*required)) == obj.end()) {
                            aErrors.push_back(SchemaError(aPath, "property \"" + dependency.first +
                                                                     "\" requires property \"" +
                                                                     pjsonImpl::_string(*required) +
                                                                     "\""));
                        }
                    }
                } else {
                    _validateCtx(aNode, *dependency.second, aPath, aErrors, aCtx);
                    if (aCtx.aborted)
                        return false;
                }
            }
        }

        if (const pjson* addl = schema.find("additionalProperties")) {
            for (const auto& kv : obj) {
                if (!chargeLoopWork(aCtx, aErrors, aPath))
                    return false;
                const bool declared =
                    props && props->isObject() &&
                    pjsonImpl::_object(*props).find(kv.first) != pjsonImpl::_object(*props).end();
                const bool matched = patternMatched.find(kv.first) != patternMatched.end();
                if (declared || matched)
                    continue;
                if (addl->isBool()) {
                    if (!pjsonImpl::_boolean(*addl)) {
                        aErrors.push_back(
                            SchemaError(_pointerAppend(aPath, kv.first),
                                        "additional property \"" + kv.first + "\" is not allowed"));
                    }
                } else {
                    _validateCtx(*kv.second, *addl, _pointerAppend(aPath, kv.first), aErrors, aCtx);
                }
                if (aCtx.aborted)
                    return false;
            }
        }
    }

    // ---- logical combinators ----
    // allOf contributes each branch's concrete errors. anyOf, oneOf, and not
    // are speculative: branches validate into scratch vectors so only the
    // combinator-level outcome is exposed to callers. Budget aborts bypass that
    // isolation through failValidationBudget and stop all remaining work.
    if (const pjson* allOf = schema.find("allOf")) {
        if (allOf->isArray()) {
            for (const pjson* sub : pjsonImpl::_array(*allOf)) {
                if (!chargeLoopWork(aCtx, aErrors, aPath))
                    return false;
                _validateCtx(aNode, *sub, aPath, aErrors, aCtx);
                if (aCtx.aborted)
                    return false;
            }
        }
    }
    if (const pjson* anyOf = schema.find("anyOf")) {
        if (anyOf->isArray()) {
            bool any = false;
            for (const pjson* sub : pjsonImpl::_array(*anyOf)) {
                if (!chargeLoopWork(aCtx, aErrors, aPath))
                    return false;
                std::vector<SchemaError> scratch;
                SchemaErrorSink scratchSink(scratch, aCtx, false);
                if (_validateCtx(aNode, *sub, aPath, scratchSink, aCtx)) {
                    any = true;
                    break;
                }
                if (aCtx.aborted)
                    return false;
            }
            if (!any) {
                aErrors.push_back(SchemaError(aPath, "value does not match any schema in anyOf"));
            }
        }
    }
    if (const pjson* oneOf = schema.find("oneOf")) {
        if (oneOf->isArray()) {
            int matches = 0;
            for (const pjson* sub : pjsonImpl::_array(*oneOf)) {
                if (!chargeLoopWork(aCtx, aErrors, aPath))
                    return false;
                std::vector<SchemaError> scratch;
                SchemaErrorSink scratchSink(scratch, aCtx, false);
                if (_validateCtx(aNode, *sub, aPath, scratchSink, aCtx))
                    ++matches;
                if (aCtx.aborted)
                    return false;
            }
            if (matches != 1) {
                aErrors.push_back(SchemaError(aPath, "value matched " + std::to_string(matches) +
                                                         " schemas in oneOf (exactly 1 required)"));
            }
        }
    }
    const pjson* nots = schema.find("not");
    if (nots != nullptr && (nots->isBool() || nots->isObject())) {
        std::vector<SchemaError> scratch;
        SchemaErrorSink scratchSink(scratch, aCtx, false);
        if (_validateCtx(aNode, *nots, aPath, scratchSink, aCtx)) {
            aErrors.push_back(SchemaError(aPath, "value must not match the \"not\" schema"));
        }
        if (aCtx.aborted)
            return false;
    }

    return !aCtx.aborted && aErrors.size() == before;
}
/*static*/
// Runs one noexcept validation session. Unexpected failures become best-effort
// diagnostics rather than escaping across the public API boundary.
bool pjsonImpl::_validate(const pjson& aNode, const pjson& aSchema, const std::string& aPath,
                          std::vector<SchemaError>& aErrors, const SchemaOptions& aOpts) noexcept {
    try {
        SchemaValidationCtx ctx(aSchema, aOpts, &aErrors);
        SchemaErrorSink sink(aErrors, ctx);
        return _validateCtx(aNode, aSchema, aPath, sink, ctx);
    } catch (const SchemaBudgetExceeded&) {
        return false;
    } catch (const std::bad_alloc&) {
        bestEffortSchemaError(aErrors, aPath.c_str(), "schema validation ran out of memory");
    } catch (const std::exception&) {
        bestEffortSchemaError(aErrors, aPath.c_str(),
                              "schema validation failed with an internal exception");
    } catch (...) {
        bestEffortSchemaError(aErrors, aPath.c_str(),
                              "schema validation failed with an unknown exception");
    }
    return false;
}
// Validates and returns only the aggregate result, discarding diagnostics.
bool pjson::validate(const pjson& aSchema, const SchemaOptions& aOpts) const noexcept {
    std::vector<SchemaError> errors;
    return pjsonImpl::_validate(*this, aSchema, "", errors, aOpts);
}
// Validates from the root path and appends diagnostics to aErrors.
bool pjson::validate(const pjson& aSchema, std::vector<SchemaError>& aErrors,
                     const SchemaOptions& aOpts) const noexcept {
    return pjsonImpl::_validate(*this, aSchema, "", aErrors, aOpts);
}
