//
// Copyright 2025 ByteDance Ltd. and/or its affiliates. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License").
//
//===----------------------------------------------------------------------===//
// 05 — Parsing and error reporting
//
// Show RFC 8259 parsing, duplicate-key policies, resource budgets, and
// precise failure locations.
// Referenced by docs/05-parsing-and-errors.md.
//
#include "pjson.h"

#include <iostream>

using namespace ByteDance;

// Attempts one parse and prints either its compact form or the precise failure
// location. Reporting parse APIs reset ParseError on entry.
static void tryParse(const char* label, const std::string& text, const pjson::ParseOptions& opt) {
    pjson::ParseError err;
    pjson doc = pjson::parse(text, err, opt);
    std::cout << label << ": ";
    if (err.ok) {
        pjson::SerializeOptions compact;
        compact.maxOutputBytes = size_t(64) * 1024 * 1024;
        std::cout << "OK -> " << doc.toString(compact) << "\n";
    } else {
        std::cout << "FAILED at " << err.line << ':' << err.column << " (byte " << err.offset
                  << ", " << err.message << ")\n";
    }
}

// Exercises invalid syntax, duplicate-key policy, and a nesting-depth budget.
int main() {
    // --- JSON syntax -------------------------------------------------------
    pjson::ParseOptions defaults;
    tryParse("trailing comma", "[1, 2, ]", defaults);
    tryParse("uppercase keyword", "NULL", defaults);
    std::string rawTab = "\"a\tb\"";
    tryParse("raw tab", rawTab, defaults);

    // Duplicate policy does not relax the JSON grammar.
    tryParse("duplicate (reject)", R"({"id":1,"id":2})", defaults);
    pjson::ParseOptions keepLast;
    keepLast.duplicateKeys = pjson::ParseOptions::KeepLastDuplicate;
    tryParse("duplicate (keep last)", R"({"id":1,"id":2})", keepLast);

    // --- Resource limits --------------------------------------------------
    // Guard against runaway nesting.
    pjson::ParseOptions shallow;
    shallow.maxDepth = 3;
    tryParse("deep nesting (maxDepth=3)", "[[[[1]]]]", shallow);
    return 0;
}
