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
// The core pjson DOM has no schema dependency, so applications that do not need
// validation never link this code.
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
    /// inputs.
    ///
    /// Dialect contract: the validator implements one explicitly named dialect,
    /// documentedSubsetDialectUri(). A root `$schema` may select it; any other
    /// declared or default dialect fails schema compilation. `$vocabulary` may
    /// require documentedSubsetVocabularyUri(); unknown optional vocabularies
    /// are annotations and unknown required vocabularies fail compilation.
    ///
    /// Supported keywords (documented subset):
    ///   type, enum, const, $ref (local JSON Pointer fragments);
    ///   properties, patternProperties, propertyNames, required,
    ///     dependentRequired, dependencies, dependentSchemas,
    ///     additionalProperties, minProperties, maxProperties;
    ///   items, prefixItems, contains, minContains, maxContains,
    ///     minItems, maxItems, uniqueItems;
    ///   minimum, maximum, exclusiveMinimum, exclusiveMaximum, multipleOf;
    ///   minLength, maxLength, pattern, format;
    ///   allOf, anyOf, oneOf, not, if, then, else.
    /// A boolean schema (true/false) accepts/rejects everything. By default
    /// unknown or unsupported keywords are ignored; strict() rejects unsupported
    /// standard keywords. Not implemented: $dynamicRef/$dynamicAnchor,
    /// unevaluatedItems/unevaluatedProperties, full standard-vocabulary
    /// negotiation/meta-schema loading, and remote $ref.
    class pJsonSchemaValidator {
    public:
        //== Diagnostics =====================================================
        /// One validation failure: `path` is a JSON Pointer to the offending
        /// node ("" for the document root) and `message` explains the failure.
        struct Error {
            /// Distinguishes an instance-validation failure from an invalid or
            /// unsupported schema contract discovered while compiling the validator.
            enum Category {
                InstanceValidation, ///< The instance violates a valid schema.
                SchemaCompilation   ///< The schema contract is invalid or unsupported.
            };

            std::string path; ///< JSON Pointer into the instance or schema.
            std::string message; ///< Human-readable validation or compilation diagnostic.
            Category category; ///< Selects which document `path` addresses.
            /// Constructs an error with an empty root path and message.
            Error();
            /// Constructs an error for aPath with the supplied diagnostic message.
            Error(const std::string& aPath, const std::string& aMsg,
                  Category aCategory = InstanceValidation);
        };

        //== Options =========================================================
        // Bounds schema regular-expression work and controls format checks. By
        // default only a conservative, non-ambiguous ECMAScript subset is
        // accepted and both pattern/subject sizes are capped, preventing
        // catastrophic std::regex backtracking. trustedRegex() restores
        // unrestricted ECMAScript regex behavior for trusted schemas/data.
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
            size_t maxErrors; ///< Collected diagnostic budget.
            bool validateFormats; ///< Validates known string formats (default true).
            // Strict, fail-closed subset mode. When true, a schema that uses a
            // standard validation/applicator keyword this validator does not
            // implement makes validation fail rather than silently ignoring the
            // constraint. Unknown non-standard extension keywords are still
            // allowed as annotations. Default false keeps permissive behavior.
            bool strictSubset; ///< Rejects unsupported standard keywords when true.
            /// Dialect used when the root schema has no `$schema`. The only
            /// supported value today is documentedSubsetDialectUri(). An empty
            /// value selects that default; every other URI is rejected when the
            /// validator is constructed.
            std::string defaultDialectUri; ///< Dialect used when `$schema` is absent.
            /// Selects bounded safe-regex, traversal, reference, work, error, and format defaults.
            Options();
            /// Disables only regex restrictions; all other defaults remain enabled.
            static Options trustedRegex();
            /// Returns the defaults with strict fail-closed subset mode enabled.
            static Options strict();
        };

        //== Construction ====================================================
        /// Compiles aSchema (deep-copied) with the supplied options. Inspect
        /// isSchemaValid()/schemaErrors() before trusting validation results.
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
        const Options& options() const noexcept;

    private:
        pJsonSchemaValidator(const pJsonSchemaValidator&);
        pJsonSchemaValidator& operator=(const pJsonSchemaValidator&);

        pjson _schema;    // owned, compiled deep copy of the schema
        Options _options; // validation limits and policy
        std::string _dialect;
        std::vector<Error> _schemaErrors;
    };
} // namespace ByteDance

#endif /* !PRAVEENJSON_SCHEMA_H */
