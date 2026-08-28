//
// Copyright 2025 ByteDance Ltd. and/or its affiliates. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License").
//
//===----------------------------------------------------------------------===//
// 02 — Building values
//
// Shows every way to put data into a pjson value: scalars, nested objects,
// arrays (from vectors, by index, and by appending), and deep nesting.
// Referenced by docs/02-creating-json.md.
//
#include "pjson.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace ByteDance;

// Builds one document through the scalar, object, array, and nesting APIs.
int main() {
    pjson doc;

    // --- Scalars -----------------------------------------------------------
    doc["name"] = "Ada";      // const char*  -> string
    doc["active"] = true;     // bool
    doc["age"] = int64_t(36); // integers are represented explicitly as int64_t
    doc["ratio"] = double(0.5);
    doc["nickname"]; // no value assigned -> stays null

    // --- Nested objects ----------------------------------------------------
    doc["address"]["city"] = "London";
    doc["address"]["zip"] = "N1";

    // --- Arrays ------------------------------------------------------------
    // From a std::vector:
    doc["scores"] = std::vector<int64_t>({90, 82, 77});

    // By index (auto-extends, filling gaps with null):
    doc["mixed"][0] = int64_t(1);
    doc["mixed"][1] = "two";
    doc["mixed"][3] = true; // index 2 becomes null

    // By appending with += (promotes the node to an array):
    doc["tags"] += "c++";
    doc["tags"] += "json";
    doc["tags"] += std::vector<std::string>({"fast", "simple"});

    // --- Deep nesting ------------------------------------------------------
    doc["matrix"][0] = std::vector<int64_t>({1, 2, 3});
    doc["matrix"][1] = std::vector<int64_t>({4, 5, 6});

    // --- Serialization options --------------------------------------------
    // Start with pretty-print defaults, then make the relevant layout choices
    // explicit. Key ordering applies independently at every object level.
    pjson::SerializeOptions options = pjson::SerializeOptions::prettyPrinted();
    options.indentWidth = 2;
    options.indentCharacter = ' ';
    options.escapeNonAscii = false;
    options.keyOrder = pjson::SerializeOptions::AscendingKeys;
    options.maxOutputBytes = size_t(64) * 1024 * 1024;
    std::cout << doc.toString(options) << "\n";
    return 0;
}
