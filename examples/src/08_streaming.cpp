//
// Copyright 2025 ByteDance Ltd. and/or its affiliates. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License").
//
//===----------------------------------------------------------------------===//
// 08 — Process a JSON stream without building an in-memory document.
// Referenced by docs/11-streaming.md.
//
#include "pjson.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <sstream>

using namespace ByteDance;

// SAX handler that summarizes every JSON number encountered anywhere in the
// stream. Unoverridden callbacks accept and ignore non-numeric events.
struct NumberSummary : pjson::SaxHandler {
    size_t count = 0;
    double total = 0.0;

    // Count an integer event and continue parsing.
    bool onInt(int64_t value) override {
        ++count;
        total += static_cast<double>(value);
        return true;
    }

    // Count a floating-point event and continue parsing.
    bool onDouble(double value) override {
        ++count;
        total += value;
        return true;
    }
};

// Streams an input through the SAX summary, then streams a small DOM report to
// standard output without constructing either whole-document output string.
int main() {
    // --- Incremental input -------------------------------------------------
    std::istringstream input(R"({"readings":[10,12.5,8,9.5]})");
    NumberSummary summary;
    pjson::ParseError error;
    if (!pjson::parseSaxStream(input, summary, error)) {
        std::cerr << error.line << ':' << error.column << ": " << error.message << '\n';
        return 1;
    }

    std::cout << "numbers: " << summary.count << "\nsum: " << summary.total << '\n';

    // --- Streaming output -------------------------------------------------
    // write(out, options) serializes an existing DOM without first building a
    // complete output string. Check the stream after the void-returning call.
    pjson report;
    report["numbers"] = static_cast<int64_t>(summary.count);
    report["sum"] = summary.total;
    pjson::SerializeOptions options = pjson::SerializeOptions::prettyPrinted();
    options.indentWidth = 2;
    options.escapeNonAscii = true;
    options.maxOutputBytes = size_t(64) * 1024 * 1024;
    report.write(std::cout, options);
    std::cout << '\n';
    if (!std::cout)
        return 1;
    return 0;
}
