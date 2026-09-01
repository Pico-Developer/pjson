//
// Copyright 2025 ByteDance Ltd. and/or its affiliates. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License").
//
//===----------------------------------------------------------------------===//
// 06 — Schema validation
//
// Validate a document against a JSON-Schema-subset schema (itself a pjson
// value), collecting applicable failures within configured budgets.
// Referenced by docs/06-schema-validation.md.
//
#include "pjson.h"
#include "pjson_schema.h"

#include <iostream>
#include <vector>

using namespace ByteDance;

// Validates one conforming and one non-conforming instance against a reusable
// schema, first as a yes/no query and then with detailed errors.
int main() {
    // --- Define the schema -------------------------------------------------
    // Local $defs keep shared constraints in one place; $ref resolves them by
    // RFC 6901 fragment pointers within this same schema document.
    pjson::ParseError parseError;
    pjson schema = pjson::parse(R"({
        "$defs": {
            "displayName": { "type": "string", "minLength": 1 },
            "emailAddress": { "type": "string", "pattern": "@" }
        },
        "type": "object",
        "required": ["name", "age", "email", "joined"],
        "additionalProperties": false,
        "properties": {
            "name":  { "$ref": "#/$defs/displayName" },
            "age":   { "type": "integer", "minimum": 0, "maximum": 150 },
            "email": { "$ref": "#/$defs/emailAddress" },
            "joined": { "type": "string", "format": "date" },
            "roles": {
                "type": "array",
                "items": { "type": "string", "enum": ["admin", "user", "guest"] }
            }
        }
    })",
                                parseError);

    // --- Validate a conforming instance -----------------------------------
    pjson::ParseError goodError;
    pjson good = pjson::parse(R"({
        "name": "Ada", "age": 36, "email": "ada@example.com",
        "joined": "2025-01-02", "roles": ["admin"]
    })",
                              goodError);
    if (!parseError.ok || !goodError.ok) {
        std::cerr << "could not parse schema or valid example\n";
        return 1;
    }

    // These limits bound traversal and reference work. Known string formats,
    // such as the date above, are checked because validateFormats is enabled.
    // The schema is compiled once into a reusable validator; validate() then
    // checks any number of instances against it.
    pJsonSchemaValidator::Options options;
    options.maxValidationDepth = 64;
    options.maxRefResolutions = 1024;
    options.validateFormats = true;
    pJsonSchemaValidator validator(schema, options);
    std::cout << "good is valid: " << (validator.validate(good) ? "yes" : "no") << "\n";

    // --- Collect failures for a non-conforming instance -------------------
    pjson::ParseError badError;
    pjson bad = pjson::parse(R"({
        "name": "", "age": 200, "email": "nope",
        "joined": "2025-01-02", "roles": ["root"], "extra": 1
    })",
                             badError);
    if (!badError.ok) {
        std::cerr << "could not parse invalid example\n";
        return 1;
    }
    std::vector<pJsonSchemaValidator::Error> errors;
    // This overload appends applicable failures up to the configured budget;
    // each path is an RFC 6901 JSON Pointer identifying the offending value.
    bool ok = validator.validate(bad, errors);
    std::cout << "bad is valid: " << (ok ? "yes" : "no") << "\n";
    std::cout << "failures:\n";
    for (const pJsonSchemaValidator::Error& e : errors) {
        std::cout << "  " << (e.path.empty() ? "(root)" : e.path) << ": " << e.message << "\n";
    }
    return 0;
}
