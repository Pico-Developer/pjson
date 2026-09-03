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
// ergonomic obj["key"][i] = value building style plus serialization, lookup,
// mutation, and equality. Parsing lives in the separate ByteDance::pJsonParser
// helper declared by <pjson_parser.h>; the DOM does not depend on that parser.
// JSON Schema validation similarly lives in ByteDance::pJsonSchemaValidator in
// <pjson_schema.h>, which consumes only the public DOM API.
//
// Author: Praveen Babu J D
// License: Apache 2.0
//
#ifndef PRAVEENJSON_H
#define PRAVEENJSON_H

// Library version. PJSON_VERSION is the string form ("MAJOR.MINOR.PATCH");
// the numeric parts allow compile-time checks, e.g.
//   #if PJSON_VERSION_MAJOR >= 1
#define PJSON_VERSION_MAJOR 3
#define PJSON_VERSION_MINOR 0
#define PJSON_VERSION_PATCH 0
#define PJSON_VERSION "3.0.0"
#define PJSON_ABI_VERSION 3

#if defined(_WIN32) && defined(PJSON_SHARED)
#if defined(PJSON_BUILDING_LIBRARY)
#define PJSON_API __declspec(dllexport)
#else
#define PJSON_API __declspec(dllimport)
#endif
#elif defined(PJSON_BUILDING_LIBRARY) && defined(__GNUC__)
#define PJSON_API __attribute__((visibility("default")))
#else
#define PJSON_API
#endif

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
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
    class PJSON_API pjson {
    public:
        //== Library version =================================================
        /// Returns the process-lifetime semantic-version string for this library.
        static const char* getVersion();

        //== Types ===========================================================

        /// JSON value kind.
        ///
        /// Numbers are stored in one of three representations: signed whole
        /// numbers as a 64-bit signed integer (jsonNumberInt), explicitly
        /// unsigned whole numbers as a 64-bit unsigned integer (jsonNumberUInt),
        /// and fractional/exponent values as a double (jsonNumberDouble).
        enum jsonType : int64_t {
            jsonNull = 0,     ///< JSON null and the stable default discriminator.
            jsonString,       ///< UTF-8 string value.
            jsonNumberInt,    ///< Signed 64-bit integer value.
            jsonNumberDouble, ///< Binary64 number value.
            jsonBoolean,      ///< Boolean value.
            jsonArray,        ///< Ordered array value.
            jsonObject,       ///< Key-sorted object value.
            jsonNumberUInt,   ///< Unsigned 64-bit integer value.
        };

        /// Runtime allocator for persistent DOM storage.
        ///
        /// The allocator is non-owning and must outlive every pjson value that
        /// refers to it. Allocation covers pjson child/root nodes, non-null
        /// private implementations, and the std::string, array, and object
        /// wrapper objects. Storage used internally by those standard-library
        /// objects and transient parser/algorithm scratch space continues to use
        /// the standard allocator.
        struct PJSON_API Allocator {
            /// Identifies the purpose and matching deallocation contract of storage.
            enum AllocationKind {
                NodeAllocation = 0,          ///< Storage for a child or allocator-created root.
                StringAllocation = 1,        ///< Storage for a std::string wrapper.
                ArrayAllocation = 2,         ///< Storage for an array-container wrapper.
                ObjectAllocation = 3,        ///< Storage for an object-container wrapper.
                ImplementationAllocation = 4 ///< Opaque state of one non-null pjson value.
            };

            /// Enables destruction through an Allocator base pointer.
            virtual ~Allocator();
            /// Returns non-null aligned storage or throws; returning null is unsupported.
            virtual void* allocate(size_t aSize, size_t aAlignment, AllocationKind aKind) = 0;
            /// Releases a non-null allocation using its original size, alignment, and kind.
            virtual void deallocate(void* aPtr, size_t aSize, size_t aAlignment,
                                    AllocationKind aKind) noexcept = 0;
        };

        /// Structured JSON Pointer (RFC 6901) lookup failure.
        ///
        /// `tokenIndex` is zero-based and `token` is the decoded token that could
        /// not be resolved (or the source token when its escape sequence is
        /// invalid). std::string reporting overloads reset all fields on entry. A
        /// C-string overload can report allocation failure before copying the
        /// pointer text.
        struct PJSON_API PointerError {
            /// Stable categories for programmatic JSON Pointer failure handling.
            enum Code {
                Ok,                    ///< Lookup succeeded.
                InvalidSyntax,         ///< The pointer does not begin with slash or is malformed.
                InvalidEscape,         ///< A token contains an invalid tilde escape.
                MissingTarget,         ///< An object member or array element does not exist.
                ExpectedContainer,     ///< Traversal encountered a scalar before the final token.
                InvalidArrayIndex,     ///< An array token is not a canonical non-negative index.
                ArrayIndexOutOfRange,  ///< An array token exceeds the current array bounds.
                AppendTokenNotAllowed, ///< The append token is invalid for lookup.
                AllocationFailure,     ///< Diagnostic construction could not allocate.
                InternalError          ///< An unexpected internal exception was contained.
            };

            bool ok;             ///< True exactly when code is Ok.
            Code code;           ///< Stable machine-readable result category.
            std::string pointer; ///< Pointer text supplied by the caller.
            size_t tokenIndex;   ///< Zero-based index of the failing token.
            std::string token;   ///< Decoded failing token when available.
            std::string message; ///< Human-readable diagnostic; wording is not stable.
            /// Constructs a successful lookup state with no pointer or token details.
            PointerError();
        };

        /// Structured JSON Patch (RFC 6902) / Merge Patch (RFC 7396) failure.
        ///
        /// Patch application is atomic: failure leaves the target unchanged.
        /// Reporting patch APIs reset all fields on entry and on success.
        struct PJSON_API PatchError {
            /// Stable categories for programmatic JSON Patch failure handling.
            enum Code {
                Ok,                   ///< Patch application succeeded.
                InvalidPatchDocument, ///< The patch document is not an array.
                OperationNotObject,   ///< An operation entry is not an object.
                MissingOp,            ///< An operation lacks a string `op` member.
                MissingPath,          ///< An operation lacks a string `path` member.
                MissingFrom,          ///< A copy or move lacks a string `from` member.
                MissingValue,         ///< An add, replace, or test lacks `value`.
                InvalidOp,            ///< The operation name is unsupported.
                InvalidPath,          ///< The destination JSON Pointer is invalid.
                InvalidFrom,          ///< The source JSON Pointer is invalid.
                TargetMissing,        ///< A required source, parent, or target is absent.
                InvalidArrayIndex,    ///< An array path token is not a valid index.
                ArrayIndexOutOfRange, ///< An array path token exceeds valid bounds.
                MoveRootNotAllowed,   ///< A move attempts to remove the document root.
                MoveIntoDescendant,   ///< A move destination lies under its source.
                TestFailed,           ///< A test operation did not compare equal.
                ResourceLimit,        ///< A configured patch-work limit was reached.
                AllocationFailure,    ///< Patch application could not allocate.
                InternalError         ///< An unexpected internal exception was contained.
            };

            bool ok;             ///< True exactly when code is Ok.
            Code code;           ///< Stable machine-readable result category.
            size_t opIndex;      ///< Zero-based index of the failing operation.
            std::string op;      ///< Operation name when available.
            std::string path;    ///< Destination pointer when available.
            std::string from;    ///< Source pointer when available.
            size_t tokenIndex;   ///< Zero-based index of the failing pointer token.
            std::string token;   ///< Decoded failing pointer token when available.
            std::string message; ///< Human-readable diagnostic; wording is not stable.
            /// Constructs a successful patch state with no operation or token details.
            PatchError();
        };

        /// Bounds transactional patch amplification.
        ///
        /// Zero selects the documented built-in ceiling rather than disabling a
        /// safety limit. Clone bytes include node storage plus string and
        /// object-key payload bytes.
        struct PJSON_API PatchOptions {
            size_t maxOperations;  ///< Operation ceiling; zero selects the hard 10,000 limit.
            size_t maxClonedNodes; ///< Clone-node ceiling; zero selects the hard 1,000,000 limit.
            size_t maxClonedBytes; ///< Clone-byte ceiling; zero selects the hard 64 MiB limit.
            size_t maxWork;        ///< Work ceiling; zero selects the hard 1,000,000 limit.
            /// Selects the documented finite default limits.
            PatchOptions();
        };

        /// Controls JSON serialization.
        ///
        /// The default produces the same compact, ascending-key output as
        /// toString()/write() without options. Pretty output places each array
        /// element/object member on its own line. Only space and tab are valid
        /// indentation characters; any other value is treated as a space so
        /// serialization always remains valid JSON.
        ///
        /// Objects are stored in std::map, so source/insertion order is not
        /// available. Key ordering is therefore explicitly ascending or
        /// descending according to std::map's bytewise std::string ordering.
        struct PJSON_API SerializeOptions {
            /// Selects ascending or descending deterministic object-key order.
            enum KeyOrder {
                AscendingKeys, ///< Emit keys in ascending std::map order.
                DescendingKeys ///< Emit keys in descending std::map order.
            };

            // Governs how a stored non-finite double (NaN, +/-infinity) is
            // serialized. JSON has no non-finite literal, so the default fails
            // with a structured error rather than silently changing the value's
            // type. NonFiniteToNull opts in to the legacy behavior of writing
            // JSON null; NonFiniteToString writes the strings "NaN",
            // "Infinity", and "-Infinity" for interoperability with permissive
            // consumers. The chosen policy applies identically to compact,
            // pretty, buffered, and streaming output.
            /// Selects how stored NaN and infinity values are represented.
            enum NonFinitePolicy {
                RejectNonFinite,  ///< Fail because JSON has no non-finite number literal.
                NonFiniteToNull,  ///< Emit non-finite values as JSON null.
                NonFiniteToString ///< Emit "NaN" or signed "Infinity" strings.
            };

            bool pretty;               ///< Enables line breaks and indentation.
            size_t indentWidth;        ///< Indentation characters per nesting level.
            char indentCharacter;      ///< Space or tab; invalid values are treated as space.
            bool escapeNonAscii;       ///< Emits non-ASCII code points as Unicode escapes.
            KeyOrder keyOrder;         ///< Deterministic object-key ordering.
            NonFinitePolicy nonFinite; ///< Policy for NaN and infinity values.
            size_t maxOutputBytes;     ///< Output ceiling; zero explicitly means unlimited.

            /// Selects compact output, two-space indentation, ascending keys,
            /// and non-finite rejection.
            SerializeOptions();
            /// Returns the defaults with pretty printing enabled.
            static SerializeOptions prettyPrinted();
        };

        /// Structured outcome for the non-throwing serialization APIs.
        struct PJSON_API SerializeError {
            /// Stable categories for programmatic serialization failure handling.
            enum Code {
                None,              ///< Serialization succeeded.
                InvalidUtf8,       ///< A stored string or object key is not valid UTF-8.
                NonFiniteNumber,   ///< NaN or infinity was rejected by the active policy.
                OutputLimit,       ///< maxOutputBytes or representable output size was exceeded.
                AllocationFailure, ///< Temporary serialization storage could not be allocated.
                StreamFailure,     ///< The destination stream rejected a physical write.
                InternalError      ///< An unexpected internal exception was contained.
            };

            Code code;           ///< Stable machine-readable result category.
            std::string message; ///< Human-readable diagnostic; empty on success.
            /// Constructs a successful serialization result.
            SerializeError();
            /// Resets the result to success.
            void reset() noexcept;
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
        // Same-allocator, non-overlapping swap is O(1). Cross-allocator and
        // ancestor/descendant swaps are rejected as safe no-ops; use canSwap()
        // to test before requesting one.
        /// Exchanges compatible, non-overlapping contents; otherwise does nothing.
        void swap(pjson& aOther) noexcept;
        /// Returns the borrowed allocator bound to this value.
        Allocator& getAllocator() const noexcept;
        /// Returns whether swap(aOther) can exchange contents safely.
        bool canSwap(const pjson& aOther) const noexcept;

        //== Serialization ===================================================
        // Invalid UTF-8 and rejected non-finite doubles are serialization
        // failures: the convenience toString() overloads throw, while legacy
        // write() sets failbit (and may propagate enabled stream exceptions).
        // The SerializeError overloads never throw and expose stable categories.
        /// Returns compact JSON using the default serialization options.
        std::string toString() const;
        /// Returns JSON serialized according to aOpts.
        std::string toString(const SerializeOptions& aOpts) const;
        /// Serializes into aOut transactionally and reports failure without throwing.
        bool toString(std::string& aOut, SerializeError& aError,
                      const SerializeOptions& aOpts = SerializeOptions()) const noexcept;
        /// Writes compact JSON to aOut using the default serialization options.
        void write(std::ostream& aOut) const;
        /// Writes JSON configured by aOpts to aOut.
        void write(std::ostream& aOut, const SerializeOptions& aOpts) const;
        /// Writes JSON and reports logical or physical failure without throwing.
        bool write(std::ostream& aOut, SerializeError& aError,
                   const SerializeOptions& aOpts = SerializeOptions()) const noexcept;

        //== Type inspection =================================================
        /// Returns this node's stored JSON representation.
        jsonType getType() const;
        /// Returns whether this node is null.
        bool isNull() const;
        /// Returns whether this node stores a string.
        bool isString() const;
        /// Returns whether this node stores either numeric representation.
        bool isNumber() const;
        /// Returns whether this node stores a signed-integer representation.
        bool isInt() const;
        /// Returns whether this node stores an unsigned-integer representation.
        bool isUInt() const;
        /// Returns whether this node stores any integer representation (signed or unsigned).
        bool isInteger() const;
        /// Returns whether this node stores a floating-point representation.
        bool isDouble() const;
        /// Returns whether this node stores a boolean.
        bool isBool() const;
        /// Returns whether this node stores an array.
        bool isArray() const;
        /// Returns whether this node stores an object.
        bool isObject() const;

        /// Minimal C++11-compatible, non-owning view of a JSON string.
        ///
        /// A view aliases bytes owned by this pjson node and is valid only while
        /// that node remains alive and unchanged. Assignment, reset, swap, move,
        /// destruction, erasing the node, or replacing/resetting an ancestor
        /// invalidates it. Strings may contain embedded NUL bytes; use size()
        /// rather than strlen().
        class PJSON_API StringView {
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
        // and leaves aResult unchanged. A signed integer read accepts an
        // unsigned value only when it fits in int64_t; an unsigned integer read
        // accepts a signed value only when it is non-negative; a double read
        // widens any integer. No other coercions are performed. StringView
        // avoids a string copy.
        /// Extracts a signed integer; an unsigned value must fit in int64_t.
        bool tryGet(int64_t& aResult) const noexcept;
        /// Extracts an unsigned integer; a signed value must be non-negative.
        bool tryGet(uint64_t& aResult) const noexcept;
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

        //== Non-allocating traversal =======================================
        /// Callback for read-only object-member traversal.
        typedef bool (*ConstMemberVisitor)(StringView aKey, const pjson& aValue, void* aContext);
        /// Callback for mutable object-member traversal.
        typedef bool (*MemberVisitor)(StringView aKey, pjson& aValue, void* aContext);
        /// Callback for read-only array-element traversal.
        typedef bool (*ConstElementVisitor)(const pjson& aValue, void* aContext);
        /// Callback for mutable array-element traversal.
        typedef bool (*ElementVisitor)(pjson& aValue, void* aContext);
        ///
        /// Traversal copies no object names and performs no per-member lookup.
        /// Object members are visited in sorted key order and array elements in
        /// index order. Borrowed arguments are valid only for the callback. The
        /// opaque context is forwarded unchanged. Returning false stops early.
        /// A callback must not resize the traversed container. Calling a traversal
        /// method on the wrong container type is a no-op that returns true.
        /// Visits each object member as (key view, const value); false stops early.
        bool forEachMember(ConstMemberVisitor aVisitor, void* aContext) const;
        /// Visits each object member as (key view, mutable value); false stops early.
        bool forEachMember(MemberVisitor aVisitor, void* aContext);
        /// Visits each array element as a const value; false stops early.
        bool forEachElement(ConstElementVisitor aVisitor, void* aContext) const;
        /// Visits each array element as a mutable value; false stops early.
        bool forEachElement(ElementVisitor aVisitor, void* aContext);

        //== Non-mutating lookup =============================================
        /// Returns whether this object contains aKey.
        bool hasKey(const std::string& aKey) const;
        /// Returns whether this object contains non-null aKey; null returns false.
        bool hasKey(const char* aKey) const;
        /// Alias for hasKey(aKey); reads more naturally at call sites.
        bool contains(const std::string& aKey) const;
        /// Alias for hasKey(aKey); reads more naturally at call sites.
        bool contains(const char* aKey) const;
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
        /// Extracts the unsigned-integer child at aKey without mutating this object.
        bool tryGet(const std::string& aKey, uint64_t& aResult) const;
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
        /// Extracts the unsigned-integer child at non-null aKey.
        bool tryGet(const char* aKey, uint64_t& aResult) const;
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
        /// Extracts the unsigned-integer array child at aIndex.
        bool tryGet(int aIndex, uint64_t& aResult) const noexcept;
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
        // a non-array into an array; valid negative indexes count from the end,
        // while an index before the beginning throws std::out_of_range without
        // mutation. Non-negative indexes past the end grow the array with null
        // children. A single access that would create more than one million
        // children throws std::length_error before mutation. Use find()/tryGet()
        // for reads.
        /// Returns or creates the child at aString.
        pjson& operator[](const std::string& aString);
        /// Returns or creates the child at aSkey; throws std::invalid_argument for null.
        pjson& operator[](const char* aSkey);
        /// Returns or creates the child at index under the auto-growth rules above.
        pjson& operator[](int index);
        /// Returns or creates the child at a non-negative index.
        pjson& operator[](size_t index);

        //== Factories and typed construction ================================
        // Explicit, unambiguous ways to create each JSON kind without relying on
        // default construction having a particular type. Each uses the default
        // allocator; the allocator-aware constructors remain available for
        // custom-allocator trees.
        /// Returns a JSON null value.
        static pjson null();
        /// Returns an empty JSON object value.
        static pjson object();
        /// Returns an empty JSON array value.
        static pjson array();
        /// Replaces this value with JSON null (std::nullptr_t assignment).
        pjson& operator=(std::nullptr_t);

        //== Checked access ==================================================
        // at() is a checked, non-vivifying accessor. Unlike operator[], it never
        // creates a child: a missing object key, an out-of-range index, or a
        // wrong container type throws std::out_of_range.
        /// Returns the existing child at aKey or throws std::out_of_range.
        pjson& at(const std::string& aKey);
        /// Returns the read-only child at aKey or throws std::out_of_range.
        const pjson& at(const std::string& aKey) const;
        /// Returns the existing element at aIndex or throws std::out_of_range.
        pjson& at(size_t aIndex);
        /// Returns the read-only element at aIndex or throws std::out_of_range.
        const pjson& at(size_t aIndex) const;

        //== Generic child insertion ========================================
        // Append or assign arbitrary pjson values, not just scalars. A non-array
        // target is promoted to an array by pushBack. Cross-allocator inserts
        // deep-copy the value into this node's allocator.
        /// Appends a deep copy of aValue, promoting this node to an array.
        pjson& pushBack(const pjson& aValue);
        /// Moves an independent same-allocator value; otherwise deep-copies it.
        pjson& pushBack(pjson&& aValue);
        /// Inserts or replaces the member aKey with a deep copy of aValue.
        pjson& insertOrAssign(const std::string& aKey, const pjson& aValue);
        /// Moves an independent same-allocator value; otherwise deep-copies it.
        pjson& insertOrAssign(const std::string& aKey, pjson&& aValue);
        /// Reserves capacity for at least aCount array elements (no-op for non-arrays
        /// unless this node is first made an array); returns *this for chaining.
        pjson& reserve(size_t aCount);

        // Assign a scalar value, replacing whatever this node was. Numbers are
        // stored as int64_t (signed integers), uint64_t (unsigned integers), or
        // double (floating point). An explicit uint64_t assignment retains the
        // unsigned type identity even for small values.
        /// Replaces this value with a copy of aString.
        pjson& operator=(const std::string& aString);
        /// Replaces this value with aCString; throws std::invalid_argument for null.
        pjson& operator=(const char* aCString);
        /// Replaces this value with aBool.
        pjson& operator=(const bool aBool);
        /// Replaces this value with aInt.
        pjson& operator=(const int64_t aInt);
        /// Replaces this value with an unsigned integer, keeping unsigned identity.
        pjson& operator=(const uint64_t aUInt);
        /// Replaces this value with aDouble; the non-finite policy governs output.
        pjson& operator=(const double aDouble);

        // Vector assignment atomically replaces this node with an array of copied
        // children allocated through this node's allocator.
        /// Replaces this value with a copied string array.
        pjson& operator=(const std::vector<std::string>& aValueArray);
        /// Replaces this value with a copied boolean array.
        pjson& operator=(const std::vector<bool>& aValueArray);
        /// Replaces this value with a copied integer array.
        pjson& operator=(const std::vector<int64_t>& aValueArray);
        /// Replaces this value with a copied unsigned-integer array.
        pjson& operator=(const std::vector<uint64_t>& aValueArray);
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
        /// Appends aValue as an unsigned-integer child.
        pjson& operator+=(const uint64_t aValue);
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
        /// Appends every unsigned integer in aValueArray.
        pjson& operator+=(const std::vector<uint64_t>& aValueArray);
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

        //== Numeric ordering ================================================
        // Exact ordering across the signed, unsigned, and double numeric kinds
        // without rounding an integer through binary64. On success aOrder is set
        // to -1, 0, or 1 for this value being less than, equal to, or greater
        // than aOther. Returns false and leaves aOrder unchanged when either
        // value is not a number, or when the comparison is unordered because a
        // NaN is involved. This is the exact comparison callers (including
        // schema validators) need for numeric bounds; equality alone is exposed
        // through operator==.
        /// Compares two stored numbers exactly; false when non-numeric/unordered.
        bool tryCompareNumber(const pjson& aOther, int& aOrder) const noexcept;

        // NOTE: JSON Schema validation is no longer a member of pjson. It now
        // lives in the standalone ByteDance::pJsonSchemaValidator helper declared
        // in <pjson_schema.h>, which consumes only pjson's public API. This keeps
        // the core DOM free of the schema/regex machinery. The validator carries
        // its own Error and Options vocabulary types.

    private:
        //== Internal helpers ================================================
        // Encoding, ownership, and other DOM operations that need to touch the
        // data members below live behind the pjsonImpl helper. Schema
        // validation is deliberately separate and uses only the public API.
        // pjsonImpl is a friend so it can reach the storage union directly; no
        // instance helper methods are declared here.
        friend struct pjsonImpl;
        // These two pointers are the stable ABI handle. Null values share a
        // private implementation sentinel; non-null state belongs to _allocator.
        Allocator* _allocator;
        pjsonImpl* _pImpl;
    };
    //========================================================================
}; // end namespace ByteDance
#endif /* !PRAVEENJSON_H */
