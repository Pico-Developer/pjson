// SPDX-FileCopyrightText: 2026 ByteDance Ltd. and/or its affiliates
// SPDX-License-Identifier: Apache-2.0
#ifndef PRAVEENJSON_PARSER_H
#define PRAVEENJSON_PARSER_H

#include "pjson.h"

namespace ByteDance {
    /// Configured, reusable JSON parser for DOM and SAX input.
    ///
    /// The parser depends on the pjson DOM, while pjson itself has no parser
    /// dependency. A parser borrows its allocator, owns a copy of its options,
    /// and keeps no mutable per-call state, so it may be reused for many inputs.
    class pJsonParser {
    public:
        /// Bounds parsing work and selects duplicate-key and number policies.
        struct Options {
            /// Controls how repeated object member names are handled.
            enum DuplicateKeyPolicy {
                RejectDuplicateKeys, ///< Fail when a name occurs more than once.
                KeepFirstDuplicate,  ///< Retain the first value and discard later values.
                KeepLastDuplicate    ///< Replace an earlier value with the last value.
            };
            /// Controls numeric tokens that cannot be represented exactly.
            enum NumberPolicy {
                RejectUnrepresentableNumbers, ///< Reject range overflow and nonzero underflow.
                AllowLossyNumbers             ///< Store the nearest finite double when possible.
            };

            int maxDepth;                     ///< Nesting limit; non-positive means one level.
            size_t maxNodes;                  ///< Maximum values processed; zero is unlimited.
            size_t maxInputBytes;             ///< Maximum input bytes; zero is unlimited.
            DuplicateKeyPolicy duplicateKeys; ///< Duplicate object-name policy.
            NumberPolicy numberPolicy;        ///< Unrepresentable-number policy.
            /// Selects strict defaults and bounded parser resources.
            Options();
        };

        /// Structured result for DOM and SAX parsing.
        struct Error {
            /// Stable categories for programmatic parse-failure handling.
            enum Code {
                None = 0,          ///< Parsing succeeded.
                Syntax,            ///< The input violates JSON grammar.
                InvalidEncoding,   ///< UTF-8 or an escaped Unicode value is invalid.
                DuplicateKey,      ///< A repeated object name was rejected.
                NumberRange,       ///< A number is outside the configured representation policy.
                DepthLimit,        ///< Nesting exceeded the effective depth limit.
                InputLimit,        ///< Input exceeded maxInputBytes.
                NodeLimit,         ///< Values processed exceeded maxNodes.
                AllocationFailure, ///< Parser or DOM allocation failed.
                StreamError,       ///< Reading from the input stream failed.
                CallbackError,     ///< A SAX callback cancelled or threw.
                InvalidArgument    ///< The caller supplied an invalid argument.
            };

            bool ok;             ///< True when the most recent parse succeeded.
            Code code;           ///< Stable machine-facing result category.
            size_t offset;       ///< Zero-based input byte offset.
            size_t line;         ///< One-based source line.
            size_t column;       ///< One-based byte column.
            std::string message; ///< Human-readable diagnostic; wording is not stable.
            /// Constructs the successful start-of-input state.
            Error();
        };

        /// Event sink for non-owning SAX parsing.
        struct SaxHandler {
            /// Enables destruction through a handler base pointer.
            virtual ~SaxHandler();
            /// Receives null; return false to cancel.
            virtual bool onNull();
            /// Receives a boolean; return false to cancel.
            virtual bool onBool(bool aValue);
            /// Receives a signed integer; return false to cancel.
            virtual bool onInt(int64_t aValue);
            /// Receives an unsigned integer above the signed range; return false to cancel.
            virtual bool onUInt(uint64_t aValue);
            /// Receives a floating-point number; return false to cancel.
            virtual bool onDouble(double aValue);
            /// Receives a borrowed decoded string; return false to cancel.
            virtual bool onString(const std::string& aValue);
            /// Marks the beginning of an array; return false to cancel.
            virtual bool onStartArray();
            /// Marks the end of an array; return false to cancel.
            virtual bool onEndArray();
            /// Marks the beginning of an object; return false to cancel.
            virtual bool onStartObject();
            /// Receives a borrowed decoded key; return false to cancel.
            virtual bool onKey(const std::string& aKey);
            /// Marks the end of an object; return false to cancel.
            virtual bool onEndObject();
        };

        /// Uses the default DOM allocator and default parser options.
        explicit pJsonParser(const Options& aOptions = Options());
        /// Uses borrowed aAllocator, which must outlive this parser and its DOM results.
        explicit pJsonParser(pjson::Allocator& aAllocator, const Options& aOptions = Options());

        /// Returns the immutable options used by every parse call.
        const Options& options() const noexcept;
        /// Returns the allocator used for DOM results.
        pjson::Allocator& allocator() const noexcept;

        /// Parses one string document by value; failure returns JSON null.
        pjson parse(const std::string& aInput) const;
        /// Parses one exact byte span by value; failure returns JSON null.
        pjson parse(const char* aInput, size_t aSize) const;
        /// Parses one string document and resets/reports aError.
        pjson parse(const std::string& aInput, Error& aError) const;
        /// Parses one exact byte span and resets/reports aError.
        pjson parse(const char* aInput, size_t aSize, Error& aError) const;
        /// Buffers and parses one stream document by value.
        pjson parseStream(std::istream& aInput) const;
        /// Buffers and parses one stream document and resets/reports aError.
        pjson parseStream(std::istream& aInput, Error& aError) const;

        /// Emits events for one string document.
        bool parseSax(const std::string& aInput, SaxHandler& aHandler) const;
        /// Emits events for one exact byte span.
        bool parseSax(const char* aInput, size_t aSize, SaxHandler& aHandler) const;
        /// Emits string-document events and resets/reports aError.
        bool parseSax(const std::string& aInput, SaxHandler& aHandler, Error& aError) const;
        /// Emits byte-span events and resets/reports aError.
        bool parseSax(const char* aInput, size_t aSize, SaxHandler& aHandler, Error& aError) const;
        /// Incrementally emits events from a stream.
        bool parseSaxStream(std::istream& aInput, SaxHandler& aHandler) const;
        /// Incrementally emits events from a stream and resets/reports aError.
        bool parseSaxStream(std::istream& aInput, SaxHandler& aHandler, Error& aError) const;

    private:
        pjson::Allocator* _allocator;
        Options _options;
    };
} // namespace ByteDance

#endif // PRAVEENJSON_PARSER_H
