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
// Shared helpers for the pjson test suite.
//
// The public parse() API returns a pjson value plus a ParseError (no smart
// pointer). Parsed is a TEST-ONLY owning wrapper that adapts that value+error
// pair to a pointer-like handle so the many existing cases can keep reading as
// `if (p)`, `p->`, `*p`, and `p == nullptr` where those meant "parse
// succeeded". It is not part of the library API.
//
#ifndef PJSON_TEST_UTIL_H
#define PJSON_TEST_UTIL_H

#include "pjson.h"
#include "pjson_schema.h"
#include "test_harness.h"

#include <cstddef>
#include <istream>
#include <string>
#include <utility>
#include <vector>

namespace pjson_test {

    using ByteDance::pjson;
    using ByteDance::pJsonSchemaValidator;

    // Short aliases for the validator's vocabulary types. Schema validation is
    // no longer a pjson member; it lives in the external pJsonSchemaValidator,
    // which carries its own Error/Options types. These aliases keep the many
    // existing schema tests concise.
    typedef pJsonSchemaValidator::Error SchemaError;
    typedef pJsonSchemaValidator::Options SchemaOptions;

    // Convenience wrappers around the construct-once/validate API. The bulk of
    // the schema suite only checks pass/fail (optionally collecting errors and
    // supplying options) and does not care about reusing a compiled validator,
    // so these adapt the external validator to a single call. Tests that
    // exercise construction, reuse, or introspection use pJsonSchemaValidator
    // directly.
    inline bool schemaValidate(const pjson& aInstance, const pjson& aSchema) {
        pJsonSchemaValidator validator(aSchema);
        return validator.validate(aInstance);
    }
    inline bool schemaValidate(const pjson& aInstance, const pjson& aSchema,
                               std::vector<SchemaError>& aErrors) {
        pJsonSchemaValidator validator(aSchema);
        return validator.validate(aInstance, aErrors);
    }
    inline bool schemaValidate(const pjson& aInstance, const pjson& aSchema,
                               const SchemaOptions& aOptions) {
        pJsonSchemaValidator validator(aSchema, aOptions);
        return validator.validate(aInstance);
    }
    inline bool schemaValidate(const pjson& aInstance, const pjson& aSchema,
                               std::vector<SchemaError>& aErrors,
                               const SchemaOptions& aOptions) {
        pJsonSchemaValidator validator(aSchema, aOptions);
        return validator.validate(aInstance, aErrors);
    }

    // Owning, pointer-like parse result. `ok()` (and the bool/nullptr operators)
    // reflect ParseError::ok, so a successfully parsed literal `null` is truthy,
    // while only an actual failure compares equal to nullptr.
    struct Parsed {
        pjson value;
        pjson::ParseError error;

        Parsed() {} // error defaults to ok == true
        // Wraps an already-built value (e.g. a hand-constructed document) as a
        // successful result.
        Parsed(pjson aValue) // NOLINT(runtime/explicit): intentional convenience
                : value(std::move(aValue)) {}
        // Binds the held value to a specific allocator so allocator-provenance
        // tests observe the intended allocator after a same-allocator move.
        explicit Parsed(pjson::Allocator& aAlloc)
                : value(aAlloc) {}

        bool ok() const { return error.ok; }
        explicit operator bool() const { return error.ok; }
        bool operator==(std::nullptr_t) const { return !error.ok; }
        bool operator!=(std::nullptr_t) const { return error.ok; }

        pjson* operator->() { return &value; }
        const pjson* operator->() const { return &value; }
        pjson& operator*() { return value; }
        const pjson& operator*() const { return value; }
    };

    inline bool operator==(std::nullptr_t, const Parsed& aParsed) {
        return !aParsed.error.ok;
    }
    inline bool operator!=(std::nullptr_t, const Parsed& aParsed) {
        return aParsed.error.ok;
    }

