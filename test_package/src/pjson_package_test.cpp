// SPDX-License-Identifier: Apache-2.0

#include <pjson.h>

#include <string>

// ---- Conan package consumer smoke test ---------------------------------

// Exercises construction, serialization, parsing, lookup, typed access, and
// version metadata using only the headers and library exported by the package.
int main() {
    ByteDance::pjson value;
    value["packaged"] = true;

    const ByteDance::pjson::unique_ptr parsed = ByteDance::pjson::parse(value.toString());
    bool packaged = false;
    // A successful package preserves the sentinel property through a round
    // trip and keeps the public header macro in sync with the linked library.
    return parsed && parsed->tryGet("packaged", packaged) && packaged &&
                   std::string(ByteDance::pjson::getVersion()) == PJSON_VERSION
               ? 0
               : 1;
}
