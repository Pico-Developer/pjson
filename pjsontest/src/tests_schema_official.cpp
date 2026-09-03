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
// Optional official Draft 7 and Draft 2020-12 JSON-Schema-Test-Suite
// integration. Explicit manifests record every selected run/skip decision so
// unsupported files or groups cannot disappear through ad-hoc filtering.
//
#include "pjson.h"
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

#ifndef PJSON_TEST_DEFAULT_JSON_SCHEMA_TEST_SUITE_DIR
#define PJSON_TEST_DEFAULT_JSON_SCHEMA_TEST_SUITE_DIR ""
#endif

namespace {

    pjson_test::Parsed parseJson(const std::string& text, pjson::ParseError* error = NULL) {
        if (error != NULL) {
            return pjson_test::parse(text, *error, pjson::ParseOptions());
        }
        return pjson_test::parse(text, pjson::ParseOptions());
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

    void listJsonFiles(const std::string& root, const std::string& relative,
                       std::vector<std::string>& output) {
        const std::string directory = relative.empty() ? root : joinPath(root, relative);
#if defined(_WIN32)
        WIN32_FIND_DATAA entry;
        const std::string pattern = joinPath(directory, "*");
        HANDLE handle = FindFirstFileA(pattern.c_str(), &entry);
        if (handle == INVALID_HANDLE_VALUE)
            return;
        do {
            const std::string name = entry.cFileName;
            if (name == "." || name == "..")
                continue;
            const std::string child = relative.empty() ? name : relative + "/" + name;
            if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                listJsonFiles(root, child, output);
            else if (name.size() >= 5 && name.substr(name.size() - 5) == ".json")
                output.push_back(child);
        } while (FindNextFileA(handle, &entry));
        FindClose(handle);
#else
        DIR* handle = ::opendir(directory.c_str());
        if (handle == NULL)
            return;
        while (dirent* entry = ::readdir(handle)) {
            const std::string name = entry->d_name;
            if (name == "." || name == "..")
                continue;
            const std::string child = relative.empty() ? name : relative + "/" + name;
            const std::string path = joinPath(root, child);
            if (isDirectory(path))
                listJsonFiles(root, child, output);
            else if (isRegularFile(path) && name.size() >= 5 &&
                     name.substr(name.size() - 5) == ".json")
                output.push_back(child);
        }
        ::closedir(handle);
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

    struct OfficialResolverContext {
        std::string remoteRoot;
    };

    bool resolveOfficialSchema(const std::string& uri, pjson& output, void* opaque) {
        OfficialResolverContext& context = *static_cast<OfficialResolverContext*>(opaque);
        const std::string prefix = "http://localhost:1234/";
        if (uri.compare(0, prefix.size(), prefix) != 0)
            return false;
        const std::string relative = uri.substr(prefix.size());
        const std::string path = joinPath(context.remoteRoot, relative);
        if (!isRegularFile(path))
            return false;
        pjson::ParseError error;
        output = pjson::parse(readFile(path), error);
        if (error.ok && output.isObject())
            output.erase("$schema");
        return error.ok;
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

    // Draft 2020-12 lives under tests/draft2020-12 in the same pinned corpus. It
    // shares the manifest-driven runner; only the directory and ledger differ.
    std::string resolveDraft2020Dir() {
        const std::string configured = configuredSchemaSuiteDir();
        if (configured.empty()) {
            return std::string();
        }
        if (isDirectory(joinPath(configured, "tests/draft2020-12"))) {
            return joinPath(configured, "tests/draft2020-12");
        }
        if (isDirectory(joinPath(configured, "draft2020-12"))) {
            return joinPath(configured, "draft2020-12");
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

    // Draft 2020-12 conformance ledger. Supported keyword files run whole; the
    // remaining custom-meta-schema, Unicode \\p{} regex, and annotation-only
    // format cases are skipped with a concrete reason.
    std::vector<FileRule> manifest2020() {
        std::vector<FileRule> rules;
        FileRule r;
        r = FileRule();
        r.relativePath = "additionalProperties.json";
        r.mode = RunWholeFile;
        r.reason = "supported documented-subset keywords";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "allOf.json";
        r.mode = RunWholeFile;
        r.reason = "supported documented-subset keywords";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "anchor.json";
        r.mode = RunWholeFile;
        r.reason = "requires $anchor plus $id base resolution";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "anyOf.json";
        r.mode = RunWholeFile;
        r.reason = "supported documented-subset keywords";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "boolean_schema.json";
        r.mode = RunWholeFile;
        r.reason = "supported documented-subset keywords";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "const.json";
        r.mode = RunWholeFile;
        r.reason = "supported documented-subset keywords";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "contains.json";
        r.mode = RunWholeFile;
        r.reason = "supported documented-subset keywords";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "content.json";
        r.mode = RunWholeFile;
        r.reason = "supported documented-subset keywords";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "default.json";
        r.mode = RunWholeFile;
        r.reason = "supported documented-subset keywords";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "defs.json";
        r.mode = SkipWholeFile;
        r.reason = "requires metaschema remote $ref validation";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "dependentRequired.json";
        r.mode = RunWholeFile;
        r.reason = "supported documented-subset keywords";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "dependentSchemas.json";
        r.mode = RunWholeFile;
        r.reason = "supported documented-subset keywords";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "dynamicRef.json";
        r.mode = RunSelectedGroups;
        r.reason = "";
        r.groups.push_back(GroupRule{"A $dynamicRef to a $dynamicAnchor in the same schema "
                                     "resource behaves like a normal $ref to an $anchor",
                                     true, "supported"});
        r.groups.push_back(GroupRule{"A $dynamicRef to an $anchor in the same schema resource "
                                     "behaves like a normal $ref to an $anchor",
                                     true, "supported"});
        r.groups.push_back(GroupRule{"A $ref to a $dynamicAnchor in the same schema resource "
                                     "behaves like a normal $ref to an $anchor",
                                     true, "supported"});
        r.groups.push_back(GroupRule{"A $dynamicRef resolves to the first $dynamicAnchor still in "
                                     "scope that is encountered when the schema is evaluated",
                                     true, "supported"});
        r.groups.push_back(
            GroupRule{"A $dynamicRef without anchor in fragment behaves identical to $ref", true,
                      "supported"});
        r.groups.push_back(
            GroupRule{"A $dynamicRef with intermediate scopes that don't include a matching "
                      "$dynamicAnchor does not affect dynamic scope resolution",
                      true, "supported"});
        r.groups.push_back(GroupRule{"An $anchor with the same name as a $dynamicAnchor is not "
                                     "used for dynamic scope resolution",
                                     true, "supported"});
        r.groups.push_back(GroupRule{"A $dynamicRef without a matching $dynamicAnchor in the same "
                                     "schema resource behaves like a normal $ref to $anchor",
                                     true, "supported"});
        r.groups.push_back(GroupRule{"A $dynamicRef with a non-matching $dynamicAnchor in the same "
                                     "schema resource behaves like a normal $ref to $anchor",
                                     true, "supported"});
        r.groups.push_back(
            GroupRule{"A $dynamicRef that initially resolves to a schema with a matching "
                      "$dynamicAnchor resolves to the first $dynamicAnchor in the dynamic scope",
                      true, "supported"});
        r.groups.push_back(
            GroupRule{"A $dynamicRef that initially resolves to a schema without a matching "
                      "$dynamicAnchor behaves like a normal $ref to $anchor",
                      true, "supported"});
        r.groups.push_back(
            GroupRule{"multiple dynamic paths to the $dynamicRef keyword", true, "supported"});
        r.groups.push_back(GroupRule{
            "after leaving a dynamic scope, it is not used by a $dynamicRef", true, "supported"});
        r.groups.push_back(GroupRule{"strict-tree schema, guards against misspelled properties",
                                     true, "supported"});
        r.groups.push_back(GroupRule{"tests for implementation dynamic anchor and reference link",
                                     true, "supported"});
        r.groups.push_back(GroupRule{
            "$ref and $dynamicAnchor are independent of order - $defs first", true, "supported"});
        r.groups.push_back(GroupRule{
            "$ref and $dynamicAnchor are independent of order - $ref first", true, "supported"});
        r.groups.push_back(
            GroupRule{"$ref to $dynamicRef finds detached $dynamicAnchor", true, "supported"});
        r.groups.push_back(GroupRule{"$dynamicRef points to a boolean schema", true, "supported"});
        r.groups.push_back(GroupRule{
            "$dynamicRef skips over intermediate resources - direct reference", true, "supported"});
        r.groups.push_back(
            GroupRule{"$dynamicRef avoids the root of each schema, but scopes are still registered",
                      true, "supported"});
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "enum.json";
        r.mode = RunWholeFile;
        r.reason = "supported documented-subset keywords";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "exclusiveMaximum.json";
        r.mode = RunWholeFile;
        r.reason = "supported documented-subset keywords";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "exclusiveMinimum.json";
        r.mode = RunWholeFile;
        r.reason = "supported documented-subset keywords";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "format.json";
        r.mode = RunSelectedGroups;
        r.reason = "";
        r.groups.push_back(GroupRule{"email format", true, "supported"});
        r.groups.push_back(GroupRule{"idn-email format", true, "supported"});
        r.groups.push_back(GroupRule{"regex format", true, "supported"});
        r.groups.push_back(
            GroupRule{"ipv4 format", true, "modern subset treats format as annotation-only"});
        r.groups.push_back(
            GroupRule{"ipv6 format", true, "modern subset treats format as annotation-only"});
        r.groups.push_back(GroupRule{"idn-hostname format", true, "supported"});
        r.groups.push_back(GroupRule{"hostname format", true, "supported"});
        r.groups.push_back(
            GroupRule{"date format", true, "modern subset treats format as annotation-only"});
        r.groups.push_back(
            GroupRule{"date-time format", true, "modern subset treats format as annotation-only"});
        r.groups.push_back(
            GroupRule{"time format", true, "modern subset treats format as annotation-only"});
        r.groups.push_back(GroupRule{"json-pointer format", true, "supported"});
        r.groups.push_back(GroupRule{"relative-json-pointer format", true, "supported"});
        r.groups.push_back(GroupRule{"iri format", true, "supported"});
        r.groups.push_back(GroupRule{"iri-reference format", true, "supported"});
        r.groups.push_back(GroupRule{"uri format", true, "supported"});
        r.groups.push_back(GroupRule{"uri-reference format", true, "supported"});
        r.groups.push_back(GroupRule{"uri-template format", true, "supported"});
        r.groups.push_back(
            GroupRule{"uuid format", true, "modern subset treats format as annotation-only"});
        r.groups.push_back(GroupRule{"duration format", true, "supported"});
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "if-then-else.json";
        r.mode = RunWholeFile;
        r.reason = "supported documented-subset keywords";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "infinite-loop-detection.json";
        r.mode = RunWholeFile;
        r.reason = "supported documented-subset keywords";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "items.json";
        r.mode = RunWholeFile;
        r.reason = "supported documented-subset keywords";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "maxContains.json";
        r.mode = RunWholeFile;
        r.reason = "supported documented-subset keywords";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "maxItems.json";
        r.mode = RunWholeFile;
        r.reason = "supported documented-subset keywords";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "maxLength.json";
        r.mode = RunWholeFile;
        r.reason = "supported documented-subset keywords";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "maxProperties.json";
        r.mode = RunWholeFile;
        r.reason = "supported documented-subset keywords";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "maximum.json";
        r.mode = RunWholeFile;
        r.reason = "supported documented-subset keywords";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "minContains.json";
        r.mode = RunWholeFile;
        r.reason = "supported documented-subset keywords";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "minItems.json";
        r.mode = RunWholeFile;
        r.reason = "supported documented-subset keywords";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "minLength.json";
        r.mode = RunWholeFile;
        r.reason = "supported documented-subset keywords";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "minProperties.json";
        r.mode = RunWholeFile;
        r.reason = "supported documented-subset keywords";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "minimum.json";
        r.mode = RunWholeFile;
        r.reason = "supported documented-subset keywords";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "multipleOf.json";
        r.mode = RunWholeFile;
        r.reason = "supported documented-subset keywords";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "not.json";
        r.mode = RunSelectedGroups;
        r.reason = "";
        r.groups.push_back(GroupRule{"not", true, "supported"});
        r.groups.push_back(GroupRule{"not multiple types", true, "supported"});
        r.groups.push_back(GroupRule{"not more complex schema", true, "supported"});
        r.groups.push_back(GroupRule{"forbidden property", true, "supported"});
        r.groups.push_back(GroupRule{"forbid everything with empty schema", true, "supported"});
        r.groups.push_back(
            GroupRule{"forbid everything with boolean schema true", true, "supported"});
        r.groups.push_back(
            GroupRule{"allow everything with boolean schema false", true, "supported"});
        r.groups.push_back(GroupRule{"double negation", true, "supported"});
        r.groups.push_back(
            GroupRule{"collect annotations inside a 'not', even if collection is disabled", true,
                      "supported internal annotation evaluation"});
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "oneOf.json";
        r.mode = RunWholeFile;
        r.reason = "supported documented-subset keywords";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "pattern.json";
        r.mode = RunSelectedGroups;
        r.reason = "";
        r.groups.push_back(GroupRule{"pattern validation", true, "supported"});
        r.groups.push_back(GroupRule{"pattern is not anchored", true, "supported"});
        r.groups.push_back(GroupRule{"pattern with Unicode property escape requires unicode mode",
                                     true, "SRELL provides Unicode ECMAScript property escapes"});
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "patternProperties.json";
        r.mode = RunSelectedGroups;
        r.reason = "";
        r.groups.push_back(GroupRule{"patternProperties validates properties matching a regex",
                                     true, "supported"});
        r.groups.push_back(
            GroupRule{"multiple simultaneous patternProperties are validated", true, "supported"});
        r.groups.push_back(GroupRule{"regexes are not anchored by default and are case sensitive",
                                     true, "supported"});
        r.groups.push_back(GroupRule{"patternProperties with boolean schemas", true, "supported"});
        r.groups.push_back(
            GroupRule{"patternProperties with null valued instance properties", true, "supported"});
        r.groups.push_back(GroupRule{"patternProperties with Unicode property escape", true,
                                     "SRELL provides Unicode ECMAScript property escapes"});
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "prefixItems.json";
        r.mode = RunWholeFile;
        r.reason = "supported documented-subset keywords";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "properties.json";
        r.mode = RunWholeFile;
        r.reason = "supported documented-subset keywords";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "propertyNames.json";
        r.mode = RunWholeFile;
        r.reason = "supported documented-subset keywords";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "ref.json";
        r.mode = RunSelectedGroups;
        r.reason = "";
        r.groups.push_back(GroupRule{"root pointer ref", true, "supported"});
        r.groups.push_back(GroupRule{"relative pointer ref to object", true, "supported"});
        r.groups.push_back(GroupRule{"relative pointer ref to array", true, "supported"});
        r.groups.push_back(GroupRule{"escaped pointer ref", true, "supported"});
        r.groups.push_back(GroupRule{"nested refs", true, "supported"});
        r.groups.push_back(GroupRule{"ref applies alongside sibling keywords", true,
                                     "supported modern subset semantics"});
        r.groups.push_back(GroupRule{
            "remote ref, containing refs itself", false,
            "requires the official 2020-12 meta-schema, which pjson intentionally does not claim"});
        r.groups.push_back(
            GroupRule{"property named $ref that is not a reference", true, "supported"});
        r.groups.push_back(
            GroupRule{"property named $ref, containing an actual $ref", true, "supported"});
        r.groups.push_back(GroupRule{"$ref to boolean schema true", true, "supported"});
        r.groups.push_back(GroupRule{"$ref to boolean schema false", true, "supported"});
        r.groups.push_back(
            GroupRule{"Recursive references between schemas", true, "supported explicit resolver"});
        r.groups.push_back(GroupRule{"refs with quote", true, "supported"});
        r.groups.push_back(
            GroupRule{"ref creates new scope when adjacent to keywords", true, "supported"});
        r.groups.push_back(GroupRule{
            "naive replacement of $ref with its destination is not correct", true, "supported"});
        r.groups.push_back(GroupRule{"refs with relative uris and defs", true, "supported"});
        r.groups.push_back(
            GroupRule{"relative refs with absolute uris and defs", true, "supported"});
        r.groups.push_back(
            GroupRule{"$id must be resolved against nearest parent, not just immediate parent",
                      true, "supported"});
        r.groups.push_back(GroupRule{"order of evaluation: $id and $ref", true, "supported"});
        r.groups.push_back(
            GroupRule{"order of evaluation: $id and $anchor and $ref", true, "supported"});
        r.groups.push_back(
            GroupRule{"order of evaluation: $id and $ref on nested schema", true, "supported"});
        r.groups.push_back(
            GroupRule{"simple URN base URI with $ref via the URN", true, "supported"});
        r.groups.push_back(GroupRule{"simple URN base URI with JSON pointer", true, "supported"});
        r.groups.push_back(GroupRule{"URN base URI with NSS", true, "supported"});
        r.groups.push_back(GroupRule{"URN base URI with r-component", true, "supported"});
        r.groups.push_back(GroupRule{"URN base URI with q-component", true, "supported"});
        r.groups.push_back(
            GroupRule{"URN base URI with URN and JSON pointer ref", true, "supported"});
        r.groups.push_back(GroupRule{"URN base URI with URN and anchor ref", true, "supported"});
        r.groups.push_back(GroupRule{"URN ref with nested pointer ref", true, "supported"});
        r.groups.push_back(GroupRule{"ref to if", true, "supported"});
        r.groups.push_back(GroupRule{"ref to then", true, "supported"});
        r.groups.push_back(GroupRule{"ref to else", true, "supported"});
        r.groups.push_back(GroupRule{"ref with absolute-path-reference", true, "supported"});
        r.groups.push_back(
            GroupRule{"$id with file URI still resolves pointers - *nix", true, "supported"});
        r.groups.push_back(
            GroupRule{"$id with file URI still resolves pointers - windows", true, "supported"});
        r.groups.push_back(GroupRule{"empty tokens in $ref json-pointer", true, "supported"});
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "refRemote.json";
        r.mode = RunWholeFile;
        r.reason = "requires remote schema resolution";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "required.json";
        r.mode = RunWholeFile;
        r.reason = "supported documented-subset keywords";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "type.json";
        r.mode = RunWholeFile;
        r.reason = "supported documented-subset keywords";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "unevaluatedItems.json";
        r.mode = RunWholeFile;
        r.reason = "supported unevaluated-item annotation propagation";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "unevaluatedProperties.json";
        r.mode = RunWholeFile;
        r.reason = "supported unevaluated-property annotation propagation";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "uniqueItems.json";
        r.mode = RunWholeFile;
        r.reason = "supported documented-subset keywords";
        rules.push_back(r);
        r = FileRule();
        r.relativePath = "vocabulary.json";
        r.mode = RunSelectedGroups;
        r.reason = "";
        r.groups.push_back(
            GroupRule{"schema that uses custom metaschema with with no validation vocabulary",
                      false, "requires $vocabulary negotiation and custom metaschema resolution"});
        r.groups.push_back(GroupRule{"ignore unrecognized optional vocabulary", true, "supported"});
        rules.push_back(r);

        // Optional suites remain explicit as well. Running subsets that exercise
        // already documented behavior prevents the small mandatory skip count
        // from being mistaken for a full conformance denominator.
        const auto addWhole = [&rules](const char* path, const char* reason) {
            FileRule rule;
            rule.relativePath = path;
            rule.mode = RunWholeFile;
            rule.reason = reason;
            rules.push_back(rule);
        };
        const auto addSkip = [&rules](const char* path, const char* reason) {
            FileRule rule;
            rule.relativePath = path;
            rule.mode = SkipWholeFile;
            rule.reason = reason;
            rules.push_back(rule);
        };
        addWhole("optional/anchor.json", "identifier isolation inside instance-valued keywords");
        addWhole("optional/dependencies-compatibility.json",
                 "supported legacy compatibility keyword");
        addWhole("optional/dynamicRef.json", "supported dynamic-scope behavior");
        addWhole("optional/float-overflow.json", "bounded binary64 arithmetic behavior");
        addWhole("optional/id.json", "identifier isolation inside instance-valued keywords");
        addWhole("optional/no-schema.json", "documented default dialect behavior");
        addWhole("optional/refOfUnknownKeyword.json",
                 "JSON Pointer references may target arbitrary schema-shaped locations");
        addWhole("optional/unknownKeyword.json",
                 "unknown-keyword contents are not traversed as schemas");

        addSkip("optional/bignum.json",
                "pjson intentionally rejects integers outside its signed/unsigned 64-bit model");
        addSkip("optional/cross-draft.json",
                "historic JSON Schema dialect interpretation is not implemented");
        addWhole("optional/ecmascript-regex.json",
                 "SRELL Unicode ECMAScript regular-expression implementation");
        addWhole("optional/non-bmp-regex.json",
                 "SRELL Unicode code-point regular-expression semantics");
        addSkip("optional/format-assertion.json",
                "custom meta-schema format-assertion vocabulary selection is not implemented");

        static const char* const kFormatSuites[] = {
            "optional/format/date-time.json",
            "optional/format/date.json",
            "optional/format/duration.json",
            "optional/format/email.json",
            "optional/format/hostname.json",
            "optional/format/idn-email.json",
            "optional/format/idn-hostname.json",
            "optional/format/ipv4.json",
            "optional/format/ipv6.json",
            "optional/format/iri-reference.json",
            "optional/format/iri.json",
            "optional/format/json-pointer.json",
            "optional/format/relative-json-pointer.json",
            "optional/format/time.json",
            "optional/format/unknown.json",
            "optional/format/uri-reference.json",
            "optional/format/uri-template.json",
            "optional/format/uri.json",
            "optional/format/uuid.json",
        };
        for (size_t i = 0; i < sizeof(kFormatSuites) / sizeof(kFormatSuites[0]); ++i)
            addSkip(kFormatSuites[i],
                    "Draft 2020-12 format assertions require vocabulary-controlled activation");
        addWhole("optional/format/regex.json", "supported asserted regex format");
        addWhole("optional/format/ecmascript-regex.json",
                 "SRELL parser with ECMA-262 extension restrictions");
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

    std::string firstErrorSummary(const std::vector<pjson_test::SchemaError>& errors) {
        if (errors.empty()) {
            return std::string("no schema errors reported");
        }

        std::ostringstream os;
        os << "first error";
        if (!errors[0].instanceLocation.empty()) {
            os << " at " << errors[0].instanceLocation;
        }
        if (!errors[0].message.empty()) {
            os << ": " << errors[0].message;
        }
        return os.str();
    }

    void recordFailure(const std::string& scope, const std::string& detail) {
        ::pjson_test::report_failure(__FILE__, __LINE__, scope.c_str(), detail);
    }

    void requireCompleteManifest(const std::string& suiteDir, const std::vector<FileRule>& rules) {
        std::vector<std::string> files;
        listJsonFiles(suiteDir, std::string(), files);
        std::sort(files.begin(), files.end());

        std::vector<std::string> declared;
        for (size_t i = 0; i < rules.size(); ++i)
            declared.push_back(rules[i].relativePath);
        std::sort(declared.begin(), declared.end());

        for (size_t i = 1; i < declared.size(); ++i) {
            if (declared[i] == declared[i - 1])
                recordFailure("official schema suite manifest duplicate", declared[i]);
        }
        for (size_t i = 0; i < files.size(); ++i) {
            if (!std::binary_search(declared.begin(), declared.end(), files[i]))
                recordFailure("official schema suite manifest gap",
                              files[i] + " has no explicit run/skip decision");
        }
        for (size_t i = 0; i < declared.size(); ++i) {
            if (!std::binary_search(files.begin(), files.end(), declared[i]))
                recordFailure("official schema suite manifest stale",
                              declared[i] + " is not present in the suite");
        }
    }

    // Runs one upstream case while preserving its file/group/case hierarchy in diagnostics.
    void runOneOfficialCase(const std::string& relativePath, const std::string& groupDesc,
                            const pJsonSchemaValidator& validator, const pjson& testCase,
                            RunSummary& summary) {
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

        std::vector<pjson_test::SchemaError> errors;
        const bool actual = validator.validate(*data, errors);
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
    void runWholeGroup(const std::string& relativePath, const pjson& group, RunSummary& summary,
                       const pJsonSchemaValidator::Options& options, bool adaptDialect) {
        const pjson* schema = group.find("schema");
        const pjson* tests = group.find("tests");
        const std::string groupDesc = groupDescription(group);

        if (schema == NULL || tests == NULL || !tests->isArray()) {
            recordFailure("official schema suite group shape", relativePath + " :: " + groupDesc);
            return;
        }

        // The upstream files declare their official draft URI. pjson does not
        // claim those complete dialects: this manifest intentionally exercises
        // selected cases under pjson's named documented-subset dialect instead.
        // Removing only the root declaration is the explicit adaptation; every
        // validation/applicator keyword and instance remains unchanged. Compile
        // once per upstream group, matching the public validator lifecycle.
        pjson subsetSchema(*schema);
        if (adaptDialect)
            subsetSchema.erase("$schema");
        pJsonSchemaValidator validator(subsetSchema, options);

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
            runOneOfficialCase(relativePath, groupDesc, validator, *testCase, summary);
        }
    }

    // Enforces a bidirectional manifest invariant: every upstream group has a rule and every rule
    // still names an upstream group. This makes suite upgrades fail visibly instead of shrinking
    // coverage silently.
    void runSelectedGroups(const FileRule& fileRule, const pjson& suiteFile, RunSummary& summary,
                           const pJsonSchemaValidator::Options& options, bool adaptDialect) {
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

            runWholeGroup(fileRule.relativePath, group, summary, options, adaptDialect);
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
    void runWholeFile(const FileRule& fileRule, const pjson& suiteFile, RunSummary& summary,
                      const pJsonSchemaValidator::Options& options, bool adaptDialect) {
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
            runWholeGroup(fileRule.relativePath, *group, summary, options, adaptDialect);
        }
    }

} // namespace

// Shared manifest-driven runner used by both the draft7 and draft2020-12 gates.
static void runOfficialSuite(const std::string& suiteDir, const std::vector<FileRule>& rules,
                             const char* dialectLabel, const pJsonSchemaValidator::Options& options,
                             bool adaptDialect) {
    RunSummary summary;
    for (size_t i = 0; i < rules.size(); ++i) {
        const std::string path = joinPath(suiteDir, rules[i].relativePath);
        summary.filesVisited += 1;
        pJsonSchemaValidator::Options fileOptions = options;
        if (std::string(rules[i].relativePath).compare(0, 16, "optional/format/") == 0)
            fileOptions.validateFormats = true;

        if (!isRegularFile(path)) {
            recordFailure("official schema suite file missing",
                          std::string(rules[i].relativePath) + " under " + suiteDir);
            continue;
        }

        if (rules[i].mode == SkipWholeFile) {
            summary.filesSkipped += 1;
            std::printf("    INFO skip %s [%s]\n", rules[i].relativePath, rules[i].reason);
            continue;
        }

        pjson::ParseError parseError;
        pjson_test::Parsed suite = parseJson(readFile(path), &parseError);
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
            runWholeFile(rules[i], *suite, summary, fileOptions, adaptDialect);
        } else {
            runSelectedGroups(rules[i], *suite, summary, fileOptions, adaptDialect);
        }
    }

    std::printf("    INFO official %s suite visited %llu files (%llu whole-file skips), "
                "ran %llu groups / %llu cases, skipped %llu groups / %llu cases\n",
                dialectLabel, static_cast<unsigned long long>(summary.filesVisited),
                static_cast<unsigned long long>(summary.filesSkipped),
                static_cast<unsigned long long>(summary.groupsRun),
                static_cast<unsigned long long>(summary.casesRun),
                static_cast<unsigned long long>(summary.groupsSkipped),
                static_cast<unsigned long long>(summary.casesSkipped));
}

TEST(schema_official_draft7_optional) {
    const std::string draft7Dir = resolveDraft7Dir();
    if (draft7Dir.empty()) {
        std::printf("    INFO JSON-Schema-Test-Suite skipped; set "
                    "PJSON_JSON_SCHEMA_TEST_SUITE_DIR or run "
                    "scripts/fetch-json-schema-test-suite.sh\n");
        CHECK(true);
        return;
    }
    runOfficialSuite(draft7Dir, manifest(), "draft7", pJsonSchemaValidator::Options(), true);
}

TEST(schema_official_draft2020_optional) {
    const std::string dir = resolveDraft2020Dir();
    if (dir.empty()) {
        std::printf("    INFO draft2020-12 JSON-Schema-Test-Suite skipped; set "
                    "PJSON_JSON_SCHEMA_TEST_SUITE_DIR or run "
                    "scripts/fetch-json-schema-test-suite.sh\n");
        CHECK(true);
        return;
    }
    OfficialResolverContext resolverContext;
    resolverContext.remoteRoot = joinPath(configuredSchemaSuiteDir(), "remotes");
    pJsonSchemaValidator::Options options = pJsonSchemaValidator::Options::modernSubset();
    options.resolver = resolveOfficialSchema;
    options.resolverContext = &resolverContext;
    const std::vector<FileRule> rules = manifest2020();
    requireCompleteManifest(dir, rules);
    runOfficialSuite(dir, rules, "draft2020-12", options, true);
}
