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
// pjson_schema.h — standalone JSON Schema validation for pjson.
//
// pJsonSchemaValidator validates a pjson value against a schema that is itself a
// pjson value. It is a pure consumer of pjson's public API: it holds a compiled
// (deep-copied) schema plus options and validates many instances against it.
// The core pjson type has no schema dependency. The implementation remains in
// focused private translation units so it can later become an independently
// linked optional component without changing the DOM API.
//
// This is a documented JSON Schema subset, not a complete draft implementation.
// See the supported-keyword list in the class comment.
//
// Author: Praveen Babu J D
// License: Apache 2.0
//
#ifndef PRAVEENJSON_SCHEMA_H
#define PRAVEENJSON_SCHEMA_H

#include "pjson.h"

#include <cstddef>
#include <string>
#include <vector>

namespace ByteDance {
    //==[Interface]============================================================
    /// Validates pjson values against a schema (itself a pjson value).
    ///
    /// Construct once from a schema; validate many instances. The schema is
    /// deep-copied on construction, so the caller's schema value may change or
    /// be destroyed afterward. Validation never throws and never mutates its
    /// inputs. Construction resolves and owns external resources; validate() is
    /// read-only and may be called concurrently when each caller uses its own
    /// error vector.
    ///
    /// Dialect contract: default construction implements the named subset dialect.
    /// Options::draft2020() selects official Draft 2020-12, bundled meta-schema
    /// validation, and per-resource vocabulary activation. Custom meta-schemas
    /// require explicit resolver-based loading. Unknown optional vocabularies are
    /// annotations and unknown required vocabularies fail compilation.
    ///
    /// Supported keywords (documented subset):
    ///   type, enum, const, $ref, $dynamicRef, $id, $anchor, $dynamicAnchor;
    ///   properties, patternProperties, propertyNames, required,
    ///     dependentRequired, dependencies, dependentSchemas,
    ///     additionalProperties, unevaluatedProperties, minProperties, maxProperties;
    ///   items, prefixItems, contains, minContains, maxContains,
    ///     minItems, maxItems, uniqueItems, unevaluatedItems;
    ///   minimum, maximum, exclusiveMinimum, exclusiveMaximum, multipleOf;
    ///   minLength, maxLength, pattern, format;
    ///   allOf, anyOf, oneOf, not, if, then, else.
    /// A boolean schema (true/false) accepts/rejects everything. By default
    /// unknown or unsupported keywords are ignored; strict() rejects unsupported
    /// standard keywords. External references require an explicit Resolver;
    /// pjson never performs network I/O. Regular expressions use a private Unicode-aware
    /// ECMAScript engine, including property escapes.
    class pJsonSchemaValidator {
    public:
        /// Resolves one absolute schema-document URI during construction.
        /// Implementations populate aSchema and return true, or return false
        /// when unavailable. The resolved document is copied into a cache owned
        /// by the validator; pjson performs no implicit network I/O. The callback
        /// and aContext need remain valid only until construction returns.
        typedef bool (*Resolver)(const std::string& aDocumentUri, pjson& aSchema, void* aContext);

        //== Diagnostics =====================================================
        /// One schema-compilation or instance-validation failure.
        struct Error {
            /// Distinguishes an instance-validation failure from an invalid or
            /// unsupported schema contract discovered while compiling the validator.
            enum Category {
                InstanceValidation, ///< The instance violates a valid schema.
                SchemaCompilation   ///< The schema contract is invalid or unsupported.
            };