    //== Default-allocator parse helpers =====================================
    inline Parsed parse(const std::string& s,
                        const pjson::ParseOptions& o = pjson::ParseOptions()) {
        Parsed r;
        r.value = pjson::parse(s, r.error, o);
        return r;
    }
    inline Parsed parse(const std::string& s, pjson::ParseError& e,
                        const pjson::ParseOptions& o = pjson::ParseOptions()) {
        Parsed r;
        r.value = pjson::parse(s, e, o);
        r.error = e;
        return r;
    }
    inline Parsed parse(const char* s, size_t n,
                        const pjson::ParseOptions& o = pjson::ParseOptions()) {
        Parsed r;
        r.value = pjson::parse(s, n, r.error, o);
        return r;
    }
    inline Parsed parse(const char* s, size_t n, pjson::ParseError& e,
                        const pjson::ParseOptions& o = pjson::ParseOptions()) {
        Parsed r;
        r.value = pjson::parse(s, n, e, o);
        r.error = e;
        return r;
    }

    //== Allocator-aware parse helpers (preserve provenance) =================
    inline Parsed parse(const std::string& s, pjson::Allocator& a,
                        const pjson::ParseOptions& o = pjson::ParseOptions()) {
        Parsed r(a);
        r.value = pjson::parse(s, r.error, a, o);
        return r;
    }
    inline Parsed parse(const std::string& s, pjson::ParseError& e, pjson::Allocator& a,
                        const pjson::ParseOptions& o = pjson::ParseOptions()) {
        Parsed r(a);
        r.value = pjson::parse(s, e, a, o);
        r.error = e;
        return r;
    }
    inline Parsed parse(const char* s, size_t n, pjson::Allocator& a,
                        const pjson::ParseOptions& o = pjson::ParseOptions()) {
        Parsed r(a);
        r.value = pjson::parse(s, n, r.error, a, o);
        return r;
    }
    inline Parsed parse(const char* s, size_t n, pjson::ParseError& e, pjson::Allocator& a,
                        const pjson::ParseOptions& o = pjson::ParseOptions()) {
        Parsed r(a);
        r.value = pjson::parse(s, n, e, a, o);
        r.error = e;
        return r;
    }

    //== Stream parse helpers ================================================
    inline Parsed parseStream(std::istream& in,
                              const pjson::ParseOptions& o = pjson::ParseOptions()) {
        Parsed r;
        r.value = pjson::parseStream(in, r.error, o);
        return r;
    }
    inline Parsed parseStream(std::istream& in, pjson::ParseError& e,
                              const pjson::ParseOptions& o = pjson::ParseOptions()) {
        Parsed r;
        r.value = pjson::parseStream(in, e, o);
        r.error = e;
        return r;
    }
    inline Parsed parseStream(std::istream& in, pjson::Allocator& a,
                              const pjson::ParseOptions& o = pjson::ParseOptions()) {
        Parsed r(a);
        r.value = pjson::parseStream(in, r.error, a, o);
        return r;
    }
    inline Parsed parseStream(std::istream& in, pjson::ParseError& e, pjson::Allocator& a,
                              const pjson::ParseOptions& o = pjson::ParseOptions()) {
        Parsed r(a);
        r.value = pjson::parseStream(in, e, a, o);
        r.error = e;
        return r;
    }

    inline int64_t valueInt(const ByteDance::pjson& value) {
        int64_t result = 0;
        CHECK(value.tryGet(result));
        return result;
    }

    inline double valueDouble(const ByteDance::pjson& value) {
        double result = 0.0;
        CHECK(value.tryGet(result));
        return result;
    }

    inline bool valueBool(const ByteDance::pjson& value) {
        bool result = false;
        CHECK(value.tryGet(result));
        return result;
    }

    inline std::string valueString(const ByteDance::pjson& value) {
        std::string result;
        CHECK(value.tryGet(result));
        return result;
    }

} // namespace pjson_test

#endif // PJSON_TEST_UTIL_H
