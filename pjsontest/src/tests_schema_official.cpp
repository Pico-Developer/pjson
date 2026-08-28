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
// Optional official draft-07 JSON-Schema-Test-Suite conformance integration.
// This harness intentionally uses an explicit
// manifest so unsupported files or groups are skipped with a concrete reason
// instead of disappearing through ad-hoc filtering.
//
#include "pjson.h"
#include "test_harness.h"

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

#ifndef PJSON_TEST_DEFAULT_JSON_SCHEMA_TEST_SUITE_DIR
#define PJSON_TEST_DEFAULT_JSON_SCHEMA_TEST_SUITE_DIR ""
#endif

namespace {

    pjson::unique_ptr parseJson(const std::string& text, pjson::ParseError* error = NULL) {
        if (error != NULL) {
            return pjson::parse(text, *error, pjson::ParseOptions());
        }
        return pjson::parse(text, pjson::ParseOptions());
    }

    // Every upstream file is either fully run, fully skipped, or filtered by named groups.
    enum ManifestMode { RunWholeFile, SkipWholeFile, RunSelectedGroups };

    // Explicit allow/skip decision for a group whose upstream description is its stable key.
    struct GroupRule {
        const char* description;
        bool enabled;
        const char* reason;
    };

    // Per-file manifest entry; `groups` is populated only for RunSelectedGroups.
    struct FileRule {
        const char* relativePath;
        ManifestMode mode;
        const char* reason;
        std::vector<GroupRule> groups;
    };

    // Accumulates execution coverage for the informational suite summary.
    struct RunSummary {
        size_t filesVisited;
        size_t filesSkipped;
        size_t groupsRun;
        size_t groupsSkipped;
        size_t casesRun;
        size_t casesSkipped;

        RunSummary()
                : filesVisited(0)
                , filesSkipped(0)
                , groupsRun(0)
                , groupsSkipped(0)
                , casesRun(0)
                , casesSkipped(0) {}
    };

    // Cross-platform suite-location and file-loading helpers.

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

