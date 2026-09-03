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
#include "pjson_schema_regex.h"
#include "pjson_schema_util.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace ByteDance;
using namespace ByteDance::pjson_schema_detail;

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

    //===------------------------------------------------------------------===//
    // Regex cache, run context, and diagnostic sink
    //===------------------------------------------------------------------===//

    // One compiled schema regex or a cached policy/syntax rejection.
    struct RegexCacheEntry {
        enum State { Uninitialized, Ready, PatternTooLarge, UnsafePattern, InvalidPattern };
        State state;
        EcmaRegex expression;
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
        std::string location;
        SchemaTarget()
                : schema(nullptr)
                , resourceRoot(nullptr) {}
        SchemaTarget(const pjson* aSchema, const pjson* aResourceRoot, const std::string& aBase,
                     const std::string& aLocation = std::string())
                : schema(aSchema)
                , resourceRoot(aResourceRoot)
                , baseUri(aBase)
                , location(aLocation) {}
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
        size_t diagnosticsUsed;
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
                , diagnosticsUsed(0)
                , publicErrorStart(aPublicErrors == nullptr ? 0 : aPublicErrors->size())
                , aborted(false) {}
    };

    struct SchemaBudgetExceeded {};

    size_t diagnosticLimit(const Options& options) {
        if (options.stopAfterFirstError)
            return size_t(1);
        return options.maxErrors == 0 ? size_t(100) : options.maxErrors;
    }

    // Facade over a caller or speculative error vector enforcing one shared
    // per-validation diagnostic budget.
    struct ErrorSink {
        enum Mode { Public, Causes, Discard };

        std::vector<SchemaError>& values;
        ValidationCtx& ctx;
        Mode mode;
        size_t failures;

        ErrorSink(std::vector<SchemaError>& aValues, ValidationCtx& aCtx, Mode aMode = Public)
                : values(aValues)
                , ctx(aCtx)
                , mode(aMode)
                , failures(0) {}

        size_t size() const { return failures; }

        void push_back(const SchemaError& error) {
            if (ctx.aborted)
                return;
            if (failures != std::numeric_limits<size_t>::max())
                ++failures;
            if (mode == Discard) {
                (void)error;
                return;
            }
            const size_t limit = diagnosticLimit(ctx.options);
            // Nested causes share the run-wide diagnostic budget and leave one
            // slot for their enclosing public combinator error.
            if (mode == Causes && ctx.diagnosticsUsed + size_t(1) >= limit) {
                return;
            }
            if (mode == Public && (ctx.errorsUsed >= limit || ctx.diagnosticsUsed >= limit)) {
                ctx.aborted = true;
                if (ctx.publicErrors != nullptr &&
                    ctx.publicErrors->size() - ctx.publicErrorStart < limit &&
                    ctx.diagnosticsUsed < limit) {
                    try {
                        ctx.publicErrors->push_back(
                            SchemaError(SchemaError::ResourceLimit, SchemaError::InstanceValidation,
                                        error.instanceLocation, error.schemaLocation, std::string(),
                                        "schema validation error budget exceeded"));
                        ++ctx.diagnosticsUsed;
                    } catch (...) {
                        ctx.publicErrors = nullptr;
                    }
                }
                throw SchemaBudgetExceeded();
            }
            values.push_back(error);
            ++ctx.diagnosticsUsed;
            if (mode == Public) {
                ++ctx.errorsUsed;
                if (ctx.options.stopAfterFirstError) {
                    ctx.aborted = true;
                    throw SchemaBudgetExceeded();
                }
            }
        }
    };

    std::string schemaLocationFor(const ValidationCtx& ctx, const pjson& schema,
                                  const std::string& keyword = std::string()) {
        std::map<const pjson*, SchemaTarget>::const_iterator found =
            ctx.compiled.nodeTargets.find(&schema);
        const std::string base =
            found == ctx.compiled.nodeTargets.end() ? std::string() : found->second.location;
        return keyword.empty() ? base : pointerAppend(base, keyword);
    }

    SchemaError validationError(const ValidationCtx& ctx, const pjson& schema,
                                SchemaError::Code code, const std::string& instanceLocation,
                                const std::string& keyword, const std::string& message) {
        return SchemaError(code, SchemaError::InstanceValidation, instanceLocation,
                           schemaLocationFor(ctx, schema, keyword), keyword, message);
    }

    void bestEffortSchemaError(std::vector<SchemaError>& errors, SchemaError::Code code,
                               const std::string& path, const std::string& message) noexcept {
        try {
            errors.push_back(SchemaError(code, SchemaError::InstanceValidation, path, std::string(),
                                         std::string(), message));
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
        const size_t errorLimit = diagnosticLimit(ctx.options);
        if (ctx.errorsUsed >= errorLimit)
            return;
        std::vector<SchemaError>& destination =
            ctx.publicErrors != nullptr ? *ctx.publicErrors : errors.values;
        const size_t before = destination.size();
        bestEffortSchemaError(destination, SchemaError::ResourceLimit, path, message);
        if (destination.size() != before) {
            ++ctx.errorsUsed;
            ++ctx.diagnosticsUsed;
        }
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

    bool addSchemaError(ValidationCtx& ctx, ErrorSink& errors, const pjson& schema,
                        SchemaError::Code code, const std::string& path, const std::string& keyword,
                        const std::string& message) {
        errors.push_back(validationError(ctx, schema, code, path, keyword, message));
        return !errors.ctx.aborted;
    }

    bool evaluateRegex(const std::string& subject, const std::string& pattern,
                       const std::string& path, const pjson& schema, const std::string& keyword,
                       ErrorSink& errors, ValidationCtx& ctx, bool& matches) {
        matches = false;
        if (ctx.options.maxRegexSubjectBytes != 0 &&
            subject.size() > ctx.options.maxRegexSubjectBytes) {
            errors.push_back(validationError(
                ctx, schema, SchemaError::ResourceLimit, path, keyword,
                "string exceeds regex safety limit (" + std::to_string(subject.size()) +
                    " bytes, limit " + std::to_string(ctx.options.maxRegexSubjectBytes) + ")"));
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
                cached.state = cached.expression.compile(pattern) ? RegexCacheEntry::Ready
                                                                  : RegexCacheEntry::InvalidPattern;
            }
        }

        if (cached.state == RegexCacheEntry::PatternTooLarge) {
            errors.push_back(validationError(ctx, schema, SchemaError::RegexFailure, path, keyword,
                                             "schema regex pattern exceeds safety limit"));
            return false;
        }
        if (cached.state == RegexCacheEntry::UnsafePattern) {
            errors.push_back(validationError(ctx, schema, SchemaError::RegexFailure, path, keyword,
                                             "schema regex pattern rejected by safety policy"));
            return false;
        }
        if (cached.state == RegexCacheEntry::InvalidPattern) {
            errors.push_back(validationError(ctx, schema, SchemaError::RegexFailure, path, keyword,
                                             "schema has an invalid regex pattern"));
            return false;
        }
        if (!chargeLoopWork(ctx, errors, path, subject.size() + size_t(1)))
            return false;
        const EcmaRegex::Result result = cached.expression.search(subject);
        if (result == EcmaRegex::WorkLimit) {
            errors.push_back(validationError(ctx, schema, SchemaError::ResourceLimit, path, keyword,
                                             "schema regex work limit exceeded"));
            return false;
        }
        if (result == EcmaRegex::Invalid) {
            errors.push_back(validationError(ctx, schema, SchemaError::RegexFailure, path, keyword,
                                             "schema regex evaluation failed"));
            return false;
        }
        matches = result == EcmaRegex::Match;
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

    std::string absoluteSchemaLocation(const std::string& documentUri, const std::string& pointer) {
        return documentUri.empty() ? pointer : stripFragment(documentUri) + "#" + pointer;
    }

    void addCompilationError(std::vector<SchemaError>& errors, SchemaError::Code code,
                             const std::string& path, const std::string& keyword,
                             const std::string& message) {
        errors.push_back(SchemaError(code, SchemaError::SchemaCompilation, std::string(), path,
                                     keyword, message));
    }

    bool isSchemaNode(const pjson& value) {
        return value.isObject() || value.isBool();
    }

    bool isFiniteSchemaNumber(const pjson& value) {
        if (!value.isNumber())
            return false;
        if (!value.isDouble())
            return true;
        double number = 0.0;
        return value.tryGet(number) && std::isfinite(number);
    }

    bool isNonnegativeInteger(const pjson& value) {
        size_t ignored = 0;
        bool aboveRange = false;
        return schemaSize(value, ignored, aboveRange);
    }

    bool validTypeName(const std::string& value) {
        static const char* const kTypes[] = {
            "null", "boolean", "object", "array", "number", "string", "integer",
        };
        for (const char* type : kTypes) {
            if (value == type)
                return true;
        }
        return false;
    }

    bool isUniqueStringArray(const pjson& value, bool allowEmpty) {
        if (!value.isArray() || (!allowEmpty && value.empty()))
            return false;
        std::set<std::string> seen;
        for (size_t i = 0; i < value.size(); ++i) {
            const pjson* item = value.find(static_cast<int>(i));
            if (item == nullptr || !item->isString() || !seen.insert(strOf(*item)).second)
                return false;
        }
        return true;
    }

    bool isTypeDeclaration(const pjson& value) {
        if (value.isString())
            return validTypeName(strOf(value));
        if (!isUniqueStringArray(value, false))
            return false;
        for (size_t i = 0; i < value.size(); ++i) {
            const pjson* item = value.find(static_cast<int>(i));
            if (item == nullptr || !validTypeName(strOf(*item)))
                return false;
        }
        return true;
    }

    bool hasDuplicateArrayValue(const pjson& value) {
        if (!value.isArray())
            return false;
        for (size_t i = 0; i < value.size(); ++i) {
            const pjson* left = value.find(static_cast<int>(i));
            for (size_t j = i + 1; left != nullptr && j < value.size(); ++j) {
                const pjson* right = value.find(static_cast<int>(j));
                if (right != nullptr && *left == *right)
                    return true;
            }
        }
        return false;
    }

    // Validates every implemented keyword before strict-mode validation begins.
    // Permissive mode deliberately retains the historical ignore-malformed behavior.
    void validateKeywordShapes(const pjson& schema, const Options& options,
                               std::vector<SchemaError>& errors, const std::string& documentUri,
                               const std::string& path) {
        if (!options.strictSubset || !schema.isObject())
            return;
        const size_t limit = diagnosticLimit(options);
        const auto reject = [&](const char* keyword, const std::string& expectation,
                                SchemaError::Code code = SchemaError::InvalidSchema) {
            if (errors.size() < limit)
                addCompilationError(
                    errors, code, absoluteSchemaLocation(documentUri, pointerAppend(path, keyword)),
                    keyword, std::string(keyword) + " " + expectation);
        };
        const auto valueOf = [&](const char* keyword) { return schema.find(keyword); };
        const auto rejectSchemaContainerValues = [&](const char* keyword, const pjson& value) {
            const std::vector<std::string> keys = value.keys();
            for (size_t i = 0; i < keys.size() && errors.size() < limit; ++i) {
                const pjson* child = value.find(keys[i]);
                if (child == nullptr || !isSchemaNode(*child))
                    addCompilationError(
                        errors, SchemaError::InvalidSchema,
                        absoluteSchemaLocation(
                            documentUri, pointerAppend(pointerAppend(path, keyword), keys[i])),
                        keyword, "schema must be an object or boolean");
            }
        };
        const auto rejectSchemaArrayValues = [&](const char* keyword, const pjson& value) {
            for (size_t i = 0; i < value.size() && errors.size() < limit; ++i) {
                const pjson* child = value.find(static_cast<int>(i));
                if (child == nullptr || !isSchemaNode(*child))
                    addCompilationError(errors, SchemaError::InvalidSchema,
                                        absoluteSchemaLocation(
                                            documentUri, pointerAppend(pointerAppend(path, keyword),
                                                                       std::to_string(i))),
                                        keyword, "schema must be an object or boolean");
            }
        };

        if (const pjson* value = valueOf("type")) {
            if (!isTypeDeclaration(*value))
                reject("type",
                       "must be a valid type name or a non-empty array of unique type names");
        }
        if (const pjson* value = valueOf("enum")) {
            if (!value->isArray() || value->empty() || hasDuplicateArrayValue(*value))
                reject("enum", "must be a non-empty array of unique values");
        }
        for (const char* keyword :
             {"$schema", "$comment", "title", "description", "pattern", "format"}) {
            const pjson* value = valueOf(keyword);
            if (value != nullptr && !value->isString())
                reject(keyword, "must be a string");
        }
        for (const char* keyword : {"deprecated", "readOnly", "writeOnly", "uniqueItems"}) {
            const pjson* value = valueOf(keyword);
            if (value != nullptr && !value->isBool())
                reject(keyword, "must be a boolean");
        }
        for (const char* keyword : {"minimum", "maximum", "exclusiveMinimum", "exclusiveMaximum"}) {
            const pjson* value = valueOf(keyword);
            if (value != nullptr && !isFiniteSchemaNumber(*value))
                reject(keyword, "must be a finite number");
        }
        if (const pjson* value = valueOf("multipleOf")) {
            if (!isFiniteSchemaNumber(*value) || numberAsDouble(*value) <= 0.0)
                reject("multipleOf", "must be a finite number greater than zero");
        }
        for (const char* keyword : {"minLength", "maxLength", "minItems", "maxItems", "minContains",
                                    "maxContains", "minProperties", "maxProperties"}) {
            const pjson* value = valueOf(keyword);
            if (value != nullptr && !isNonnegativeInteger(*value))
                reject(keyword, "must be a non-negative integer");
        }
        for (const char* keyword :
             {"additionalProperties", "unevaluatedProperties", "unevaluatedItems", "contains",
              "propertyNames", "not", "if", "then", "else"}) {
            const pjson* value = valueOf(keyword);
            if (value != nullptr && !isSchemaNode(*value))
                reject(keyword, "must be an object or boolean schema");
        }
        if (const pjson* value = valueOf("items")) {
            if (!isSchemaNode(*value) && !value->isArray())
                reject("items", "must be a schema or an array of schemas");
            else if (value->isArray())
                rejectSchemaArrayValues("items", *value);
        }
        for (const char* keyword : {"allOf", "anyOf", "oneOf"}) {
            const pjson* value = valueOf(keyword);
            if (value != nullptr) {
                if (!value->isArray() || value->empty())
                    reject(keyword, "must be a non-empty array of schemas");
                else
                    rejectSchemaArrayValues(keyword, *value);
            }
        }
        if (const pjson* value = valueOf("prefixItems")) {
            if (!value->isArray() || value->empty())
                reject("prefixItems", "must be a non-empty array of schemas");
            else
                rejectSchemaArrayValues("prefixItems", *value);
        }
        for (const char* keyword :
             {"$defs", "definitions", "properties", "patternProperties", "dependentSchemas"}) {
            const pjson* value = valueOf(keyword);
            if (value != nullptr) {
                if (!value->isObject())
                    reject(keyword, "must be an object whose values are schemas");
                else
                    rejectSchemaContainerValues(keyword, *value);
            }
        }
        if (const pjson* value = valueOf("required")) {
            if (!isUniqueStringArray(*value, true))
                reject("required", "must be an array of unique strings");
        }
        if (const pjson* value = valueOf("dependentRequired")) {
            if (!value->isObject()) {
                reject("dependentRequired", "must be an object");
            } else {
                const std::vector<std::string> keys = value->keys();
                for (size_t i = 0; i < keys.size(); ++i) {
                    const pjson* entry = value->find(keys[i]);
                    if (entry == nullptr || !isUniqueStringArray(*entry, true)) {
                        addCompilationError(
                            errors, SchemaError::InvalidSchema,
                            absoluteSchemaLocation(
                                documentUri,
                                pointerAppend(pointerAppend(path, "dependentRequired"), keys[i])),
                            "dependentRequired", "value must be an array of unique strings");
                        break;
                    }
                }
            }
        }
        if (const pjson* value = valueOf("dependencies")) {
            if (!value->isObject()) {
                reject("dependencies", "must be an object");
            } else {
                const std::vector<std::string> keys = value->keys();
                for (size_t i = 0; i < keys.size(); ++i) {
                    const pjson* entry = value->find(keys[i]);
                    if (entry == nullptr ||
                        (!isSchemaNode(*entry) && !isUniqueStringArray(*entry, true))) {
                        addCompilationError(
                            errors, SchemaError::InvalidSchema,
                            absoluteSchemaLocation(
                                documentUri,
                                pointerAppend(pointerAppend(path, "dependencies"), keys[i])),
                            "dependencies", "value must be a schema or an array of unique strings");
                        break;
                    }
                }
            }
        }
        if (const pjson* value = valueOf("$vocabulary")) {
            if (!value->isObject()) {
                reject("$vocabulary", "must be an object mapping URI strings to booleans");
            } else {
                const std::vector<std::string> keys = value->keys();
                for (size_t i = 0; i < keys.size(); ++i) {
                    const pjson* entry = value->find(keys[i]);
                    if (entry == nullptr || !entry->isBool()) {
                        reject("$vocabulary", "entries must be boolean");
                        break;
                    }
                }
            }
        }
        if (const pjson* value = valueOf("examples")) {
            if (!value->isArray())
                reject("examples", "must be an array");
        }
        const auto rejectRegex = [&](const std::string& pattern, const std::string& location,
                                     const char* keyword) {
            if (options.maxRegexPatternBytes != 0 &&
                pattern.size() > options.maxRegexPatternBytes) {
                if (errors.size() < limit)
                    addCompilationError(errors, SchemaError::RegexFailure, location, keyword,
                                        "schema regex pattern exceeds safety limit");
                return;
            }
            if (!validEcmaRegex(pattern)) {
                if (errors.size() < limit)
                    addCompilationError(errors, SchemaError::RegexFailure, location, keyword,
                                        "schema has an invalid regex pattern");
            }
        };
        if (const pjson* value = valueOf("pattern")) {
            if (value->isString())
                rejectRegex(strOf(*value),
                            absoluteSchemaLocation(documentUri, pointerAppend(path, "pattern")),
                            "pattern");
        }
        if (const pjson* value = valueOf("patternProperties")) {
            if (value->isObject()) {
                const std::vector<std::string> patterns = value->keys();
                for (size_t i = 0; i < patterns.size() && errors.size() < limit; ++i)
                    rejectRegex(
                        patterns[i],
                        absoluteSchemaLocation(
                            documentUri,
                            pointerAppend(pointerAppend(path, "patternProperties"), patterns[i])),
                        "patternProperties");
            }
        }
        for (const std::string& keyword : schema.keys()) {
            if (errors.size() >= limit)
                break;
            if (!isSupportedSchemaKeyword(keyword) && isStandardSchemaKeyword(keyword))
                reject(keyword.c_str(), "is not supported by the selected pjson dialect",
                       SchemaError::UnsupportedKeyword);
        }
    }

    // Establishes the root schema's dialect and required-vocabulary contract.
    // pjson deliberately names its implemented subset with a private URN rather
    // than accepting the official 2020-12 meta-schema URI and over-claiming
    // conformance. Unknown optional vocabularies are annotations; unknown
    // required vocabularies fail compilation.
    void compileDialectContract(const pjson& schema, const Options& options, std::string& dialect,
                                std::vector<SchemaError>& errors,
                                const std::string& location = std::string()) {
        const size_t errorLimit = diagnosticLimit(options);
        dialect = options.defaultDialectUri.empty() ? kDocumentedSubsetDialect
                                                    : options.defaultDialectUri;

        if (schema.isObject()) {
            const pjson* declared = schema.find("$schema");
            if (declared != nullptr) {
                if (!declared->isString()) {
                    addCompilationError(errors, SchemaError::InvalidSchema,
                                        pointerAppend(location, "$schema"), "$schema",
                                        "$schema must be a string URI");
                    return;
                } else {
                    dialect = strOf(*declared);
                }
            }
        }

        if (dialect != kDocumentedSubsetDialect) {
            addCompilationError(errors, SchemaError::UnsupportedDialect,
                                pointerAppend(location, "$schema"), "$schema",
                                "unsupported schema dialect: " + dialect);
            return;
        }

        if (!schema.isObject())
            return;
        const pjson* vocabularies = schema.find("$vocabulary");
        if (vocabularies == nullptr)
            return;
        if (!vocabularies->isObject()) {
            addCompilationError(errors, SchemaError::InvalidSchema,
                                pointerAppend(location, "$vocabulary"), "$vocabulary",
                                "$vocabulary must be an object mapping URI strings to booleans");
            return;
        }

        const std::vector<std::string> uris = vocabularies->keys();
        for (size_t i = 0; i < uris.size() && errors.size() < errorLimit; ++i) {
            const pjson* requirement = vocabularies->find(uris[i]);
            const std::string path =
                pointerAppend(location, "$vocabulary") + "/" + pjson::escapePointerToken(uris[i]);
            if (requirement == nullptr || !requirement->isBool()) {
                addCompilationError(errors, SchemaError::InvalidSchema, path, "$vocabulary",
                                    "$vocabulary entries must be boolean");
                continue;
            }
            bool required = false;
            requirement->tryGet(required);
            if (required && uris[i] != kDocumentedSubsetVocabulary) {
                addCompilationError(errors, SchemaError::UnsupportedVocabulary, path, "$vocabulary",
                                    "unsupported required schema vocabulary: " + uris[i]);
            }
        }
    }

    void compileSchemaResource(const pjson& node, const pjson* resourceRoot,
                               const std::string& inheritedBase, CompiledSchemaIndex& index,
                               std::vector<SchemaError>& errors, const Options& options,
                               const std::string& path, size_t depth = 0,
                               const std::string& documentUri = std::string()) {
        const size_t errorLimit = diagnosticLimit(options);
        if (errors.size() >= errorLimit)
            return;
        if (depth >= validationDepthLimit(options)) {
            addCompilationError(errors, SchemaError::ResourceLimit,
                                absoluteSchemaLocation(documentUri, path), std::string(),
                                "schema compilation depth budget exceeded");
            return;
        }
        const size_t workLimit = validationWorkLimit(options);
        if (index.workUsed >= workLimit) {
            addCompilationError(errors, SchemaError::ResourceLimit,
                                absoluteSchemaLocation(documentUri, path), std::string(),
                                "schema compilation work budget exceeded");
            return;
        }
        ++index.workUsed;
        if (!node.isObject() && !node.isBool()) {
            if (options.strictSubset)
                addCompilationError(errors, SchemaError::InvalidSchema,
                                    absoluteSchemaLocation(documentUri, path), std::string(),
                                    "schema must be an object or boolean");
            return;
        }
        const pjson* currentResource = resourceRoot;
        std::string currentBase = inheritedBase;
        if (node.isBool())
            index.nodeTargets[&node] = SchemaTarget(&node, currentResource, currentBase,
                                                    absoluteSchemaLocation(documentUri, path));
        if (node.isObject()) {
            const pjson* id = node.find("$id");
            if (id != nullptr && !id->isString()) {
                addCompilationError(errors, SchemaError::InvalidSchema,
                                    absoluteSchemaLocation(documentUri, pointerAppend(path, "$id")),
                                    "$id", "$id must be a string URI-reference");
                return;
            }
            if (id != nullptr) {
                currentBase = stripFragment(resolveUri(inheritedBase, strOf(*id)));
                currentResource = &node;
                if (resourceRoot != &node) {
                    std::string nestedDialect;
                    compileDialectContract(node, options, nestedDialect, errors,
                                           absoluteSchemaLocation(documentUri, path));
                }
            }
            if (!currentBase.empty()) {
                std::map<std::string, SchemaResource>::const_iterator existing =
                    index.resources.find(currentBase);
                if (existing != index.resources.end() && existing->second.root != currentResource) {
                    addCompilationError(
                        errors, SchemaError::InvalidSchema,
                        absoluteSchemaLocation(documentUri, pointerAppend(path, "$id")), "$id",
                        "duplicate schema resource identifier: " + currentBase);
                    return;
                }
                index.resources[currentBase] = SchemaResource(currentResource, currentBase);
            }
            const SchemaTarget nodeTarget(&node, currentResource, currentBase,
                                          absoluteSchemaLocation(documentUri, path));
            index.nodeTargets[&node] = nodeTarget;
            validateKeywordShapes(node, options, errors, documentUri, path);
            if (errors.size() >= errorLimit)
                return;

            const pjson* anchor = node.find("$anchor");
            if (anchor != nullptr && (!anchor->isString() || !validAnchorName(strOf(*anchor)))) {
                addCompilationError(
                    errors, SchemaError::InvalidSchema,
                    absoluteSchemaLocation(documentUri, pointerAppend(path, "$anchor")), "$anchor",
                    "$anchor must be a valid anchor name");
                return;
            }
            if (anchor != nullptr) {
                const std::string name = strOf(*anchor);
                const std::string key = currentBase + "#" + name;
                if (index.anchors.find(key) != index.anchors.end()) {
                    addCompilationError(
                        errors, SchemaError::InvalidSchema,
                        absoluteSchemaLocation(documentUri, pointerAppend(path, "$anchor")),
                        "$anchor", "duplicate schema anchor: " + key);
                    return;
                }
                index.anchors[key] = nodeTarget;
            }
            const pjson* dynamicAnchor = node.find("$dynamicAnchor");
            if (dynamicAnchor != nullptr &&
                (!dynamicAnchor->isString() || !validAnchorName(strOf(*dynamicAnchor)))) {
                addCompilationError(
                    errors, SchemaError::InvalidSchema,
                    absoluteSchemaLocation(documentUri, pointerAppend(path, "$dynamicAnchor")),
                    "$dynamicAnchor", "$dynamicAnchor must be a valid anchor name");
                return;
            }
            if (dynamicAnchor != nullptr) {
                const std::string name = strOf(*dynamicAnchor);
                const std::string key = currentBase + "#" + name;
                if (index.dynamicAnchors.find(key) != index.dynamicAnchors.end() ||
                    index.anchors.find(key) != index.anchors.end()) {
                    addCompilationError(
                        errors, SchemaError::InvalidSchema,
                        absoluteSchemaLocation(documentUri, pointerAppend(path, "$dynamicAnchor")),
                        "$dynamicAnchor", "duplicate schema anchor: " + key);
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
                    addCompilationError(
                        errors, SchemaError::InvalidSchema,
                        absoluteSchemaLocation(documentUri, pointerAppend(path, keyword)), keyword,
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
                                          options, pointerAppend(path, keyword), depth + 1,
                                          documentUri);
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
                        compileSchemaResource(*child, currentResource, currentBase, index, errors,
                                              options,
                                              pointerAppend(pointerAppend(path, keyword), names[i]),
                                              depth + 1, documentUri);
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
                                depth + 1, documentUri);
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
                            depth + 1, documentUri);
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
                                depth + 1, documentUri);
                    }
                }
            }
        }
        return;
    }

    void compileExternalResources(CompiledSchemaIndex& index, const Options& options,
                                  std::vector<SchemaError>& errors) {
        const size_t errorLimit = diagnosticLimit(options);
        while (!index.pendingDocuments.empty() && errors.size() < errorLimit) {
            const std::string documentUri = *index.pendingDocuments.begin();
            index.pendingDocuments.erase(index.pendingDocuments.begin());
            if (index.resources.find(documentUri) != index.resources.end())
                continue;
            if (!uriHasScheme(documentUri)) {
                addCompilationError(
                    errors, SchemaError::ReferenceFailure, "", "$ref",
                    "relative external schema reference requires a retrieval URI or root $id: " +
                        documentUri);
                index.failedDocuments.insert(documentUri);
                continue;
            }
            if (options.resolver == nullptr) {
                addCompilationError(errors, SchemaError::ResolverFailure, "", "$ref",
                                    "no resolver for external schema: " + documentUri);
                index.failedDocuments.insert(documentUri);
                continue;
            }
            if (index.documents.size() >= resolvedDocumentLimit(options)) {
                addCompilationError(errors, SchemaError::ResourceLimit, "", "$ref",
                                    "schema resolved-document budget exceeded");
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
                addCompilationError(errors, SchemaError::ResolverFailure, documentUri, "$ref",
                                    "external schema resolver threw for " + documentUri + ": " +
                                        exception.what());
                index.failedDocuments.insert(documentUri);
                continue;
            } catch (...) {
                index.documents.pop_back();
                addCompilationError(errors, SchemaError::ResolverFailure, documentUri, "$ref",
                                    "external schema resolver threw for " + documentUri);
                index.failedDocuments.insert(documentUri);
                continue;
            }
            if (!resolved) {
                index.documents.pop_back();
                addCompilationError(errors, SchemaError::ResolverFailure, documentUri, "$ref",
                                    "external schema resolution failed: " + documentUri);
                index.failedDocuments.insert(documentUri);
                continue;
            }
            loaded.schema.copyFrom(temporary);
            std::string resolvedDialect;
            const size_t beforeContract = errors.size();
            compileDialectContract(loaded.schema, options, resolvedDialect, errors,
                                   documentUri + "#");
            if (errors.size() != beforeContract) {
                index.documents.pop_back();
                index.failedDocuments.insert(documentUri);
                continue;
            }
            const size_t limit = resolvedByteLimit(options);
            if (index.resolvedBytes >= limit) {
                index.documents.pop_back();
                addCompilationError(errors, SchemaError::ResourceLimit, documentUri, "$ref",
                                    "schema resolved-byte budget exceeded");
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
                addCompilationError(errors, SchemaError::ResourceLimit, documentUri, "$ref",
                                    "schema resolved-byte budget exceeded");
                index.failedDocuments.insert(documentUri);
                return;
            } catch (const std::exception& exception) {
                index.documents.pop_back();
                addCompilationError(errors, SchemaError::InvalidSchema, documentUri, std::string(),
                                    "resolved schema is not serializable: " +
                                        std::string(exception.what()));
                index.failedDocuments.insert(documentUri);
                continue;
            }
            if (compact.size() > limit - std::min(index.resolvedBytes, limit)) {
                index.documents.pop_back();
                addCompilationError(errors, SchemaError::ResourceLimit, documentUri, "$ref",
                                    "schema resolved-byte budget exceeded");
                index.failedDocuments.insert(documentUri);
                return;
            }
            index.resolvedBytes += compact.size();

            const pjson* root = &loaded.schema;
            // Keep the retrieval URI as an alias, then let compilation apply the
            // root `$id` exactly once relative to that retrieval URI.
            index.resources[documentUri] = SchemaResource(root, documentUri);
            compileSchemaResource(*root, root, documentUri, index, errors, options, "", 0,
                                  documentUri);
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
            target = SchemaTarget(root, root, resource->second.baseUri,
                                  absoluteSchemaLocation(document, ""));
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
                     ? SchemaTarget(selected, root, resource->second.baseUri,
                                    absoluteSchemaLocation(document, decoded))
                     : indexed->second;
        return true;
    }

    void validateCompiledReferences(const CompiledSchemaIndex& index, const Options& options,
                                    std::vector<SchemaError>& errors) {
        const size_t errorLimit = diagnosticLimit(options);
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
                    addCompilationError(errors, SchemaError::ReferenceFailure,
                                        pointerAppend(it->second.location, keyword), keyword,
                                        std::string("unresolved ") + keyword + ": " +
                                            strOf(*reference));
                } else if (options.strictSubset && target.schema != nullptr &&
                           !target.schema->isObject() && !target.schema->isBool()) {
                    addCompilationError(errors, SchemaError::InvalidSchema,
                                        pointerAppend(it->second.location, keyword), keyword,
                                        std::string(keyword) +
                                            " target must be an object or boolean schema");
                }
            }
        }
    }

    bool resolveSchemaReference(const std::string& reference, const pjson* resourceRoot,
                                const std::string& baseUri, ValidationCtx& ctx, ErrorSink& errors,
                                const std::string& path, const pjson& schema,
                                const std::string& keyword, SchemaTarget& target) {
        const std::string absolute = resolveUri(baseUri, reference);
        std::string document;
        std::string fragment;
        splitReference(absolute, document, fragment);
        std::string decodedFragment;
        if (!percentDecodeFragment(fragment, decodedFragment)) {
            errors.push_back(validationError(ctx, schema, SchemaError::ReferenceFailure, path,
                                             keyword, "malformed schema reference: " + reference));
            return false;
        }

        if (document.empty())
            document = stripFragment(baseUri);
        std::map<std::string, SchemaResource>::const_iterator resource =
            ctx.compiled.resources.find(document);
        if (resource == ctx.compiled.resources.end()) {
            errors.push_back(validationError(ctx, schema, SchemaError::ReferenceFailure, path,
                                             keyword,
                                             "unresolved compiled schema resource: " + document));
            return false;
        }

        const pjson* root = resource->second.root != nullptr ? resource->second.root : resourceRoot;
        if (fragment.empty()) {
            target = SchemaTarget(root, root, resource->second.baseUri,
                                  absoluteSchemaLocation(document, ""));
            return true;
        }
        if (decodedFragment.empty() || decodedFragment[0] != '/') {
            const std::string anchorKey = document + "#" + decodedFragment;
            std::map<std::string, SchemaTarget>::const_iterator found =
                ctx.compiled.anchors.find(anchorKey);
            if (found == ctx.compiled.anchors.end()) {
                errors.push_back(validationError(ctx, schema, SchemaError::ReferenceFailure, path,
                                                 keyword, "unresolved schema anchor: " + absolute));
                return false;
            }
            target = found->second;
            return true;
        }

        pjson::PointerError pointerError;
        const pjson* selected = root->findPointer(decodedFragment, pointerError);
        if (selected == nullptr) {
            errors.push_back(validationError(ctx, schema, SchemaError::ReferenceFailure, path,
                                             keyword, "unresolved schema reference: " + reference));
            return false;
        }
        std::map<const pjson*, SchemaTarget>::const_iterator indexed =
            ctx.compiled.nodeTargets.find(selected);
        target = indexed == ctx.compiled.nodeTargets.end()
                     ? SchemaTarget(selected, root, resource->second.baseUri,
                                    absoluteSchemaLocation(document, decodedFragment))
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
                    errors.push_back(validationError(ctx, *currentSchema, SchemaError::FalseSchema,
                                                     path, std::string(),
                                                     "schema is false; no value is valid here"));
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
                                        path, *currentSchema, "$ref", resolved))
                return false;

            const std::pair<const pjson*, const pjson*> active(&node, resolved.schema);
            if (std::find(ctx.activeRefs.begin(), ctx.activeRefs.end(), active) !=
                ctx.activeRefs.end()) {
                errors.push_back(validationError(ctx, *currentSchema, SchemaError::ReferenceCycle,
                                                 path, "$ref",
                                                 "schema reference cycle detected: " + refText));
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
                                            errors, path, schema, "$ref", resolved))
                    return false;
                const std::pair<const pjson*, const pjson*> active(&node, resolved.schema);
                if (std::find(ctx.activeRefs.begin(), ctx.activeRefs.end(), active) !=
                    ctx.activeRefs.end()) {
                    errors.push_back(
                        validationError(ctx, schema, SchemaError::ReferenceCycle, path, "$ref",
                                        "schema reference cycle detected: " + refText));
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
                                            errors, path, schema, "$dynamicRef", resolved))
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
                    errors.push_back(validationError(
                        ctx, schema, SchemaError::ReferenceCycle, path, "$dynamicRef",
                        "schema dynamic-reference cycle detected: " + refText));
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
                    addSchemaError(
                        ctx, errors, schema, SchemaError::UnsupportedKeyword, path, keys[i],
                        "strict schema mode: unsupported standard keyword \"" + keys[i] + "\"");
                }
            }
            if (ctx.aborted)
                return false;
        }

        // ---- type ----
        if (const pjson* t = schema.find("type")) {
            if (t->isString()) {
                if (!typeMatches(node, strOf(*t)))
                    errors.push_back(
                        validationError(ctx, schema, SchemaError::TypeMismatch, path, "type",
                                        "expected type " + strOf(*t) + ", got " + typeName(node)));
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
                    errors.push_back(validationError(
                        ctx, schema, SchemaError::TypeMismatch, path, "type",
                        "expected one of type [" + names + "], got " + typeName(node)));
            }
        }

        // ---- const ----
        if (const pjson* cst = schema.find("const")) {
            bool equal = false;
            if (!equalWithBudget(node, *cst, ctx, errors, path, equal))
                return false;
            if (!equal)
                errors.push_back(validationError(ctx, schema, SchemaError::ConstMismatch, path,
                                                 "const",
                                                 "value does not equal the required const"));
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
                    errors.push_back(validationError(ctx, schema, SchemaError::EnumMismatch, path,
                                                     "enum", "value is not in the allowed enum"));
            }
        }

        // ---- numeric constraints ----
        if (node.isNumber()) {
            int order = 0;
            if (const pjson* m = schema.find("minimum")) {
                if (m->isNumber() && node.tryCompareNumber(*m, order) && order < 0)
                    addSchemaError(
                        ctx, errors, schema, SchemaError::NumericConstraint, path, "minimum",
                        "value " + formatNumber(node) + " is below minimum " + formatNumber(*m));
            }
            if (const pjson* m = schema.find("maximum")) {
                if (m->isNumber() && node.tryCompareNumber(*m, order) && order > 0)
                    addSchemaError(
                        ctx, errors, schema, SchemaError::NumericConstraint, path, "maximum",
                        "value " + formatNumber(node) + " is above maximum " + formatNumber(*m));
            }
            if (const pjson* m = schema.find("exclusiveMinimum")) {
                if (m->isNumber() && node.tryCompareNumber(*m, order) && order <= 0)
                    addSchemaError(ctx, errors, schema, SchemaError::NumericConstraint, path,
                                   "exclusiveMinimum",
                                   "value " + formatNumber(node) +
                                       " is not greater than exclusiveMinimum " + formatNumber(*m));
            }
            if (const pjson* m = schema.find("exclusiveMaximum")) {
                if (m->isNumber() && node.tryCompareNumber(*m, order) && order >= 0)
                    addSchemaError(ctx, errors, schema, SchemaError::NumericConstraint, path,
                                   "exclusiveMaximum",
                                   "value " + formatNumber(node) +
                                       " is not less than exclusiveMaximum " + formatNumber(*m));
            }
            if (const pjson* m = schema.find("multipleOf")) {
                if (m->isNumber() && !isExactMultiple(node, *m))
                    addSchemaError(ctx, errors, schema, SchemaError::NumericConstraint, path,
                                   "multipleOf",
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
                    addSchemaError(ctx, errors, schema, SchemaError::StringConstraint, path,
                                   "minLength",
                                   "string length " + std::to_string(length) +
                                       " is below minLength " + formatNumber(*m));
            }
            if (const pjson* m = schema.find("maxLength")) {
                size_t bound = 0;
                bool aboveRange = false;
                if (schemaSize(*m, bound, aboveRange) && !aboveRange && length > bound)
                    addSchemaError(ctx, errors, schema, SchemaError::StringConstraint, path,
                                   "maxLength",
                                   "string length " + std::to_string(length) +
                                       " is above maxLength " + formatNumber(*m));
            }
            if (const pjson* p = schema.find("pattern")) {
                if (p->isString()) {
                    const std::string pattern = strOf(*p);
                    bool matches = false;
                    if (evaluateRegex(s, pattern, path, schema, "pattern", errors, ctx, matches) &&
                        !matches)
                        errors.push_back(validationError(
                            ctx, schema, SchemaError::StringConstraint, path, "pattern",
                            "string does not match pattern /" + pattern + "/"));
                }
            }
            if (ctx.options.validateFormats) {
                if (const pjson* format = schema.find("format")) {
                    if (format->isString()) {
                        bool known = false;
                        if (!knownFormatValid(strOf(*format), s, known) && known)
                            errors.push_back(validationError(
                                ctx, schema, SchemaError::FormatMismatch, path, "format",
                                "string is not a valid " + strOf(*format) + " format"));
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
                    addSchemaError(ctx, errors, schema, SchemaError::ArrayConstraint, path,
                                   "minItems",
                                   "array has " + std::to_string(arrSize) +
                                       " items, below minItems " + formatNumber(*m));
            }
            if (const pjson* m = schema.find("maxItems")) {
                size_t bound = 0;
                bool aboveRange = false;
                if (schemaSize(*m, bound, aboveRange) && !aboveRange && arrSize > bound)
                    addSchemaError(ctx, errors, schema, SchemaError::ArrayConstraint, path,
                                   "maxItems",
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
                        errors.push_back(validationError(ctx, schema, SchemaError::ArrayConstraint,
                                                         path, "uniqueItems",
                                                         "array items are not unique"));
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
                    ErrorSink scratchSink(scratch, ctx, ErrorSink::Discard);
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
                    addSchemaError(ctx, errors, schema, SchemaError::ArrayConstraint, path,
                                   "contains",
                                   "array has " + std::to_string(matched) +
                                       " items matching \"contains\", below minContains " +
                                       std::to_string(minContains));
                if (const pjson* xc = schema.find("maxContains")) {
                    size_t bound = 0;
                    bool xcAbove = false;
                    if (schemaSize(*xc, bound, xcAbove) && !xcAbove && matched > bound)
                        addSchemaError(ctx, errors, schema, SchemaError::ArrayConstraint, path,
                                       "contains",
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
                            errors.push_back(validationError(
                                ctx, schema, SchemaError::ObjectConstraint, path, "required",
                                "missing required property \"" + strOf(*k) + "\""));
                    }
                }
            }
            if (const pjson* m = schema.find("minProperties")) {
                size_t bound = 0;
                bool aboveRange = false;
                if (schemaSize(*m, bound, aboveRange) && (aboveRange || memberKeys.size() < bound))
                    addSchemaError(ctx, errors, schema, SchemaError::ObjectConstraint, path,
                                   "minProperties",
                                   "object has " + std::to_string(memberKeys.size()) +
                                       " properties, below minProperties " + formatNumber(*m));
            }
            if (const pjson* m = schema.find("maxProperties")) {
                size_t bound = 0;
                bool aboveRange = false;
                if (schemaSize(*m, bound, aboveRange) && !aboveRange && memberKeys.size() > bound)
                    addSchemaError(ctx, errors, schema, SchemaError::ObjectConstraint, path,
                                   "maxProperties",
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
                                          pointerAppend(path, memberKeys[i]), schema,
                                          "patternProperties", errors, ctx, matches) &&
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
                            errors.push_back(validationError(
                                ctx, schema, SchemaError::ObjectConstraint, path,
                                "dependentRequired",
                                "property \"" + depKeys[d] + "\" requires property \"" +
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
                                errors.push_back(validationError(
                                    ctx, schema, SchemaError::ObjectConstraint, path,
                                    "dependencies",
                                    "property \"" + depKeys[d] + "\" requires property \"" +
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
                            errors.push_back(validationError(
                                ctx, schema, SchemaError::ObjectConstraint,
                                pointerAppend(path, memberKeys[i]), "additionalProperties",
                                "additional property \"" + memberKeys[i] + "\" is not allowed"));
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
            ErrorSink scratchSink(scratch, ctx, ErrorSink::Discard);
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
                std::vector<SchemaError> causes;
                const size_t causeBudgetStart = ctx.diagnosticsUsed;
                for (size_t i = 0; i < anyOf->size(); ++i) {
                    if (!chargeLoopWork(ctx, errors, path))
                        return false;
                    const pjson* sub = anyOf->find(static_cast<int>(i));
                    if (sub == nullptr)
                        continue;
                    std::vector<SchemaError> discarded;
                    std::vector<SchemaError>& branchErrors =
                        ctx.options.collectNestedCauses ? causes : discarded;
                    ErrorSink scratchSink(branchErrors, ctx,
                                          ctx.options.collectNestedCauses ? ErrorSink::Causes
                                                                          : ErrorSink::Discard);
                    SchemaAnnotations branchAnnotations;
                    if (validateCtx(node, *sub, path, scratchSink, ctx, nullptr, std::string(),
                                    &branchAnnotations)) {
                        any = true;
                        evaluated.merge(branchAnnotations);
                    }
                    if (ctx.aborted)
                        return false;
                }
                if (!any) {
                    SchemaError error =
                        validationError(ctx, schema, SchemaError::CombinatorMismatch, path, "anyOf",
                                        "value does not match any schema in anyOf");
                    if (ctx.options.collectNestedCauses)
                        error.causes.swap(causes);
                    errors.push_back(error);
                } else
                    ctx.diagnosticsUsed = causeBudgetStart;
            }
        }
        if (const pjson* oneOf = schema.find("oneOf")) {
            if (oneOf->isArray()) {
                int matches = 0;
                SchemaAnnotations matchingAnnotations;
                std::vector<SchemaError> causes;
                const size_t causeBudgetStart = ctx.diagnosticsUsed;
                for (size_t i = 0; i < oneOf->size(); ++i) {
                    if (!chargeLoopWork(ctx, errors, path))
                        return false;
                    const pjson* sub = oneOf->find(static_cast<int>(i));
                    if (sub == nullptr)
                        continue;
                    std::vector<SchemaError> discarded;
                    std::vector<SchemaError>& branchErrors =
                        ctx.options.collectNestedCauses ? causes : discarded;
                    ErrorSink scratchSink(branchErrors, ctx,
                                          ctx.options.collectNestedCauses ? ErrorSink::Causes
                                                                          : ErrorSink::Discard);
                    SchemaAnnotations branchAnnotations;
                    if (validateCtx(node, *sub, path, scratchSink, ctx, nullptr, std::string(),
                                    &branchAnnotations)) {
                        ++matches;
                        matchingAnnotations = branchAnnotations;
                    }
                    if (ctx.aborted)
                        return false;
                }
                if (matches != 1) {
                    SchemaError error =
                        validationError(ctx, schema, SchemaError::CombinatorMismatch, path, "oneOf",
                                        "value matched " + std::to_string(matches) +
                                            " schemas in oneOf (exactly 1 required)");
                    if (ctx.options.collectNestedCauses && matches == 0)
                        error.causes.swap(causes);
                    else
                        ctx.diagnosticsUsed = causeBudgetStart;
                    errors.push_back(error);
                } else {
                    ctx.diagnosticsUsed = causeBudgetStart;
                    evaluated.merge(matchingAnnotations);
                }
            }
        }
        const pjson* nots = schema.find("not");
        if (nots != nullptr && (nots->isBool() || nots->isObject())) {
            std::vector<SchemaError> scratch;
            ErrorSink scratchSink(scratch, ctx, ErrorSink::Discard);
            if (validateCtx(node, *nots, path, scratchSink, ctx))
                errors.push_back(validationError(ctx, schema, SchemaError::CombinatorMismatch, path,
                                                 "not", "value must not match the \"not\" schema"));
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
                        errors.push_back(validationError(ctx, schema, SchemaError::ObjectConstraint,
                                                         memberPath, "unevaluatedProperties",
                                                         "unevaluated property is not allowed"));
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
                        errors.push_back(validationError(ctx, schema, SchemaError::ArrayConstraint,
                                                         itemPath, "unevaluatedItems",
                                                         "unevaluated item is not allowed"));
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
            bestEffortSchemaError(errors, SchemaError::AllocationFailure, "",
                                  "schema validation ran out of memory");
        } catch (const std::exception&) {
            bestEffortSchemaError(errors, SchemaError::InternalError, "",
                                  "schema validation failed with an internal exception");
        } catch (...) {
            bestEffortSchemaError(errors, SchemaError::InternalError, "",
                                  "schema validation failed with an unknown exception");
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
        const std::string retrievalBase = stripFragment(options.retrievalUri);
        compileDialectContract(schema, options, dialect, schemaErrors,
                               retrievalBase.empty() ? std::string() : retrievalBase + "#");
        if (!schemaErrors.empty()) {
            options.resolver = nullptr;
            options.resolverContext = nullptr;
            return;
        }
        compiled.resources[retrievalBase] = SchemaResource(&schema, retrievalBase);
        compileSchemaResource(schema, &schema, retrievalBase, compiled, schemaErrors, options, "",
                              0, retrievalBase);
        if (options.stopAfterFirstError && !schemaErrors.empty()) {
            schemaErrors.resize(1);
            options.resolver = nullptr;
            options.resolverContext = nullptr;
            return;
        }
        compileExternalResources(compiled, options, schemaErrors);
        if (options.stopAfterFirstError && schemaErrors.size() > size_t(1))
            schemaErrors.resize(1);
        if (options.stopAfterFirstError && !schemaErrors.empty()) {
            options.resolver = nullptr;
            options.resolverContext = nullptr;
            return;
        }
        validateCompiledReferences(compiled, options, schemaErrors);
        if (options.stopAfterFirstError && schemaErrors.size() > size_t(1))
            schemaErrors.resize(1);
        // Resolver state is construction-only. Do not retain an application
        // context pointer that may become dangling after compilation finishes.
        options.resolver = nullptr;
        options.resolverContext = nullptr;
    }
};

pJsonSchemaValidator::Error::Error()
        : code(None)
        , category(InstanceValidation) {}
pJsonSchemaValidator::Error::Error(Code aCode, Category aCategory,
                                   const std::string& aInstanceLocation,
                                   const std::string& aSchemaLocation, const std::string& aKeyword,
                                   const std::string& aMessage)
        : code(aCode)
        , category(aCategory)
        , instanceLocation(aInstanceLocation)
        , schemaLocation(aSchemaLocation)
        , keyword(aKeyword)
        , message(aMessage) {}

pJsonSchemaValidator::Options::Options()
        : maxRegexPatternBytes(256)
        , maxRegexSubjectBytes(4096)
        , allowUnsafeRegex(false)
        , maxValidationDepth(kSchemaValidationDepthHardLimit)
        , maxRefResolutions(1024)
        , maxValidationWork(1000000)
        , maxErrors(100)
        , stopAfterFirstError(false)
        , collectNestedCauses(false)
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
