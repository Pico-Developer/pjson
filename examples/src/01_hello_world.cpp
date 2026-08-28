//
// Copyright 2025 ByteDance Ltd. and/or its affiliates. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License").
//
//===----------------------------------------------------------------------===//
// 01 — Hello, World
//
// The smallest possible pjson program: build a tiny object and print it.
// Referenced by docs/01-getting-started.md.
//
#include "pjson.h"

#include <cstdint>
#include <iostream>

using namespace ByteDance;

// Constructs a minimal document and prints its compact and pretty forms.
int main() {
    // --- Build the document ------------------------------------------------
    // A pjson value starts as null. Assigning to a key turns it into an object.
    pjson greeting;
    greeting["message"] = "Hello, World!";
    greeting["year"] = int64_t(2025);

    // --- Serialize it ------------------------------------------------------
    pjson::SerializeOptions compact;
    pjson::SerializeOptions pretty = pjson::SerializeOptions::prettyPrinted();
    std::cout << greeting.toString(compact) << "\n";
    std::cout << greeting.toString(pretty) << "\n";
    return 0;
}