            /// Stable machine-readable failure category. `keyword` identifies
            /// the precise keyword when several keywords share a category.
            enum Code {
                None,                  ///< No failure.
                FalseSchema,           ///< A false boolean schema rejected the instance.
                TypeMismatch,          ///< The instance does not satisfy `type`.
                ConstMismatch,         ///< The instance does not satisfy `const`.
                EnumMismatch,          ///< The instance does not satisfy `enum`.
                NumericConstraint,     ///< A numeric assertion failed.
                StringConstraint,      ///< A string assertion failed.
                ArrayConstraint,       ///< An array assertion failed.
                ObjectConstraint,      ///< An object assertion failed.
                FormatMismatch,        ///< A known asserted format failed.
                CombinatorMismatch,    ///< A logical applicator did not satisfy its contract.
                ReferenceFailure,      ///< A schema reference could not be resolved.
                ReferenceCycle,        ///< Reference evaluation encountered a cycle.
                RegexFailure,          ///< A pattern was invalid or rejected by safety policy.
                UnsupportedKeyword,    ///< Strict mode found an unsupported standard keyword.
                InvalidSchema,         ///< A schema or keyword has an invalid shape or value.
                UnsupportedDialect,    ///< The selected schema dialect is unsupported.
                UnsupportedVocabulary, ///< A required vocabulary is unsupported.
                ResolverFailure,       ///< The caller's external resolver could not load a schema.
                ResourceLimit,         ///< A compilation, validation, or diagnostic limit was hit.
                AllocationFailure,     ///< Validation could not allocate required temporary state.
                InternalError          ///< An unexpected exception was contained.
            };

            Code code;                    ///< Stable machine-readable failure category.
            Category category;            ///< Compilation or instance-validation phase.
            std::string instanceLocation; ///< JSON Pointer into the instance; empty is root.
            std::string schemaLocation; ///< Pointer or retrieval URI plus fragment for the schema.
            std::string keyword;        ///< Triggering keyword; empty for whole-schema failures.
            std::string message;        ///< Human-readable diagnostic, not a stable API token.
            std::vector<Error> causes;  ///< Optional bounded combinator branch failures.
            /// Constructs an empty diagnostic.
            Error();
            /// Constructs a fully located diagnostic.
            Error(Code aCode, Category aCategory, const std::string& aInstanceLocation,
                  const std::string& aSchemaLocation, const std::string& aKeyword,
                  const std::string& aMessage);
        };

        //== Options =========================================================
        // Bounds schema regular-expression work and controls format checks. By
        // default only a conservative, non-ambiguous ECMAScript subset is
        // accepted and both pattern/subject sizes are capped, preventing
        // catastrophic backtracking. trustedRegex() restores unrestricted
        // ECMAScript regex behavior for trusted schemas/data; the backend still
        // enforces its own finite work ceiling.
        struct Options {
            size_t maxRegexPatternBytes; ///< 0 = unlimited (default: 256).
            size_t maxRegexSubjectBytes; ///< 0 = unlimited (default: 4096).
            bool allowUnsafeRegex;       ///< Permits unrestricted ECMAScript regex (default false).
            /// Recursive validation depth (default and absolute hard ceiling: 64).
            /// Zero selects 64, and larger values are clamped to 64.
            size_t maxValidationDepth; ///< Recursive validation depth budget.
            /// Resolved references (default 1024); zero selects the hard ceiling of 1024.
            size_t maxRefResolutions; ///< Resolved-reference budget.
            /// Validation work units (default 1,000,000); zero selects that hard ceiling.
            size_t maxValidationWork; ///< Total validation work-unit budget.
            /// Reported errors (default 100); zero selects the hard ceiling of 100.
            size_t maxErrors;         ///< Collected top-level and nested diagnostic budget.
            bool stopAfterFirstError; ///< Stops compilation or validation after the first error.
            bool collectNestedCauses; ///< Retains bounded anyOf/oneOf branch failures.
            bool validateFormats;     ///< Validates known string formats (default true).
            // Strict, fail-closed subset mode. When true, a schema that uses a
            // standard validation/applicator keyword this validator does not
            // implement makes validation fail rather than silently ignoring the
            // constraint. Unknown non-standard extension keywords are still
            // allowed as annotations. Default false keeps permissive behavior.
            bool strictSubset; ///< Rejects unsupported standard keywords when true.
            /// Applies `$ref` siblings using pjson's modern subset semantics.
            /// The default false preserves legacy Draft 7 replacement semantics.
            bool refSiblings; ///< True when `$ref` siblings must also be evaluated.
            /// Allows an unknown absolute `$schema` URI to be resolved during
            /// construction and interpreted through its `$vocabulary` object.
            bool resolveCustomDialects; ///< Enables explicit custom meta-schema resolution.
            /// Retrieval URI used as the initial base when the root has no `$id`.
            /// Leave empty only when all references are absolute or local.
            std::string retrievalUri; ///< Initial base URI for a root without `$id`.
            /// Dialect used when the root schema has no `$schema`. An empty value
            /// selects documentedSubsetDialectUri(). Use draft2020() instead of
            /// setting this field directly when selecting the official dialect.
            std::string defaultDialectUri; ///< Dialect used when `$schema` is absent.
            Resolver resolver;             ///< Construction-only external-schema resolver.
            void* resolverContext;         ///< Construction-only opaque resolver context.
            size_t maxResolvedDocuments;   ///< External-document budget (default 32).
            size_t maxResolvedBytes;       ///< Compact resolved-DOM byte budget (default 16 MiB).
            /// Selects bounded safe-regex, traversal, reference, work, error, and format defaults.
            Options();
            /// Disables only regex restrictions; all other defaults remain enabled.
            static Options trustedRegex();
            /// Returns the defaults with strict fail-closed subset mode enabled.
            static Options strict();
            /// Selects modern subset semantics: `$ref` siblings apply and
            /// `format` is annotation-only unless the caller re-enables it.
            static Options modernSubset();
            /// Selects the official Draft 2020-12 dialect, modern `$ref`
            /// semantics, annotation-only format, and custom dialect resolution.
            static Options draft2020();
        };

