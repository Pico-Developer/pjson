//
// Copyright 2025 ByteDance Ltd. and/or its affiliates. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License").
//
//===----------------------------------------------------------------------===//
// 04 — Editing an existing document
//
// Load a document, then change values, add and remove keys and array
// elements, and re-serialize. Referenced by docs/04-editing.md.
//
#include "pjson.h"

#include <iostream>
#include <vector>

using namespace ByteDance;

// Parses a seed document, mutates it through several APIs, and prints the result.
int main() {
    // --- Parse a mutable document -----------------------------------------
    auto doc = pjson::parse(R"({
        "user": { "name": "Ada", "roles": ["admin", "dev"] },
        "count": 2,
        "deprecated": true
    })");
    if (!doc) {
        std::cerr << "parse failed\n";
        return 1;
    }
    pjson& j = *doc;

    // --- Direct DOM edits -------------------------------------------------
    // Change a value in place.
    j["user"]["name"] = "Ada Lovelace";

    // Add a new nested value.
    j["user"]["email"] = "ada@example.com";

    // Append to an existing array.
    j["user"]["roles"] += "owner";

    // Change an element's type (arrays are heterogeneous).
    j["count"] = "two";

    // Remove a key and an array element.
    j.erase("deprecated");
    j["user"]["roles"].erase(size_t(0)); // drop "admin"

    // --- Standards-based transformations ---------------------------------
    // Apply a sequence of JSON Pointer edits atomically (RFC 6902): the test
    // must succeed before the reviewer role is appended.
    pjson::ParseError parseError;
    auto patch = pjson::parse(R"([
        {"op":"test", "path":"/count", "value":"two"},
        {"op":"add", "path":"/user/roles/-", "value":"reviewer"}
    ])",
                              parseError);
    if (!patch) {
        std::cerr << "could not parse patch: " << parseError.message << "\n";
        return 1;
    }
    pjson::PatchError error;
    pjson::PatchOptions limits;
    if (!j.applyPatch(*patch, error, limits)) {
        std::cerr << "patch failed at operation " << error.opIndex << ": " << error.message << "\n";
        return 1;
    }

    // Merge Patch recursively updates objects; null removes an object member.
    // Removing a missing member, as here, is a successful no-op.
    auto merge = pjson::parse(R"({"user":{"nickname":null}})", parseError);
    if (!merge) {
        std::cerr << "could not parse merge patch: " << parseError.message << "\n";
        return 1;
    }
    if (!j.applyMergePatch(*merge, error, limits)) {
        std::cerr << "merge patch failed: " << error.message << "\n";
        return 1;
    }

    // --- Serialize the edited document -----------------------------------
    pjson::SerializeOptions output = pjson::SerializeOptions::prettyPrinted();
    output.maxOutputBytes = size_t(64) * 1024 * 1024;
    std::cout << j.toString(output) << "\n";
    return 0;
}