    bool isRegularFile(const std::string& path) {
        if (path.empty()) {
            return false;
        }

#if defined(_WIN32)
        const DWORD attrs = GetFileAttributesA(path.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
        struct stat st;
        return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
    }

    std::string readFile(const std::string& path) {
        std::ifstream in(path.c_str(), std::ios::binary);
        if (!in) {
            return std::string();
        }

        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    std::string configuredSchemaSuiteDir() {
        const char* env = std::getenv("PJSON_JSON_SCHEMA_TEST_SUITE_DIR");
        if (env != NULL && env[0] != '\0') {
            return std::string(env);
        }
        return std::string(PJSON_TEST_DEFAULT_JSON_SCHEMA_TEST_SUITE_DIR);
    }

    std::string resolveDraft7Dir() {
        const std::string configured = configuredSchemaSuiteDir();
        if (configured.empty()) {
            return std::string();
        }

        if (isDirectory(joinPath(configured, "tests/draft7"))) {
            return joinPath(configured, "tests/draft7");
        }
        if (isDirectory(joinPath(configured, "draft7"))) {
            return joinPath(configured, "draft7");
        }
        if (isDirectory(configured)) {
            return configured;
        }
        return std::string();
    }

    // Central compatibility ledger: unsupported upstream coverage is skipped with a reason, and
    // selected-group files fail if the upstream descriptions drift away from this manifest.
    std::vector<FileRule> manifest() {
        std::vector<FileRule> rules;

        FileRule refRule;
        refRule.relativePath = "ref.json";
        refRule.mode = RunSelectedGroups;
        refRule.reason = "";
        refRule.groups.push_back(
            GroupRule{"root pointer ref", true, "supported local JSON Pointer reference"});
        refRule.groups.push_back(GroupRule{"relative pointer ref to object", true,
                                           "supported local JSON Pointer reference"});
        refRule.groups.push_back(GroupRule{"relative pointer ref to array", true,
                                           "supported local JSON Pointer reference"});
        refRule.groups.push_back(
            GroupRule{"escaped pointer ref", true, "supported escaped local JSON Pointer tokens"});
        refRule.groups.push_back(
            GroupRule{"nested refs", true, "supported nested local references"});
        refRule.groups.push_back(GroupRule{"ref overrides any sibling keywords", true,
                                           "supported draft7 $ref sibling semantics"});
        refRule.groups.push_back(GroupRule{"$ref prevents a sibling $id from changing the base uri",
                                           false, "requires relative URI and $id base resolution"});
        refRule.groups.push_back(GroupRule{"remote ref, containing refs itself", false,
                                           "requires remote schema resolution"});
        refRule.groups.push_back(GroupRule{"property named $ref that is not a reference", true,
                                           "supported ordinary instance property"});
        refRule.groups.push_back(
            GroupRule{"property named $ref, containing an actual $ref", true,
                      "supported local JSON Pointer reference in a property schema"});
        refRule.groups.push_back(GroupRule{"$ref to boolean schema true", true,
                                           "supported local reference to boolean schema"});
        refRule.groups.push_back(GroupRule{"$ref to boolean schema false", true,
                                           "supported local reference to boolean schema"});
        refRule.groups.push_back(GroupRule{"Recursive references between schemas", false,
                                           "requires cross-schema URI resolution"});
        refRule.groups.push_back(GroupRule{"refs with quote", true,
                                           "supported percent-encoded local JSON Pointer token"});
        refRule.groups.push_back(GroupRule{"Location-independent identifier", false,
                                           "requires anchor and $id resolution"});
        refRule.groups.push_back(GroupRule{"Reference an anchor with a non-relative URI", false,
                                           "requires absolute URI and anchor resolution"});
        refRule.groups.push_back(
            GroupRule{"Location-independent identifier with base URI change in subschema", false,
                      "requires nested $id base and anchor resolution"});
        refRule.groups.push_back(
            GroupRule{"naive replacement of $ref with its destination is not correct", true,
                      "supported: $ref-shaped data inside enum is not evaluated"});
        refRule.groups.push_back(GroupRule{"refs with relative uris and defs", false,
                                           "requires relative URI and $id base resolution"});
        refRule.groups.push_back(GroupRule{"relative refs with absolute uris and defs", false,
                                           "requires absolute URI and $id resolution"});
        refRule.groups.push_back(
            GroupRule{"$id must be resolved against nearest parent, not just immediate parent",
                      false, "requires nested $id base resolution"});
        refRule.groups.push_back(GroupRule{"simple URN base URI with $ref via the URN", false,
                                           "requires absolute URN resolution"});
        refRule.groups.push_back(GroupRule{"simple URN base URI with JSON pointer", true,
                                           "supported local fragment despite nonlocal root $id"});
        refRule.groups.push_back(GroupRule{"URN base URI with NSS", true,
                                           "supported local fragment despite URN root $id"});
        refRule.groups.push_back(GroupRule{"URN base URI with r-component", true,
                                           "supported local fragment despite URN root $id"});
        refRule.groups.push_back(GroupRule{"URN base URI with q-component", true,
                                           "supported local fragment despite URN root $id"});
        refRule.groups.push_back(GroupRule{"URN base URI with URN and JSON pointer ref", false,
                                           "requires absolute URN resolution"});
        refRule.groups.push_back(GroupRule{"URN base URI with URN and anchor ref", false,
                                           "requires absolute URN and anchor resolution"});
        refRule.groups.push_back(
            GroupRule{"ref to if", false, "requires absolute URI and $id resolution"});
        refRule.groups.push_back(
            GroupRule{"ref to then", false, "requires absolute URI and $id resolution"});
        refRule.groups.push_back(
            GroupRule{"ref to else", false, "requires absolute URI and $id resolution"});
        refRule.groups.push_back(GroupRule{"ref with absolute-path-reference", false,
                                           "requires URI-reference and $id base resolution"});
        refRule.groups.push_back(GroupRule{"$id with file URI still resolves pointers - *nix", true,
                                           "supported local fragment despite file URI root $id"});
        refRule.groups.push_back(GroupRule{"$id with file URI still resolves pointers - windows",
                                           true,
                                           "supported local fragment despite file URI root $id"});
        refRule.groups.push_back(GroupRule{"empty tokens in $ref json-pointer", true,
                                           "supported empty local JSON Pointer tokens"});
        rules.push_back(refRule);

        FileRule cycleRule;
        cycleRule.relativePath = "infinite-loop-detection.json";
        cycleRule.mode = RunWholeFile;
        cycleRule.reason = "supported instance/schema-pair cycle detection";
        rules.push_back(cycleRule);

        FileRule definitionsRule;
        definitionsRule.relativePath = "definitions.json";
        definitionsRule.mode = SkipWholeFile;
        definitionsRule.reason =
            "official draft7 definitions file depends on metaschema remote $ref validation";
        rules.push_back(definitionsRule);

        FileRule patternPropertiesRule;
        patternPropertiesRule.relativePath = "patternProperties.json";
        patternPropertiesRule.mode = RunWholeFile;
        patternPropertiesRule.reason = "supported keyword";
        rules.push_back(patternPropertiesRule);

        FileRule propertyNamesRule;
        propertyNamesRule.relativePath = "propertyNames.json";
        propertyNamesRule.mode = RunWholeFile;
        propertyNamesRule.reason = "supported keyword";
        rules.push_back(propertyNamesRule);

        FileRule dependenciesRule;
        dependenciesRule.relativePath = "dependencies.json";
        dependenciesRule.mode = RunWholeFile;
        dependenciesRule.reason = "supported keyword";
        rules.push_back(dependenciesRule);

        FileRule additionalPropertiesRule;
        additionalPropertiesRule.relativePath = "additionalProperties.json";
        additionalPropertiesRule.mode = RunWholeFile;
        additionalPropertiesRule.reason = "supported keyword";
        rules.push_back(additionalPropertiesRule);

        FileRule multipleOfRule;
        multipleOfRule.relativePath = "multipleOf.json";
        multipleOfRule.mode = RunWholeFile;
        multipleOfRule.reason = "supported keyword";
        rules.push_back(multipleOfRule);

        FileRule dateRule;
        dateRule.relativePath = "optional/format/date.json";
        dateRule.mode = RunWholeFile;
        dateRule.reason = "supported format";
        rules.push_back(dateRule);

        FileRule dateTimeRule;
        dateTimeRule.relativePath = "optional/format/date-time.json";
        dateTimeRule.mode = RunWholeFile;
        dateTimeRule.reason = "supported format";
        rules.push_back(dateTimeRule);

        FileRule timeRule;
        timeRule.relativePath = "optional/format/time.json";
        timeRule.mode = RunWholeFile;
        timeRule.reason = "supported format";
        rules.push_back(timeRule);

        FileRule ipv4Rule;
        ipv4Rule.relativePath = "optional/format/ipv4.json";
        ipv4Rule.mode = RunWholeFile;
        ipv4Rule.reason = "supported format";
        rules.push_back(ipv4Rule);

        FileRule ipv6Rule;
        ipv6Rule.relativePath = "optional/format/ipv6.json";
        ipv6Rule.mode = RunWholeFile;
        ipv6Rule.reason = "supported format";
        rules.push_back(ipv6Rule);

        return rules;
    }

    // Manifest and diagnostic helpers used by the execution pipeline below.
    const GroupRule* findGroupRule(const FileRule& fileRule, const std::string& description) {
        for (size_t i = 0; i < fileRule.groups.size(); ++i) {
            if (description == fileRule.groups[i].description) {
                return &fileRule.groups[i];
            }
        }
        return NULL;
    }

    std::string groupDescription(const pjson& group) {
        const pjson* desc = group.find("description");
        if (desc == NULL) {
            return std::string("<missing description>");
        }
        std::string value;
        return desc->tryGet(value) ? value : std::string("<missing description>");
    }

    std::string testDescription(const pjson& testCase) {
        const pjson* desc = testCase.find("description");
        if (desc == NULL) {
            return std::string("<missing description>");
        }
        std::string value;
        return desc->tryGet(value) ? value : std::string("<missing description>");
    }

    std::string firstErrorSummary(const std::vector<pjson::SchemaError>& errors) {
        if (errors.empty()) {
            return std::string("no schema errors reported");
        }

        std::ostringstream os;
        os << "first error";
        if (!errors[0].path.empty()) {
            os << " at " << errors[0].path;
        }
        if (!errors[0].message.empty()) {
            os << ": " << errors[0].message;
        }
        return os.str();
    }

    void recordFailure(const std::string& scope, const std::string& detail) {
        ::pjson_test::report_failure(__FILE__, __LINE__, scope.c_str(), detail);
    }

    // Runs one upstream case while preserving its file/group/case hierarchy in diagnostics.
    void runOneOfficialCase(const std::string& relativePath, const std::string& groupDesc,
                            const pjson& schema, const pjson& testCase, RunSummary& summary) {
        const pjson* data = testCase.find("data");
        const pjson* valid = testCase.find("valid");
        const std::string caseDesc = testDescription(testCase);

        ::pjson_test::current().checks += 1;
        summary.casesRun += 1;

        if (data == NULL || valid == NULL || !valid->isBool()) {
            recordFailure("official schema suite case shape",
                          relativePath + " :: " + groupDesc + " :: " + caseDesc);
            return;
        }

        bool expected = false;
        if (!valid->tryGet(expected)) {
            recordFailure("official schema suite case shape",
                          relativePath + " :: " + groupDesc + " :: " + caseDesc);
            return;
        }

        std::vector<pjson::SchemaError> errors;
        const bool actual = data->validate(schema, errors);
        if (actual == expected) {
            return;
        }

        std::ostringstream os;
        os << relativePath << " :: " << groupDesc << " :: " << caseDesc << " expected "
           << (expected ? "valid" : "invalid") << " but validator returned "
           << (actual ? "valid" : "invalid");
        if (!actual) {
            os << " [" << firstErrorSummary(errors) << "]";
        }
        recordFailure("official schema suite mismatch", os.str());
    }

    // Validates a group shape once, then runs all of its cases against the shared schema.
    void runWholeGroup(const std::string& relativePath, const pjson& group, RunSummary& summary) {
        const pjson* schema = group.find("schema");
        const pjson* tests = group.find("tests");
        const std::string groupDesc = groupDescription(group);

        if (schema == NULL || tests == NULL || !tests->isArray()) {
            recordFailure("official schema suite group shape", relativePath + " :: " + groupDesc);
            return;
        }

        const size_t count = tests->size();
        summary.groupsRun += 1;
        for (size_t i = 0; i < count; ++i) {
            const pjson* testCase = tests->find(static_cast<int>(i));
            if (testCase == NULL) {
                recordFailure("official schema suite case shape",
                              relativePath + " :: " + groupDesc + " :: index " +
                                  pjson_test::to_str(static_cast<int>(i)));
                continue;
            }
            runOneOfficialCase(relativePath, groupDesc, *schema, *testCase, summary);
        }
    }

    // Enforces a bidirectional manifest invariant: every upstream group has a rule and every rule
    // still names an upstream group. This makes suite upgrades fail visibly instead of shrinking
    // coverage silently.
    void runSelectedGroups(const FileRule& fileRule, const pjson& suiteFile, RunSummary& summary) {
        if (!suiteFile.isArray()) {
            recordFailure("official schema suite file shape",
                          std::string(fileRule.relativePath) + " did not parse to an array");
            return;
        }

        std::vector<std::string> seenDescriptions;
        for (size_t i = 0; i < suiteFile.size(); ++i) {
            const pjson* groupPtr = suiteFile.find(static_cast<int>(i));
            if (groupPtr == NULL) {
                recordFailure("official schema suite group shape",
                              std::string(fileRule.relativePath) + " :: index " +
                                  pjson_test::to_str(static_cast<int>(i)));
                continue;
            }
            const pjson& group = *groupPtr;
            const std::string description = groupDescription(group);
            seenDescriptions.push_back(description);

            const GroupRule* groupRule = findGroupRule(fileRule, description);
            if (groupRule == NULL) {
                recordFailure("official schema suite manifest gap",
                              std::string(fileRule.relativePath) + " :: " + description +
                                  " is present upstream but has no explicit run/skip rule");
                continue;
            }

            if (!groupRule->enabled) {
                summary.groupsSkipped += 1;
                const pjson* tests = group.find("tests");
                if (tests != NULL && tests->isArray()) {
                    summary.casesSkipped += tests->size();
                }
                std::printf("    INFO skip %s :: %s [%s]\n", fileRule.relativePath,
                            description.c_str(), groupRule->reason);
                continue;
            }

            runWholeGroup(fileRule.relativePath, group, summary);
        }

        for (size_t i = 0; i < fileRule.groups.size(); ++i) {
            if (std::find(seenDescriptions.begin(), seenDescriptions.end(),
                          std::string(fileRule.groups[i].description)) == seenDescriptions.end()) {
                recordFailure("official schema suite manifest stale",
                              std::string(fileRule.relativePath) +
                                  " :: " + fileRule.groups[i].description +
                                  " is declared in the manifest but was not found in the suite");
            }
        }
    }

    // Runs every group in a file whose supported vocabulary needs no per-group filtering.
    void runWholeFile(const FileRule& fileRule, const pjson& suiteFile, RunSummary& summary) {
        if (!suiteFile.isArray()) {
            recordFailure("official schema suite file shape",
                          std::string(fileRule.relativePath) + " did not parse to an array");
            return;
        }

        for (size_t i = 0; i < suiteFile.size(); ++i) {
            const pjson* group = suiteFile.find(static_cast<int>(i));
            if (group == NULL) {
                recordFailure("official schema suite group shape",
                              std::string(fileRule.relativePath) + " :: index " +
                                  pjson_test::to_str(static_cast<int>(i)));
                continue;
            }
            runWholeGroup(fileRule.relativePath, *group, summary);
        }
    }

} // namespace

TEST(schema_official_draft7_optional) {
    const std::string draft7Dir = resolveDraft7Dir();
    if (draft7Dir.empty()) {
        std::printf("    INFO JSON-Schema-Test-Suite skipped; set "
                    "PJSON_JSON_SCHEMA_TEST_SUITE_DIR or run "
                    "scripts/fetch-json-schema-test-suite.sh\n");
        CHECK(true);
        return;
    }

    RunSummary summary;
    const std::vector<FileRule> rules = manifest();
    for (size_t i = 0; i < rules.size(); ++i) {
        const std::string path = joinPath(draft7Dir, rules[i].relativePath);
        summary.filesVisited += 1;

        if (!isRegularFile(path)) {
            recordFailure("official schema suite file missing",
                          std::string(rules[i].relativePath) + " under " + draft7Dir);
            continue;
        }

        if (rules[i].mode == SkipWholeFile) {
            summary.filesSkipped += 1;
            std::printf("    INFO skip %s [%s]\n", rules[i].relativePath, rules[i].reason);
            continue;
        }

        pjson::ParseError parseError;
        pjson::unique_ptr suite = parseJson(readFile(path), &parseError);
        if (!suite) {
            std::ostringstream os;
            os << rules[i].relativePath << " failed to parse";
            if (!parseError.message.empty()) {
                os << " at byte " << parseError.offset << ": " << parseError.message;
            }
            recordFailure("official schema suite parse", os.str());
            continue;
        }

        if (rules[i].mode == RunWholeFile) {
            runWholeFile(rules[i], *suite, summary);
        } else {
            runSelectedGroups(rules[i], *suite, summary);
        }
    }

    std::printf("    INFO official schema suite visited %llu files (%llu whole-file skips), "
                "ran %llu groups / %llu cases, skipped %llu groups / %llu cases\n",
                static_cast<unsigned long long>(summary.filesVisited),
                static_cast<unsigned long long>(summary.filesSkipped),
                static_cast<unsigned long long>(summary.groupsRun),
                static_cast<unsigned long long>(summary.casesRun),
                static_cast<unsigned long long>(summary.groupsSkipped),
                static_cast<unsigned long long>(summary.casesSkipped));
}
