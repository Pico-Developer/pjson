// SPDX-License-Identifier: Apache-2.0

#include <pjson.h>
#include <pjson_parser.h>
#include <pjson_schema.h>

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

static_assert(PJSON_ABI_VERSION == 4, "unexpected pjson ABI generation");
static_assert(sizeof(ByteDance::pjson) == sizeof(void*) * 2,
              "installed pjson must use the two-pointer ABI");
static_assert(sizeof(ByteDance::pJsonParser) == sizeof(void*),
              "installed parser must use the one-pointer ABI");
static_assert(sizeof(ByteDance::pJsonSchemaValidator) == sizeof(void*),
              "installed schema validator must use the one-pointer ABI");

// ---- Installed-package consumer smoke test -----------------------------

// Verifies that an external C++11 consumer sees coherent headers, version
// metadata, linkage, parsing, typed access, and compact serialization.
int main() {
    using ByteDance::pjson;
    using ByteDance::pJsonParser;
    using ByteDance::pJsonSchemaValidator;

    // The public macro and linked library function must identify the same
    // release; this also detects stale headers paired with a different binary.
    if (std::strcmp(PJSON_VERSION, "4.0.0") != 0 ||
        std::strcmp(pjson::getVersion(), "4.0.0") != 0) {
        std::cerr << "unexpected pjson version" << std::endl;
        return 1;
    }

    // A compact round trip covers the main installed API without relying on
    // any source-tree-only headers or test helpers.
    pJsonParser::Error error;
    pjson document = pJsonParser().parse("{\"answer\":42}", error);
    pjson built;
    built["answer"] = 42;
    built["values"] = std::vector<int>({1, 2, 3});
    built["long"] = 4L;
    built["unsignedLong"] = 5UL;
    built["longLong"] = 6LL;
    built["unsignedLongLong"] = 7ULL;
    built["longDouble"] = 8.5L;
    pJsonParser::Error expectedError;
    pjson expectedBuilt =
        pJsonParser().parse("{\"answer\":42,\"values\":[1,2,3],\"long\":4,\"unsignedLong\":5,"
                            "\"longLong\":6,\"unsignedLongLong\":7,\"longDouble\":8.5}",
                            expectedError);
    int64_t answer = 0;
    if (!error.ok || !document.tryGet("answer", answer) || answer != 42 ||
        document.toString() != "{\"answer\":42}" || !expectedError.ok || built != expectedBuilt) {
        std::cerr << "installed pjson failed its consumer smoke test" << std::endl;
        return 1;
    }

    // The standalone schema validator ships in its own installed header and
    // consumes only the public API; confirm an external consumer can compile a
    // schema and validate against it.
    pJsonParser::Error schemaError;
    pjson schema =
        pJsonParser().parse("{\"type\":\"object\",\"required\":[\"answer\"]}", schemaError);
    if (!schemaError.ok) {
        std::cerr << "installed pjson_schema failed to parse its schema" << std::endl;
        return 1;
    }
    pJsonSchemaValidator validator(schema);
    if (!validator.isSchemaValid() ||
        validator.dialect() != pJsonSchemaValidator::documentedSubsetDialectUri()) {
        std::cerr << "installed pjson_schema failed its dialect contract" << std::endl;
        return 1;
    }
    std::vector<pJsonSchemaValidator::Error> schemaErrors;
    pjson missing = pJsonParser().parse("{}", schemaError);
    if (!validator.validate(document) || validator.validate(missing, schemaErrors) ||
        schemaErrors.empty()) {
        std::cerr << "installed pjson_schema failed its consumer smoke test" << std::endl;
        return 1;
    }

    return 0;
}
