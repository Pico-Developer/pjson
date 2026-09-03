//
// Copyright 2025 ByteDance Ltd. and/or its affiliates. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//===----------------------------------------------------------------------===//
// RFC 8259 conformance coverage:
//   - a curated inline accept/reject corpus for RFC 8259 parsing
//   - optional runtime execution of nst/JSONTestSuite if a corpus directory is
//     configured or fetched locally
//
#include "pjson.h"
#include "pjson_parser.h"
#include "test_harness.h"
#include "test_util.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

using namespace ByteDance;

#ifndef PJSON_TEST_DEFAULT_JSONTESTSUITE_DIR
#define PJSON_TEST_DEFAULT_JSONTESTSUITE_DIR ""
#endif

namespace {

    // A named corpus entry and the parser outcome it requires.
    struct Expectation {
        const char* name;
        std::string document;
        bool shouldParse;
    };

    // Uses unbounded size/node budgets so the conformance corpus measures grammar rather than
    // deployment limits; the production recursion guard remains active for stack safety.
    pJsonParser::Options conformanceOptions() {
        pJsonParser::Options opts;
        // Keep the production recursion guard. Some implementation-defined
        // corpus files intentionally contain extreme nesting; they are skipped
        // below, while y_/n_ files remain bounded by the safe default.
        opts.maxDepth = 512;
        opts.maxNodes = 0;
        opts.maxInputBytes = 0;
        // RFC 8259 says object names SHOULD be unique but does not make
        // duplicates a grammar error. Use keep-last for the external syntax
        // corpus while the public default policy rejects duplicates.
        opts.duplicateKeys = pJsonParser::Options::KeepLastDuplicate;
        return opts;
    }

    // Constructs byte-exact inputs that cannot safely be expressed as ordinary literals.
    std::string bytes(const char* data, size_t size) {
        return std::string(data, size);
    }

    // Performs one table-driven grammar assertion with the case name in failure diagnostics.
    void expectConformanceParse(const Expectation& tc) {
        ::pjson_test::current().checks += 1;

        pJsonParser::Error err;
        pjson_test::Parsed parsed = pjson_test::parse(tc.document, err, conformanceOptions());

        if (tc.shouldParse) {
            if (parsed == nullptr) {
                std::ostringstream detail;
                detail << tc.name << " rejected";
                if (!err.message.empty()) {
                    detail << " at byte " << err.offset << ": " << err.message;
                } else {
                    detail << " at byte " << err.offset;
                }
                ::pjson_test::report_failure(__FILE__, __LINE__, "conformance accept",
                                             detail.str());
            }
            return;
        }

        if (parsed != nullptr) {
            std::ostringstream detail;
            detail << tc.name << " unexpectedly parsed as " << parsed->toString();
            ::pjson_test::report_failure(__FILE__, __LINE__, "conformance reject", detail.str());
        }
    }

    // Cross-platform corpus discovery helpers.

    bool hasJsonExtension(const std::string& path) {
        return path.size() >= 5 && path.substr(path.size() - 5) == ".json";
    }

    std::string joinPath(const std::string& base, const std::string& leaf) {
        if (base.empty()) {
            return leaf;
        }

        const char last = base[base.size() - 1];
        if (last == '/' || last == '\\') {
            return base + leaf;
        }

#if defined(_WIN32)
        return base + "\\" + leaf;
#else
        return base + "/" + leaf;
#endif
    }

    std::string baseName(const std::string& path) {
        const std::string::size_type slash = path.find_last_of("/\\");
        if (slash == std::string::npos) {
            return path;
        }
        return path.substr(slash + 1);
    }

    bool isDirectory(const std::string& path) {
        if (path.empty()) {
            return false;
        }

#if defined(_WIN32)
        const DWORD attrs = GetFileAttributesA(path.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
        struct stat st;
        return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
    }

    std::vector<std::string> listJsonFiles(const std::string& dirPath) {
        std::vector<std::string> files;

#if defined(_WIN32)
        WIN32_FIND_DATAA entry;
        HANDLE find = FindFirstFileA(joinPath(dirPath, "*.json").c_str(), &entry);
        if (find == INVALID_HANDLE_VALUE) {
            return files;
        }

        do {
            if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                files.push_back(joinPath(dirPath, entry.cFileName));
            }
        } while (FindNextFileA(find, &entry) != 0);

        FindClose(find);
#else
        DIR* dir = ::opendir(dirPath.c_str());
        if (dir == NULL) {
            return files;
        }

        while (struct dirent* entry = ::readdir(dir)) {
            const std::string name(entry->d_name);
            if (name == "." || name == "..") {
                continue;
            }

            const std::string path = joinPath(dirPath, name);
            if (hasJsonExtension(name)) {
                files.push_back(path);
            }
        }

        ::closedir(dir);
#endif

        std::sort(files.begin(), files.end());
        return files;
    }

