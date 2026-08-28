// SPDX-License-Identifier: Apache-2.0

#include <pjson.h>

#include <cstring>
#include <iostream>
#include <string>

// ---- Installed-package consumer smoke test -----------------------------

// Verifies that an external C++11 consumer sees coherent headers, version
// metadata, linkage, parsing, typed access, and compact serialization.
int main() {
    using ByteDance::pjson;

    // The public macro and linked library function must identify the same
    // release; this also detects stale headers paired with a different binary.
    if (std::strcmp(PJSON_VERSION, "1.0.0") != 0 ||
        std::strcmp(pjson::getVersion(), "1.0.0") != 0) {
        std::cerr << "unexpected pjson version" << std::endl;
        return 1;
    }

    // A compact round trip covers the main installed API without relying on
    // any source-tree-only headers or test helpers.
    pjson::unique_ptr document = pjson::parse("{\"answer\":42}");
    int64_t answer = 0;
    if (!document || !document->tryGet("answer", answer) || answer != 42 ||
        document->toString() != "{\"answer\":42}") {
        std::cerr << "installed pjson failed its consumer smoke test" << std::endl;
        return 1;
    }

    return 0;
}
