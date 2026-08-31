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
// pjson — Praveen's JSON: an ultra-simple JSON value type for C++.
//
// A single class, ByteDance::pjson, represents any JSON value and offers an
// ergonomic obj["key"][i] = value building style plus parsing, serialization,
// lookup, mutation, equality, and JSON-Schema-subset validation. All method
// bodies live in pjson.cpp; this header only declares the interface.
//
// Author: Praveen Babu J D
// License: Apache 2.0
//
#ifndef PRAVEENJSON_H
#define PRAVEENJSON_H

// Library version. PJSON_VERSION is the string form ("MAJOR.MINOR.PATCH");
// the numeric parts allow compile-time checks, e.g.
//   #if PJSON_VERSION_MAJOR >= 1
#define PJSON_VERSION_MAJOR 1
#define PJSON_VERSION_MINOR 0
#define PJSON_VERSION_PATCH 0
#define PJSON_VERSION "1.0.0"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace ByteDance {
    struct pjsonImpl;
    //==[Interface]============================================================
    /// Owning, mutable JSON value with deep-copy semantics.
    ///
    /// Child pointers and string views exposed by lookup/access APIs are borrowed
    /// from the owning tree. They become invalid when the child or an ancestor is
    /// destroyed, replaced, reset, erased, moved, swapped, cleared, or successfully
    /// patched. Unless an operation is `noexcept` or explicitly reports failures,
    /// allocation and standard-library exceptions may escape.
    class pjson {
    public:
        //== Library version =================================================
        /// Returns the process-lifetime semantic-version string for this library.
        static const char* getVersion();

        //== Types ===========================================================

        // JSON value kind. Numbers are stored in one of two representations:
        // whole numbers as a 64-bit signed integer (jsonNumberInt) and
        // everything else as a double (jsonNumberDouble).
        enum jsonType : int64_t {
            jsonNull = 0, // stable zero-valued discriminator for the default state
            jsonString,
            jsonNumberInt,
            jsonNumberDouble,
            jsonBoolean,
            jsonArray,  //[ ] array
            jsonObject, // { ... } map
        };

        // Runtime allocator for persistent DOM storage. The allocator is
        // non-owning and must outlive every pjson value that refers to it.
        // Allocation covers pjson child/root nodes plus the std::string,
        // array, and object wrapper objects. Storage used internally by those
        // standard-library objects and transient parser/algorithm scratch space
        // continues to use the standard allocator.
        struct Allocator {
            enum AllocationKind {
                NodeAllocation = 0,
                StringAllocation = 1,
                ArrayAllocation = 2,
                ObjectAllocation = 3
            };

            /// Enables destruction through an Allocator base pointer.
            virtual ~Allocator();
            /// Returns non-null aligned storage or throws; returning null is unsupported.
            virtual void* allocate(size_t aSize, size_t aAlignment, AllocationKind aKind) = 0;
            /// Releases a non-null allocation using its original size, alignment, and kind.
            virtual void deallocate(void* aPtr, size_t aSize, size_t aAlignment,
                                    AllocationKind aKind) noexcept = 0;
        };

        // Stateless ownership for allocator-created nodes. Provenance is read
        // from the node itself, so moving this pointer never transfers or owns
        // the Allocator object.
        struct ValueDeleter {
            /// Destroys aValue's tree through its originating allocator; accepts null.
            void operator()(pjson* aValue) const noexcept;
        };
        typedef std::unique_ptr<pjson, ValueDeleter> unique_ptr;

        // Bounds how much work a parse may do. Parsing always enforces RFC 8259
        // conformance and rejects:
        //   - unknown escapes (e.g. "\q")
        //   - lone/unpaired \u surrogates
        //   - upper/mixed-case keywords (NULL, True, FALSE)
        //   - raw control characters inside strings
        //   - malformed UTF-8 bytes
        struct ParseOptions {
            enum DuplicateKeyPolicy { RejectDuplicateKeys, KeepFirstDuplicate, KeepLastDuplicate };

            int maxDepth;         // nesting limit; values <= 0 enforce a one-level limit
            size_t maxNodes;      // max JSON values created (0 = unlimited)
            size_t maxInputBytes; // max input length in bytes (0 = unlimited)
            DuplicateKeyPolicy duplicateKeys;
            /// Selects duplicate rejection, depth 512, one million nodes, and a
            /// 64 MiB input limit.
            ParseOptions();
        };

        // Filled in by the error-reporting parse() overloads. `ok` is true when
        // parsing succeeded; otherwise `offset` is the zero-based byte position,
        // `line` is one-based, `column` is a one-based byte column, and
        // `message` describes the first failure. Reporting parse APIs reset all
        // fields on entry and leave this success state after a successful parse.
        struct ParseError {
            bool ok;
            size_t offset;
            size_t line;
            size_t column;
            std::string message;
            /// Constructs a success state at the beginning of an input.
            ParseError();
        };

        // Structured JSON Pointer (RFC 6901) lookup failure. `tokenIndex` is
        // zero-based and `token` is the decoded token that could not be
        // resolved (or the source token when its escape sequence is invalid).
        // std::string reporting overloads reset all fields on entry. A C-string
        // overload can report allocation failure before copying the pointer text.
        struct PointerError {
            enum Code {
                Ok,
                InvalidSyntax,
                InvalidEscape,
                MissingTarget,
                ExpectedContainer,
                InvalidArrayIndex,
                ArrayIndexOutOfRange,
                AppendTokenNotAllowed,
                AllocationFailure,
                InternalError
            };

            bool ok;
            Code code;
            std::string pointer;
            size_t tokenIndex;
            std::string token;
            std::string message;
            /// Constructs a successful lookup state with no pointer or token details.
            PointerError();
        };

        // Structured JSON Patch (RFC 6902) / Merge Patch (RFC 7396) failure.
        // Patch application is atomic: failure leaves the target unchanged.
        // Reporting patch APIs reset all fields on entry and on success.
        struct PatchError {
            enum Code {
                Ok,
                InvalidPatchDocument,
                OperationNotObject,
                MissingOp,
                MissingPath,
                MissingFrom,
                MissingValue,
                InvalidOp,
                InvalidPath,
                InvalidFrom,
                TargetMissing,
                InvalidArrayIndex,
                ArrayIndexOutOfRange,
                MoveRootNotAllowed,
                MoveIntoDescendant,
                TestFailed,
                ResourceLimit,
                AllocationFailure,
                InternalError
            };

            bool ok;
            Code code;
            size_t opIndex;
            std::string op;
            std::string path;
            std::string from;
            size_t tokenIndex;
            std::string token;
            std::string message;
            /// Constructs a successful patch state with no operation or token details.
            PatchError();
        };

        // Bounds transactional patch amplification. Zero selects the documented
        // built-in ceiling rather than disabling a safety limit. Clone bytes
        // include node storage plus string and object-key payload bytes.
        struct PatchOptions {
            size_t maxOperations;  // default/hard ceiling: 10,000
            size_t maxClonedNodes; // default/hard ceiling: 1,000,000
            size_t maxClonedBytes; // default/hard ceiling: 64 MiB
            size_t maxWork;        // default/hard ceiling: 1,000,000
            PatchOptions();
        };

        // Controls JSON serialization. The default produces the same compact,
        // ascending-key output as toString()/write() without options. Pretty
        // output places each array element/object member on its own line. Only
        // space and tab are valid indentation characters; any other value is
        // treated as a space so serialization always remains valid JSON.
        //
        // Objects are stored in std::map, so source/insertion order is not
        // available. Key ordering is therefore explicitly ascending or
        // descending according to std::map's bytewise std::string ordering.
        struct SerializeOptions {
            enum KeyOrder { AscendingKeys, DescendingKeys };

            bool pretty;
            size_t indentWidth;
            char indentCharacter;
            bool escapeNonAscii;
            KeyOrder keyOrder;
            size_t maxOutputBytes; // default 64 MiB; zero explicitly means unlimited

            /// Selects compact output, two-space indentation, and ascending keys.
            SerializeOptions();
            /// Returns the defaults with pretty printing enabled.
            static SerializeOptions prettyPrinted();
        };

        // Event sink for non-owning SAX parsing. Return false from any callback
        // to cancel parsing; public parseSax* APIs return false for cancellation
        // or thrown exceptions and populate ParseError when one is supplied.
        //
        // Callbacks are delivered in source order. Duplicate-key policy still
        // applies: RejectDuplicateKeys fails on the duplicate key,
        // KeepFirstDuplicate suppresses later duplicate-value subtrees, and
        // KeepLastDuplicate accepts duplicates while still reporting both
        // occurrences because a streaming SAX walk cannot retract prior events.
        // String and key references are borrowed and remain valid only for the
        // duration of their callback. The handler itself need only outlive the
        // parseSax* call. Default callbacks accept the event and do nothing.
        struct SaxHandler {
            /// Enables destruction through a SaxHandler base pointer.
            virtual ~SaxHandler();
            /// Receives a JSON null value; return false to cancel parsing.
            virtual bool onNull();
            /// Receives a JSON boolean value; return false to cancel parsing.
            virtual bool onBool(bool aValue);
            /// Receives an integer-valued JSON number; return false to cancel parsing.
            virtual bool onInt(int64_t aValue);
            /// Receives a floating-point JSON number; return false to cancel parsing.
            virtual bool onDouble(double aValue);
            /// Receives borrowed decoded string bytes; return false to cancel parsing.
            virtual bool onString(const std::string& aValue);
            /// Marks the beginning of an array; return false to cancel parsing.
            virtual bool onStartArray();
            /// Marks the end of an array; return false to cancel parsing.
            virtual bool onEndArray();
            /// Marks the beginning of an object; return false to cancel parsing.
            virtual bool onStartObject();
            /// Receives a borrowed decoded object key; return false to cancel parsing.
            virtual bool onKey(const std::string& aKey);
            /// Marks the end of an object; return false to cancel parsing.
            virtual bool onEndObject();
        };

        // One schema-validation failure: `path` is a JSON Pointer to the
        // offending node (e.g. "/address/zip", "" for the document root) and
        // `message` explains what was wrong.
        struct SchemaError {
            std::string path;
            std::string message;
            /// Constructs an error with an empty root path and message.
            SchemaError();
            /// Constructs an error for aPath with the supplied diagnostic message.
            SchemaError(const std::string& aPath, const std::string& aMsg);
        };

        // Bounds schema regular-expression work. By default only a conservative,
        // non-ambiguous ECMAScript subset is accepted and both pattern/subject
        // sizes are capped, preventing catastrophic std::regex backtracking.
        // trustedRegex() restores unrestricted ECMAScript regex behavior for
        // schemas and input controlled by the application.
        struct SchemaOptions {
            size_t maxRegexPatternBytes; // 0 = unlimited (default: 256)
            size_t maxRegexSubjectBytes; // 0 = unlimited (default: 4096)
            bool allowUnsafeRegex;       // default false
            /// Recursive validation depth (default and absolute hard ceiling: 128).
            /// Zero selects 128, and larger values are clamped to 128.
            size_t maxValidationDepth;
            /// Resolved references (default 1024); zero selects the hard ceiling of 1024.
            size_t maxRefResolutions;
            /// Validation work units (default 1,000,000); zero selects that hard ceiling.
            size_t maxValidationWork;
            /// Reported errors (default 100); zero selects the hard ceiling of 100.
            size_t maxErrors;
            bool validateFormats; // validate known string formats (default true)
            /// Selects bounded safe-regex, traversal, reference, work, error, and format defaults.
            SchemaOptions();
            /// Disables only regex restrictions; all other defaults remain enabled.
            static SchemaOptions trustedRegex();
        };

        //== Construction / lifetime =========================================
        /// Constructs null using the process-lifetime default allocator.
        pjson();
        /// Constructs null bound to borrowed aAlloc, which must outlive this tree.
        explicit pjson(Allocator& aAlloc) noexcept;
        /// Destroys this value and its complete owned subtree.
        ~pjson();
        /// Deep-copies aFrom using aFrom's borrowed allocator.
        pjson(const pjson& aFrom);
        /// Deep-copies aFrom into borrowed aAlloc.
        pjson(const pjson& aFrom, Allocator& aAlloc);
        /// Transfers aFrom's storage and allocator in O(1), leaving aFrom null.
        pjson(pjson&& aFrom) noexcept;
        /// Transfers in O(1) when allocators match; otherwise deep-copies then clears aFrom.
        pjson(pjson&& aFrom, Allocator& aAlloc);
        /// Deep-copies aFrom while preserving this value's allocator.
        pjson& operator=(const pjson& aFrom);
        /// Moves aFrom while preserving this allocator; cross-allocator moves may allocate.
        pjson& operator=(pjson&& aFrom);
        /// Deep-copies aFrom while preserving this value's allocator.
        void copyFrom(const pjson& aFrom);
        /// Destroys the current contents and becomes null.
        void reset();
        /// Replaces the value with the empty/default value of a valid jsonType.
        void resetTo(jsonType aeType);
        /// Calls resetTo() only when the type differs, otherwise preserving contents.
        void resetIfNeeded(jsonType aeType);
        // Same-allocator swap is O(1). A cross-allocator swap is rejected as a
        // safe no-op; use canSwap() to test before requesting it.
        /// Exchanges contents when allocators match; otherwise does nothing.
        void swap(pjson& aOther) noexcept;
        /// Returns the borrowed allocator bound to this value.
        Allocator& getAllocator() const noexcept;
        /// Returns whether swap(aOther) can exchange contents.
        bool canSwap(const pjson& aOther) const noexcept;

        //== DOM parsing with the default allocator ==========================
        // Each parse accepts exactly one JSON value followed only by whitespace.
        // In-memory parse failures return an empty pointer; diagnostic overloads
        // reset aError and describe the first failure. A byte span may contain
        // embedded NUL bytes, but a null aSrc is always an error.
        /// Parses aStr into an owning tree using the default allocator.
        static pjson::unique_ptr parse(const std::string& aStr,
                                       const ParseOptions& aOpts = ParseOptions());
        /// Parses the aSize-byte span at aSrc using the default allocator.
        static pjson::unique_ptr parse(const char* aSrc, size_t aSize,
                                       const ParseOptions& aOpts = ParseOptions());
        /// Parses aStr and reports the first failure in aError.
        static pjson::unique_ptr parse(const std::string& aStr, ParseError& aError,
                                       const ParseOptions& aOpts = ParseOptions());
        /// Parses the aSize-byte span and reports the first failure in aError.
        static pjson::unique_ptr parse(const char* aSrc, size_t aSize, ParseError& aError,
                                       const ParseOptions& aOpts = ParseOptions());

        // parseStream() buffers the document in chunks while enforcing
        // maxInputBytes. Stream or temporary-buffer exceptions may propagate.
        /// Buffers and parses one document from aIn using the default allocator.
        static pjson::unique_ptr parseStream(std::istream& aIn,
                                             const ParseOptions& aOpts = ParseOptions());
        /// Buffers and parses aIn, reporting ordinary parse/read failures in aError.
        static pjson::unique_ptr parseStream(std::istream& aIn, ParseError& aError,
                                             const ParseOptions& aOpts = ParseOptions());

        //== DOM parsing with a custom allocator =============================
        // Allocator-aware DOM parsing routes root/child nodes and string/array/
        // object wrapper objects through borrowed aAlloc. Standard-container
        // backing buffers still use their standard allocators, as described by
        // Allocator above. aAlloc must outlive the returned tree.
        /// Parses aStr with allocator-backed nodes and wrapper objects.
        static unique_ptr parse(const std::string& aStr, Allocator& aAlloc,
                                const ParseOptions& aOpts = ParseOptions());
        /// Parses a byte span with allocator-backed nodes and wrapper objects.
        static unique_ptr parse(const char* aSrc, size_t aSize, Allocator& aAlloc,
                                const ParseOptions& aOpts = ParseOptions());
        /// Parses aStr with aAlloc and reports the first failure in aError.
        static unique_ptr parse(const std::string& aStr, ParseError& aError, Allocator& aAlloc,
                                const ParseOptions& aOpts = ParseOptions());
        /// Parses a byte span with aAlloc and reports the first failure in aError.
        static unique_ptr parse(const char* aSrc, size_t aSize, ParseError& aError,
                                Allocator& aAlloc, const ParseOptions& aOpts = ParseOptions());
        /// Buffers aIn, then parses with allocator-backed nodes and wrappers.
        static unique_ptr parseStream(std::istream& aIn, Allocator& aAlloc,
                                      const ParseOptions& aOpts = ParseOptions());
        /// Buffers and parses aIn with aAlloc, reporting ordinary failures in aError.
        static unique_ptr parseStream(std::istream& aIn, ParseError& aError, Allocator& aAlloc,
                                      const ParseOptions& aOpts = ParseOptions());

        //== SAX parsing =====================================================
        // SAX parsing retains neither aHandler nor callback arguments. It returns
        // false for invalid input, cancellation, stream failure, or a handler
        // exception; callbacks already delivered before failure are not undone.
        /// Parses aStr and emits its events to aHandler without building a DOM.
        static bool parseSax(const std::string& aStr, SaxHandler& aHandler,
                             const ParseOptions& aOpts = ParseOptions());
        /// Parses the aSize-byte span and emits its events to aHandler.
        static bool parseSax(const char* aSrc, size_t aSize, SaxHandler& aHandler,
                             const ParseOptions& aOpts = ParseOptions());
        /// SAX-parses aStr and reports failure or cancellation in aError.
        static bool parseSax(const std::string& aStr, SaxHandler& aHandler, ParseError& aError,
                             const ParseOptions& aOpts = ParseOptions());
        /// SAX-parses a byte span and reports failure or cancellation in aError.
        static bool parseSax(const char* aSrc, size_t aSize, SaxHandler& aHandler,
                             ParseError& aError, const ParseOptions& aOpts = ParseOptions());
        // True streaming SAX parse: reads the istream incrementally and never
        // buffers the full document in memory.
        /// Incrementally parses aIn and emits events to aHandler.
        static bool parseSaxStream(std::istream& aIn, SaxHandler& aHandler,
                                   const ParseOptions& aOpts = ParseOptions());
        /// Incrementally SAX-parses aIn and reports failure or cancellation in aError.
        static bool parseSaxStream(std::istream& aIn, SaxHandler& aHandler, ParseError& aError,
                                   const ParseOptions& aOpts = ParseOptions());

        //== Serialization ===================================================
        // Non-finite stored doubles serialize as JSON null. Invalid UTF-8 in a
        // string value or object key is a serialization failure: toString() throws,
        // while write() sets failbit (and may propagate stream exceptions).
        // toString() may also throw for allocation or length failure.
        /// Returns compact JSON using the default serialization options.
        std::string toString() const;
        /// Returns JSON serialized according to aOpts.
        std::string toString(const SerializeOptions& aOpts) const;
        /// Writes compact JSON to aOut using the default serialization options.
        void write(std::ostream& aOut) const;
        /// Writes JSON configured by aOpts to aOut.
        void write(std::ostream& aOut, const SerializeOptions& aOpts) const;

        //== Type inspection =================================================
        /// Returns this node's stored JSON representation.
        jsonType getType() const;
        /// Returns whether this node is null.
        bool isNull() const;
        /// Returns whether this node stores a string.
        bool isString() const;
        /// Returns whether this node stores either numeric representation.
        bool isNumber() const;
        /// Returns whether this node stores an integer representation.
        bool isInt() const;
        /// Returns whether this node stores a floating-point representation.
        bool isDouble() const;
        /// Returns whether this node stores a boolean.
        bool isBool() const;
        /// Returns whether this node stores an array.
        bool isArray() const;
        /// Returns whether this node stores an object.
        bool isObject() const;

        // Minimal C++11-compatible, non-owning view of a JSON string. A view
        // aliases bytes owned by this pjson node and is valid only while that
        // node remains alive and unchanged. Assignment, reset, swap, move,
        // destruction, erasing the node, or replacing/resetting an ancestor
        // invalidates it. Strings may contain embedded NUL bytes; use size()
        // rather than strlen().
        class StringView {
        public:
            /// Constructs an empty view with data() == nullptr.
            StringView() noexcept;
            /// Returns the borrowed first byte; a default view returns null.
            const char* data() const noexcept;
            /// Returns the number of bytes in the view, including embedded NUL bytes.
            size_t size() const noexcept;
            /// Returns whether size() is zero.
            bool empty() const noexcept;

        private:
            friend class pjson;
            /// Constructs the internal borrowed view used by tryGet().
            StringView(const char* aData, size_t aSize) noexcept;

            const char* _data;
            size_t _size;
        };

        // Strict typed access to this node. On a type mismatch, returns false
        // and leaves aResult unchanged. Integers may widen to double; no other
        // coercions are performed. StringView avoids a string copy.
        /// Extracts an integer only when this node stores jsonNumberInt.
        bool tryGet(int64_t& aResult) const noexcept;
        /// Extracts a numeric value, widening a stored integer when necessary.
        bool tryGet(double& aResult) const noexcept;
        /// Extracts a boolean only when this node stores jsonBoolean.
        bool tryGet(bool& aResult) const noexcept;
        /// Copies a string only when this node stores jsonString.
        bool tryGet(std::string& aResult) const;
        /// Borrows a string view only when this node stores jsonString.
        bool tryGet(StringView& aResult) const noexcept;

        //== Container queries ===============================================
        /// Returns the element/member count for containers, or zero for scalars.
        size_t size() const;
        /// Returns whether size() is zero; consequently all scalar values are empty.
        bool empty() const;
        /// Empties a container without changing its type, or resets a scalar to null.
        void clear();

        /// Returns copied object keys in std::map order, or an empty vector otherwise.
        std::vector<std::string> keys() const;

        //== Non-mutating lookup =============================================
        /// Returns whether this object contains aKey.
        bool hasKey(const std::string& aKey) const;
        /// Returns whether this object contains non-null aKey; null returns false.
        bool hasKey(const char* aKey) const;
        /// Returns whether this array contains aIndex; negative indexes count from the end.
        bool hasIndex(int aIndex) const noexcept;

        // Returns a pointer to the child stored under aKey, or nullptr when
        // this is not a map or the key is absent. Unlike operator[], this
        // never creates or mutates anything.
        /// Returns the borrowed child at aKey, or null when absent or not an object.
        pjson* find(const std::string& aKey);
        /// Returns the borrowed child at non-null aKey, or null on failure.
        pjson* find(const char* aKey);
        /// Returns the read-only borrowed child at aKey, or null on failure.
        const pjson* find(const std::string& aKey) const;
        /// Returns the read-only borrowed child at non-null aKey, or null on failure.
        const pjson* find(const char* aKey) const;

        // Non-vivifying array lookup. Negative indexes count from the end
        // (-1 is the last element); indexes outside the array return nullptr.
        // These overloads never change this node or its size.
        /// Returns the borrowed array child at aIndex, or null on failure.
        pjson* find(int aIndex) noexcept;
        /// Returns the read-only borrowed array child at aIndex, or null on failure.
        const pjson* find(int aIndex) const noexcept;

        // RFC 6901 lookup. The empty pointer addresses this value; every
        // non-empty pointer must begin with '/'. Lookups are iterative and
        // never create missing nodes. The '-' token is not a lookup index.
        /// Escapes one decoded reference token for inclusion in a JSON Pointer.
        static std::string escapePointerToken(const std::string& aToken);
        /// Resolves aPointer and returns the borrowed target, or null on failure.
        pjson* findPointer(const std::string& aPointer);
        /// Resolves aPointer and returns the read-only borrowed target, or null.
        const pjson* findPointer(const std::string& aPointer) const;
        /// Resolves aPointer and reports lookup failure in aError.
        pjson* findPointer(const std::string& aPointer, PointerError& aError);
        /// Resolves aPointer read-only and reports lookup failure in aError.
        const pjson* findPointer(const std::string& aPointer, PointerError& aError) const;
        /// Resolves a non-null pointer string; null returns failure.
        pjson* findPointer(const char* aPointer);
        /// Resolves a non-null pointer string read-only; null returns failure.
        const pjson* findPointer(const char* aPointer) const;
        /// Resolves a non-null pointer string and reports failure in aError.
        pjson* findPointer(const char* aPointer, PointerError& aError);
        /// Resolves a non-null pointer string read-only and reports failure in aError.
        const pjson* findPointer(const char* aPointer, PointerError& aError) const;

        // Strict typed child access layered on find() and node-level tryGet().
        // Missing/null keys, invalid indexes, and type mismatches leave aResult
        // unchanged. Negative indexes count from the end.
        /// Extracts the integer child at aKey without mutating this object.
        bool tryGet(const std::string& aKey, int64_t& aResult) const;
        /// Extracts the numeric child at aKey as a double.
        bool tryGet(const std::string& aKey, double& aResult) const;
        /// Extracts the boolean child at aKey.
        bool tryGet(const std::string& aKey, bool& aResult) const;
        /// Copies the string child at aKey.
        bool tryGet(const std::string& aKey, std::string& aResult) const;
        /// Borrows a view of the string child at aKey.
        bool tryGet(const std::string& aKey, StringView& aResult) const;

        /// Extracts the integer child at non-null aKey.
        bool tryGet(const char* aKey, int64_t& aResult) const;
        /// Extracts the numeric child at non-null aKey as a double.
        bool tryGet(const char* aKey, double& aResult) const;
        /// Extracts the boolean child at non-null aKey.
        bool tryGet(const char* aKey, bool& aResult) const;
        /// Copies the string child at non-null aKey.
        bool tryGet(const char* aKey, std::string& aResult) const;
        /// Borrows a view of the string child at non-null aKey.
        bool tryGet(const char* aKey, StringView& aResult) const;

        /// Extracts the integer array child at aIndex.
        bool tryGet(int aIndex, int64_t& aResult) const noexcept;
        /// Extracts the numeric array child at aIndex as a double.
        bool tryGet(int aIndex, double& aResult) const noexcept;
        /// Extracts the boolean array child at aIndex.
        bool tryGet(int aIndex, bool& aResult) const noexcept;
        /// Copies the string array child at aIndex.
        bool tryGet(int aIndex, std::string& aResult) const;
        /// Borrows a view of the string array child at aIndex.
        bool tryGet(int aIndex, StringView& aResult) const noexcept;

        //== Building / mutable access =======================================
        // operator[] is a direct builder API. A key access changes a non-object
        // into an object and creates a missing null child. An index access changes
        // a non-array into an array; negative indexes count from the end and clamp
        // before the beginning to zero, while indexes past the end grow the array
        // with null children. A single access that would create more than one
        // million children throws std::length_error before mutation. Use
        // find()/tryGet() for reads.
        /// Returns or creates the child at aString.
        pjson& operator[](const std::string& aString);
        /// Returns or creates the child at aSkey; throws std::invalid_argument for null.
        pjson& operator[](const char* aSkey);
        /// Returns or creates the child at index under the auto-growth rules above.
        pjson& operator[](int index);

        // Assign a scalar value, replacing whatever this node was. Numbers are
        // stored as int64_t (integers) or double (floating point).
        /// Replaces this value with a copy of aString.
        pjson& operator=(const std::string& aString);
        /// Replaces this value with aCString; throws std::invalid_argument for null.
        pjson& operator=(const char* aCString);
        /// Replaces this value with aBool.
        pjson& operator=(const bool aBool);
        /// Replaces this value with aInt.
        pjson& operator=(const int64_t aInt);
        /// Replaces this value with aDouble; non-finite values serialize as null.
        pjson& operator=(const double aDouble);

        // Vector assignment atomically replaces this node with an array of copied
        // children allocated through this node's allocator.
        /// Replaces this value with a copied string array.
        pjson& operator=(const std::vector<std::string>& aValueArray);
        /// Replaces this value with a copied boolean array.
        pjson& operator=(const std::vector<bool>& aValueArray);
        /// Replaces this value with a copied integer array.
        pjson& operator=(const std::vector<int64_t>& aValueArray);
        /// Replaces this value with a copied double array.
        pjson& operator=(const std::vector<double>& aValueArray);

        // Scalar append adds one copied child. If this node is not already an
        // array, its previous value is discarded rather than retained.
        /// Appends a copy of aValue as a string child.
        pjson& operator+=(const std::string& aValue);
        /// Appends aValue as a string child; throws std::invalid_argument for null.
        pjson& operator+=(const char* aValue);
        /// Appends aValue as a boolean child.
        pjson& operator+=(const bool aValue);
        /// Appends aValue as an integer child.
        pjson& operator+=(const int64_t aValue);
        /// Appends aValue as a double child.
        pjson& operator+=(const double aValue);

        // Vector append copies every element. A non-array's prior value is
        // discarded; even an empty vector promotes a non-array to an empty array.
        /// Appends every string in aValueArray.
        pjson& operator+=(const std::vector<std::string>& aValueArray);
        /// Appends every boolean in aValueArray.
        pjson& operator+=(const std::vector<bool>& aValueArray);
        /// Appends every integer in aValueArray.
        pjson& operator+=(const std::vector<int64_t>& aValueArray);
        /// Appends every double in aValueArray.
        pjson& operator+=(const std::vector<double>& aValueArray);

        // Remove and free the child under a map key / at an array index.
        // Array indexes are zero-based and erasure shifts later elements left.
        /// Erases aKey and returns whether an object member was removed.
        bool erase(const std::string& aKey);
        /// Erases non-null aKey; null or a non-object returns false.
        bool erase(const char* aKey);
        /// Erases aIndex and returns whether an array element was removed.
        bool erase(size_t aIndex);

        // Applies all RFC 6902 operations to a scratch document and commits
        // only if every operation succeeds. RFC 7396 Merge Patch is likewise
        // atomic and uses an iterative traversal for deeply nested objects.
        // These bool-returning boundaries convert allocation and internal
        // exceptions into PatchError instead of allowing them to escape. Patch
        // input is borrowed and unchanged; successful commit invalidates prior
        // views into the target. Overloads without aError discard diagnostics.
        /// Atomically applies an RFC 6902 patch document.
        bool applyPatch(const pjson& aPatch, const PatchOptions& aOpts = PatchOptions()) noexcept;
        /// Atomically applies RFC 6902 and reports failure details in aError.
        bool applyPatch(const pjson& aPatch, PatchError& aError,
                        const PatchOptions& aOpts = PatchOptions()) noexcept;
        /// Atomically applies an RFC 7396 Merge Patch document.
        bool applyMergePatch(const pjson& aPatch,
                             const PatchOptions& aOpts = PatchOptions()) noexcept;
        /// Atomically applies RFC 7396 and reports failure details in aError.
        bool applyMergePatch(const pjson& aPatch, PatchError& aError,
                             const PatchOptions& aOpts = PatchOptions()) noexcept;

        //== Equality (deep, structural) =====================================
        // Integer and floating nodes compare equal when numerically equal
        // (e.g. 1 == 1.0). Arrays compare element-wise in order; objects
        // compare by key/value regardless of insertion order.
        /// Returns whether this value and aOther are structurally equal.
        bool operator==(const pjson& aOther) const;
        /// Returns the negation of operator==.
        bool operator!=(const pjson& aOther) const;

        //== Schema validation ===============================================
        // Validates this value against a schema that is itself a pjson object,
        // using the documented JSON Schema subset; this is not a complete draft
        // implementation. Returns true when the
        // value conforms. Never throws. The second form appends reported
        // keyword failures rather than stopping at the first. Errors inside
        // non-selected anyOf/oneOf/not branches are intentionally suppressed,
        // and a resource-budget failure can stop further validation.
        //
        // Supported keywords:
        //   type, enum, const,
        //   $ref (local JSON Pointer fragments),
        //   properties, patternProperties, propertyNames, required,
        //     dependentRequired, dependencies, additionalProperties,
        //     minProperties, maxProperties,
        //   items, minItems, maxItems, uniqueItems,
        //   minimum, maximum, exclusiveMinimum, exclusiveMaximum, multipleOf,
        //   minLength, maxLength, pattern, format,
        //   allOf, anyOf, oneOf, not.
        // A boolean schema (true/false) accepts/rejects everything. Unknown
        // keywords and unsupported keyword shapes are ignored. Both inputs are
        // borrowed and unchanged; the collecting overload appends to aErrors
        // without clearing existing entries. Resource aborts may stop collection.
        /// Returns whether this value satisfies aSchema under aOpts.
        bool validate(const pjson& aSchema,
                      const SchemaOptions& aOpts = SchemaOptions()) const noexcept;
        /// Validates and appends discovered failures to aErrors.
        bool validate(const pjson& aSchema, std::vector<SchemaError>& aErrors,
                      const SchemaOptions& aOpts = SchemaOptions()) const noexcept;

    private:
        //== Internal helpers ================================================
        // The parser, schema validator, and encoding routines live entirely in
        // pjson.cpp as the pjsonImpl helper struct, so this header stays small.
        // pjsonImpl is a friend so it can touch the data union directly; only
        // the few instance helpers other members call are declared here.
        friend struct pjsonImpl;
        friend struct ValueDeleter;

        /// Iteratively deep-copies aFrom's contents using this node's allocator.
        void copyContentsFrom(const pjson& aFrom);

        //== Data ============================================================
        typedef std::vector<pjson*> ArrayStorage;
        typedef std::map<std::string, pjson*> ObjectStorage;

        Allocator* _allocator;
        bool _allocatorOwnedNode;
        // Intrusive scratch link used only by allocation-free iterative tree
        // destruction. It is null during normal object lifetime.
        pjson* _disposeNext;
        jsonType _eType = jsonType::jsonNull;
        union Storage {
            void* _pValueRaw;
            ObjectStorage* _pValueMap;
            ArrayStorage* _pValueArray;
            int64_t _valueInt;
            double _valueDouble;
            bool _valueBool;
            std::string* _pValueString;

            /// Initializes the raw representation to null.
            Storage();
        } _uValue;
    };
    //========================================================================
}; // end namespace ByteDance
#endif /* !PRAVEENJSON_H */