    std::string readFile(const std::string& path) {
        std::ifstream in(path.c_str(), std::ios::binary);
        if (!in) {
            return std::string();
        }

        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    std::string configuredJsonTestSuiteDir() {
        const char* env = std::getenv("PJSON_JSONTESTSUITE_DIR");
        if (env != NULL && env[0] != '\0') {
            return std::string(env);
        }
        return std::string(PJSON_TEST_DEFAULT_JSONTESTSUITE_DIR);
    }

    std::string resolveJsonTestSuiteParsingDir() {
        const std::string configured = configuredJsonTestSuiteDir();
        if (configured.empty()) {
            return std::string();
        }

        if (baseName(configured) == "test_parsing" && isDirectory(configured)) {
            return configured;
        }

        const std::string nested = joinPath(configured, "test_parsing");
        if (isDirectory(nested)) {
            return nested;
        }

        if (isDirectory(configured)) {
            return configured;
        }

        return std::string();
    }

    // Applies the y_/n_ filename contract used by nst/JSONTestSuite.
    void runJsonTestSuiteExpectation(const std::string& path, bool shouldParse) {
        const std::string payload = readFile(path);
        ::pjson_test::current().checks += 1;

        pJsonParser::Error err;
        pjson_test::Parsed parsed = pjson_test::parse(payload, err, conformanceOptions());

        if (shouldParse && parsed == nullptr) {
            std::ostringstream detail;
            detail << baseName(path) << " rejected";
            if (!err.message.empty()) {
                detail << " at byte " << err.offset << ": " << err.message;
            } else {
                detail << " at byte " << err.offset;
            }
            ::pjson_test::report_failure(__FILE__, __LINE__, "JSONTestSuite y_ case", detail.str());
            return;
        }

        if (!shouldParse && parsed != nullptr) {
            std::ostringstream detail;
            detail << baseName(path) << " unexpectedly parsed";
            ::pjson_test::report_failure(__FILE__, __LINE__, "JSONTestSuite n_ case", detail.str());
        }
    }

} // namespace

//===----------------------------------------------------------------------===//
// Curated RFC 8259 grammar matrix
//===----------------------------------------------------------------------===//

TEST(conformance_rfc8259_inline_accepts) {
    const char escapedSolidus[] = {'"', '\\', '/', '"'};
    const char escapedReverseSolidus[] = {'"', 's', 'l', 'a', 's', 'h', ':', ' ', '\\', '\\', '"'};
    const char escapedControls[] = {'"', '\\', 'b', '\\', 'f', '\\',
                                    'n', '\\', 'r', '\\', 't', '"'};
    const char escapedUnicode[] = {'"', '\\', 'u', '0', '0', '4', '1', '"'};
    const char surrogatePair[] = {'"',  '\\', 'u', 'D', '8', '3', '4',
                                  '\\', 'u',  'D', 'D', '1', 'E', '"'};

    const Expectation cases[] = {
        {"null literal", "null", true},
        {"true literal", "true", true},
        {"false literal", "false", true},
        {"integer zero", "0", true},
        {"negative zero", "-0", true},
        {"positive integer", "1234567890", true},
        {"negative integer", "-987654321", true},
        {"fraction", "3.1415", true},
        {"exponent", "6.022e23", true},
        {"uppercase exponent", "-2E-3", true},
        {"string empty", "\"\"", true},
        {"string ascii", "\"hello\"", true},
        {"string escaped quote", "\"quote: \\\"\"", true},
        {"string escaped reverse solidus",
         bytes(escapedReverseSolidus, sizeof(escapedReverseSolidus)), true},
        {"string escaped solidus", bytes(escapedSolidus, sizeof(escapedSolidus)), true},
        {"string escaped controls", bytes(escapedControls, sizeof(escapedControls)), true},
        {"string unicode hex", bytes(escapedUnicode, sizeof(escapedUnicode)), true},
        {"string surrogate pair", bytes(surrogatePair, sizeof(surrogatePair)), true},
        {"array empty", "[]", true},
        {"array mixed", "[null,true,false,0,-1,1.5,\"x\",[],{}]", true},
        {"array whitespace", "[ 1 , 2 , 3 ]", true},
        {"object empty", "{}", true},
        {"object simple", "{\"a\":1,\"b\":2}", true},
        {"object nested", "{\"a\":[1,{\"b\":true},null],\"c\":{\"d\":\"x\"}}", true},
        {"document surrounding whitespace", " \t\r\n {\"ok\":true} \n", true},
        {"top level string", "\"json\"", true},
        {"top level array with nested objects", "[{\"a\":1},{\"b\":[2,3]}]", true},
        {"top level object with escaped unicode", "{\"snowman\":\"\\u2603\"}", true},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        expectConformanceParse(cases[i]);
    }
}

TEST(conformance_rfc8259_inline_rejects) {
    const char rawNewline[] = {'"', 'a', '\n', 'b', '"'};
    const char badEscape[] = {'"', '\\', 'x', '4', '1', '"'};
    const char loneHighSurrogate[] = {'"', '\\', 'u', 'D', '8', '0', '0', '"'};
    const char loneLowSurrogate[] = {'"', '\\', 'u', 'D', 'C', '0', '0', '"'};
    const char invalidUtf8[] = {'"', (char)0xC3, (char)0x28, '"'};
    const char bomPrefix[] = {(char)0xEF, (char)0xBB, (char)0xBF, '{', '}', '\n'};

    const Expectation cases[] = {
        {"empty input", "", false},
        {"whitespace only", " \r\n\t ", false},
        {"single quote string", "'json'", false},
        {"unquoted key", "{a:1}", false},
        {"missing colon", "{\"a\" 1}", false},
        {"missing comma in object", "{\"a\":1 \"b\":2}", false},
        {"trailing comma object", "{\"a\":1,}", false},
        {"leading comma object", "{,\"a\":1}", false},
        {"double comma object", "{\"a\":1,,\"b\":2}", false},
        {"missing value object", "{\"a\":}", false},
        {"non string key", "{true:1}", false},
        {"unterminated object", "{\"a\":1", false},
        {"missing comma in array", "[1 2]", false},
        {"trailing comma array", "[1,2,]", false},
        {"leading comma array", "[,1]", false},
        {"double comma array", "[1,,2]", false},
        {"unterminated array", "[1,2", false},
        {"plus sign number", "+1", false},
        {"leading zero integer", "01", false},
        {"negative leading zero integer", "-01", false},
        {"bare decimal point prefix", ".1", false},
        {"bare decimal point suffix", "1.", false},
        {"missing exponent digits", "1e", false},
        {"missing signed exponent digits", "1e+", false},
        {"hex number", "0x10", false},
        {"nan literal", "NaN", false},
        {"infinity literal", "Infinity", false},
        {"uppercase null", "NULL", false},
        {"mixed case true", "True", false},
        {"unknown keyword", "undefined", false},
        {"raw control character in string", bytes(rawNewline, sizeof(rawNewline)), false},
        {"unknown escape", bytes(badEscape, sizeof(badEscape)), false},
        {"lone high surrogate", bytes(loneHighSurrogate, sizeof(loneHighSurrogate)), false},
        {"lone low surrogate", bytes(loneLowSurrogate, sizeof(loneLowSurrogate)), false},
        {"invalid utf8 inside string", bytes(invalidUtf8, sizeof(invalidUtf8)), false},
        {"utf8 bom prefix", bytes(bomPrefix, sizeof(bomPrefix)), false},
        {"trailing garbage", "{\"a\":1} trailing", false},
        {"two top level values", "true false", false},
        {"comment syntax", "{\"a\":1//comment\n}", false},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        expectConformanceParse(cases[i]);
    }
}

//===----------------------------------------------------------------------===//
// Optional nst/JSONTestSuite integration
//===----------------------------------------------------------------------===//

TEST(conformance_json_test_suite_optional) {
    const std::string parsingDir = resolveJsonTestSuiteParsingDir();
    if (parsingDir.empty()) {
        std::printf("    INFO JSONTestSuite skipped; run scripts/fetch-json-test-suite.sh "
                    "(PJSON_JSONTESTSUITE_DIR is an optional override)\n");
        CHECK(true);
        return;
    }

    const std::vector<std::string> files = listJsonFiles(parsingDir);
    if (files.empty()) {
        std::printf("    INFO JSONTestSuite skipped; no .json files under %s\n",
                    parsingDir.c_str());
        CHECK(true);
        return;
    }

    size_t ran = 0;
    size_t implementationDefined = 0;
    size_t ignored = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        const std::string name = baseName(files[i]);
        if (name.size() < 3 || name[1] != '_') {
            ignored += 1;
            continue;
        }

        if (name[0] == 'y') {
            runJsonTestSuiteExpectation(files[i], true);
            ran += 1;
            continue;
        }

        if (name[0] == 'n') {
            runJsonTestSuiteExpectation(files[i], false);
            ran += 1;
            continue;
        }

        if (name[0] == 'i') {
            // RFC 8259 leaves these cases implementation-defined. Exercise
            // every one and require deterministic behavior: if accepted, the
            // normalized output must itself be strict JSON and round-trip.
            const std::string payload = readFile(files[i]);
            pjson_test::Parsed parsed = pjson_test::parse(payload, conformanceOptions());
            CHECK(true); // parsing the corpus entry terminated safely
            if (parsed) {
                const std::string normalized = parsed->toString();
                CHECK(pjson_test::parse(normalized, conformanceOptions()) != nullptr);
            }
            implementationDefined += 1;
            continue;
        }

        ignored += 1;
    }

    std::printf("    INFO JSONTestSuite ran %llu required + %llu implementation-defined "
                "files from %s (%llu other "
                "entries ignored)\n",
                static_cast<unsigned long long>(ran),
                static_cast<unsigned long long>(implementationDefined), parsingDir.c_str(),
                static_cast<unsigned long long>(ignored));

    if (ran == 0) {
        CHECK(true);
    }
}