        //== Construction ====================================================
        /// Compiles aSchema (deep-copied with the default allocator) and all
        /// resolved resources. Resolver failures, malformed references, and
        /// duplicate IDs/anchors are reported through isSchemaValid() and
        /// schemaErrors(); allocation failure may still throw std::bad_alloc.
        explicit pJsonSchemaValidator(const pjson& aSchema, const Options& aOptions = Options());
        /// Destroys the compiled schema.
        ~pJsonSchemaValidator();

        //== Validation ======================================================
        /// Returns whether aInstance conforms to the compiled schema. Never throws.
        bool validate(const pjson& aInstance) const noexcept;
        /// Validates and appends discovered failures to aErrors. If schema
        /// compilation failed, appends schemaErrors() instead. Never throws.
        bool validate(const pjson& aInstance, std::vector<Error>& aErrors) const noexcept;

        //== Dialect / compilation contract ===================================
        /// URI naming pjson's explicitly documented JSON Schema subset dialect.
        static const char* documentedSubsetDialectUri() noexcept;
        /// URI naming the vocabulary implemented by the subset dialect.
        static const char* documentedSubsetVocabularyUri() noexcept;
        /// URI of the supported official JSON Schema Draft 2020-12 dialect.
        static const char* draft2020DialectUri() noexcept;
        /// Returns whether dialect and required-vocabulary compilation succeeded.
        bool isSchemaValid() const noexcept;
        /// Returns immutable schema-compilation diagnostics (paths address the schema).
        const std::vector<Error>& schemaErrors() const noexcept;
        /// Returns the effective dialect URI selected by `$schema` or the option default.
        const std::string& dialect() const noexcept;

        //== Introspection ===================================================
        /// Returns the compiled schema value (read-only).
        const pjson& schema() const noexcept;
        /// Returns the options in effect for this validator.
        /// Construction-only resolver pointers are cleared in this snapshot.
        const Options& options() const noexcept;

    private:
        struct Impl;
        pJsonSchemaValidator(const pJsonSchemaValidator&);
        pJsonSchemaValidator& operator=(const pJsonSchemaValidator&);

        Impl* _impl;
    };
} // namespace ByteDance

#endif /* !PRAVEENJSON_SCHEMA_H */
