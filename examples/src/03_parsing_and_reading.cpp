//
// Copyright 2025 ByteDance Ltd. and/or its affiliates. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License").
//
//===----------------------------------------------------------------------===//
// 03 — Parsing and reading
//
// Parse a JSON string, then read scalars, arrays, and nested objects safely.
// Referenced by docs/03-parsing-and-reading.md.
//
#include "pjson.h"

#include <iostream>

using namespace ByteDance;

// Parses a representative document and demonstrates non-mutating read APIs.
int main() {
    // --- Parse the input ---------------------------------------------------
    const char* text = R"({
        "name": "Ada",
        "age": 36,
        "scores": [90, 82, 77],
        "address": { "city": "London" },
        "friends": [ {"name":"Bob"}, {"name":"Cid"} ]
    })";

    // Every DOM parse overload returns a pjson value; pass a ParseError to
    // learn whether parsing succeeded.
    pjson::ParseError parseError;
    pjson doc = pjson::parse(text, parseError);
    if (!parseError.ok) {
        std::cerr << parseError.line << ':' << parseError.column << ": " << parseError.message
                  << "\n";
        return 1;
    }
    const pjson& j = doc;

    // --- Strict scalar reads ----------------------------------------------
    // StringView borrows the stored bytes, so write the explicit length rather
    // than assuming a null terminator. The view remains valid while j is alive
    // and the underlying string is not modified.
    pjson::StringView name;
    int64_t age = 0;
    if (j.tryGet("name", name)) {
        std::cout << "name  = ";
        std::cout.write(name.data(), static_cast<std::streamsize>(name.size()));
        std::cout << "\n";
    }
    if (j.tryGet("age", age))
        std::cout << "age   = " << age << "\n";

    // --- Safe reads with an application default ---------------------------
    std::string email = "(none)";
    j.tryGet("email", email); // failure leaves the existing value unchanged
    std::cout << "email = " << email << "\n";

    // --- Arrays: iterate through non-vivifying lookup ----------------------
    std::cout << "scores:";
    const pjson* scoresNode = j.find("scores");
    if (scoresNode && scoresNode->isArray()) {
        for (size_t i = 0; i < scoresNode->size(); ++i) {
            int64_t value = 0;
            const pjson* score = scoresNode->find(static_cast<int>(i));
            if (score && score->tryGet(value))
                std::cout << " " << value;
        }
    }
    std::cout << "\n";

    // --- Non-vivifying indexed reads (negative means from the end) --------
    int64_t lastScore = 0;
    if (scoresNode && scoresNode->hasIndex(-1) && scoresNode->tryGet(-1, lastScore))
        std::cout << "last score = " << lastScore << "\n";

    int64_t firstScore = 0;
    if (scoresNode && scoresNode->tryGet(0, firstScore))
        std::cout << "first score = " << firstScore << "\n";

    // --- Array of objects --------------------------------------------------
    // Nested find()/tryGet() calls keep this traversal read-only and skip any
    // element that does not have a string-valued "name" member.
    std::cout << "friends:";
    if (const pjson* friendsNode = j.find("friends")) {
        if (friendsNode->isArray()) {
            for (size_t i = 0; i < friendsNode->size(); ++i) {
                const pjson* friend_ = friendsNode->find(static_cast<int>(i));
                pjson::StringView friendName;
                if (friend_ && friend_->tryGet("name", friendName)) {
                    std::cout << " ";
                    std::cout.write(friendName.data(),
                                    static_cast<std::streamsize>(friendName.size()));
                }
            }
        }
    }
    std::cout << "\n";

    // --- Nested lookup with RFC 6901 JSON Pointer -------------------------
    pjson::PointerError pointerError;
    if (const pjson* city = j.findPointer("/address/city", pointerError)) {
        std::string cityName;
        if (city->tryGet(cityName))
            std::cout << "city  = " << cityName << "\n";
    } else {
        std::cerr << "pointer lookup failed: " << pointerError.message << "\n";
    }
    return 0;
}
