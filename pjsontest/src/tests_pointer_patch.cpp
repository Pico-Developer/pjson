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
// JSON Pointer (RFC 6901), JSON Patch (RFC 6902), and JSON Merge Patch
// (RFC 7396) behavior and error-handling tests covering:
//
//   - pjson::PointerError / pjson::PatchError
//   - findPointer()
//   - escapePointerToken()
//   - applyPatch()
//   - applyMergePatch()
//
#include "pjson.h"
#include "test_harness.h"
#include "test_util.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace ByteDance;
using pjson_test::parse;

namespace {
    int64_t mustGetInt(const pjson& value) {
        int64_t out = 0;
        CHECK(value.tryGet(out));
        return out;
    }

    std::string mustGetString(const pjson& value) {
        std::string out;
        CHECK(value.tryGet(out));
        return out;
    }

    // Parses fixed test fixtures while still recording a normal harness failure on bad setup.
    pjson::unique_ptr parseChecked(const char* text) {
        pjson::unique_ptr doc = parse(text);
        CHECK(doc != nullptr);
        return doc;
    }

    // Returns an explicitly typed empty patch document for programmatic operation assembly.
    pjson makePatchArray() {
        pjson patch;
        patch.resetTo(pjson::jsonArray);
        return patch;
    }

    // Canonical RFC 6901 object containing every token-escaping example.
    pjson::unique_ptr makeRfc6901ExampleDoc() {
        return parseChecked(
            R"({"foo":["bar","baz"],"":0,"a/b":1,"c%d":2,"e^f":3,"g|h":4,"i\\j":5,"k\"l":6," ":7,"m~n":8})");
    }

    // Builds /token/token/... pointers for iterative-depth and budget tests.
    std::string repeatedObjectPointer(const std::string& token, int depth) {
        std::string out;
        for (int i = 0; i < depth; ++i) {
            out += "/";
            out += token;
        }
        return out;
    }

    // Builds the document matching repeatedObjectPointer without recursive test setup.
    pjson makeDeepObjectChain(int depth, int64_t leafValue, const std::string& key = "x") {
        pjson root;
        pjson* cur = &root;
        for (int i = 0; i < depth; ++i) {
            cur = &((*cur)[key]);
        }
        *cur = leafValue;
        return root;
    }

} // namespace

//===----------------------------------------------------------------------===//
// PointerError / PatchError default state
//===----------------------------------------------------------------------===//
TEST(pointer_error_defaults_ok) {
    pjson::PointerError err;
    CHECK(err.ok);
    CHECK(err.code == pjson::PointerError::Ok);
    CHECK_EQ(err.pointer, std::string(""));
    CHECK_EQ(err.tokenIndex, size_t(0));
    CHECK_EQ(err.token, std::string(""));
    CHECK_EQ(err.message, std::string(""));
}

TEST(patch_error_defaults_ok) {
    pjson::PatchError err;
    CHECK(err.ok);
    CHECK(err.code == pjson::PatchError::Ok);
    CHECK_EQ(err.opIndex, size_t(0));
    CHECK_EQ(err.op, std::string(""));
    CHECK_EQ(err.path, std::string(""));
    CHECK_EQ(err.from, std::string(""));
    CHECK_EQ(err.tokenIndex, size_t(0));
    CHECK_EQ(err.token, std::string(""));
    CHECK_EQ(err.message, std::string(""));
}

TEST(patch_options_defaults_are_finite) {
    const pjson::PatchOptions options;
    CHECK_EQ(options.maxOperations, size_t(10000));
    CHECK_EQ(options.maxClonedNodes, size_t(1000000));
    CHECK_EQ(options.maxClonedBytes, size_t(64) * 1024U * 1024U);
    CHECK_EQ(options.maxWork, size_t(1000000));
}

//===----------------------------------------------------------------------===//
// RFC 6901 pointer examples and escaping
//===----------------------------------------------------------------------===//
TEST(pointer_rfc6901_examples) {
    pjson::unique_ptr doc = makeRfc6901ExampleDoc();

    CHECK(doc->findPointer("") == doc.get());
    CHECK_EQ(doc->findPointer("/foo")->size(), size_t(2));
    CHECK_EQ(mustGetString(*doc->findPointer("/foo/0")), std::string("bar"));
    CHECK_EQ(mustGetInt(*doc->findPointer("/")), int64_t(0));
    CHECK_EQ(mustGetInt(*doc->findPointer("/a~1b")), int64_t(1));
    CHECK_EQ(mustGetInt(*doc->findPointer("/c%d")), int64_t(2));
    CHECK_EQ(mustGetInt(*doc->findPointer("/e^f")), int64_t(3));
    CHECK_EQ(mustGetInt(*doc->findPointer("/g|h")), int64_t(4));
    CHECK_EQ(mustGetInt(*doc->findPointer("/i\\j")), int64_t(5));
    CHECK_EQ(mustGetInt(*doc->findPointer("/k\"l")), int64_t(6));
    CHECK_EQ(mustGetInt(*doc->findPointer("/ ")), int64_t(7));
    CHECK_EQ(mustGetInt(*doc->findPointer("/m~0n")), int64_t(8));
}

TEST(pointer_char_ptr_and_const_overloads_work) {
    pjson::unique_ptr doc = makeRfc6901ExampleDoc();
    const pjson& cdoc = *doc;

    const pjson* cnode = cdoc.findPointer("/foo/1");
    CHECK(cnode != nullptr);
    CHECK_EQ(mustGetString(*cnode), std::string("baz"));

    pjson* mnode = doc->findPointer("/foo/1");
    CHECK(mnode != nullptr);
    CHECK_EQ(mustGetString(*mnode), std::string("baz"));
}

TEST(pointer_escape_token_round_trips) {
    const std::string token = "a/b~c";
    CHECK_EQ(pjson::escapePointerToken(token), std::string("a~1b~0c"));

    pjson doc;
    doc[token] = int64_t(42);
    const std::string ptr = "/" + pjson::escapePointerToken(token);
    const pjson* node = doc.findPointer(ptr);
    CHECK(node != nullptr);
    CHECK_EQ(mustGetInt(*node), int64_t(42));
}

TEST(pointer_empty_token_after_slash_is_empty_key) {
    pjson doc;
    doc[""] = "empty";
    const pjson* node = doc.findPointer("/");
    CHECK(node != nullptr);
    CHECK_EQ(mustGetString(*node), std::string("empty"));
}

//===----------------------------------------------------------------------===//
// Pointer errors and non-vivifying behavior
//===----------------------------------------------------------------------===//
TEST(pointer_invalid_syntax_requires_leading_slash_or_empty) {
    pjson::unique_ptr doc = parseChecked(R"({"foo":1})");
    pjson::PointerError err;
    CHECK(doc->findPointer("foo", err) == nullptr);
    CHECK(!err.ok);
    CHECK(err.code == pjson::PointerError::InvalidSyntax);
    CHECK_EQ(err.pointer, std::string("foo"));
}

TEST(pointer_invalid_escape_sequences_fail) {
    pjson::unique_ptr doc = parseChecked(R"({"foo":1})");

    pjson::PointerError badDigit;
    CHECK(doc->findPointer("/~2", badDigit) == nullptr);
    CHECK(!badDigit.ok);
    CHECK(badDigit.code == pjson::PointerError::InvalidEscape);

    pjson::PointerError trailingTilde;
    CHECK(doc->findPointer("/abc~", trailingTilde) == nullptr);
    CHECK(!trailingTilde.ok);
    CHECK(trailingTilde.code == pjson::PointerError::InvalidEscape);
}

TEST(pointer_missing_target_reports_error) {
    pjson::unique_ptr doc = parseChecked(R"({"foo":1})");
    pjson::PointerError err;
    CHECK(doc->findPointer("/bar", err) == nullptr);
    CHECK(!err.ok);
    CHECK(err.code == pjson::PointerError::MissingTarget);
    CHECK_EQ(err.pointer, std::string("/bar"));
    CHECK_EQ(err.tokenIndex, size_t(0));
    CHECK_EQ(err.token, std::string("bar"));
}

TEST(pointer_expected_container_reports_error) {
    pjson::unique_ptr doc = parseChecked(R"({"foo":1})");
    pjson::PointerError err;
    CHECK(doc->findPointer("/foo/bar", err) == nullptr);
    CHECK(!err.ok);
    CHECK(err.code == pjson::PointerError::ExpectedContainer);
    CHECK_EQ(err.tokenIndex, size_t(1));
    CHECK_EQ(err.token, std::string("bar"));
}

TEST(pointer_invalid_array_index_reports_error) {
    pjson::unique_ptr doc = parseChecked(R"(["x","y"])");

    pjson::PointerError leadingZero;
    CHECK(doc->findPointer("/01", leadingZero) == nullptr);
    CHECK(!leadingZero.ok);
    CHECK(leadingZero.code == pjson::PointerError::InvalidArrayIndex);
    CHECK_EQ(leadingZero.token, std::string("01"));

    pjson::PointerError negative;
    CHECK(doc->findPointer("/-1", negative) == nullptr);
    CHECK(!negative.ok);
    CHECK(negative.code == pjson::PointerError::InvalidArrayIndex);
    CHECK_EQ(negative.token, std::string("-1"));
}

TEST(pointer_array_index_out_of_range_reports_error) {
    pjson::unique_ptr doc = parseChecked(R"(["x","y"])");
    pjson::PointerError err;
    CHECK(doc->findPointer("/2", err) == nullptr);
    CHECK(!err.ok);
    CHECK(err.code == pjson::PointerError::ArrayIndexOutOfRange);
    CHECK_EQ(err.token, std::string("2"));
}

TEST(pointer_append_token_is_not_lookup) {
    pjson::unique_ptr doc = parseChecked(R"(["x","y"])");
    pjson::PointerError err;
    CHECK(doc->findPointer("/-", err) == nullptr);
    CHECK(!err.ok);
    CHECK(err.code == pjson::PointerError::AppendTokenNotAllowed);
    CHECK_EQ(err.token, std::string("-"));
}

TEST(pointer_object_numeric_key_is_not_array_index) {
    pjson doc;
    doc["0"] = "zero";
    const pjson* node = doc.findPointer("/0");
    CHECK(node != nullptr);
    CHECK_EQ(mustGetString(*node), std::string("zero"));
}

TEST(pointer_find_is_non_vivifying_on_missing_path) {
    pjson doc;
    CHECK(doc.isNull());

    pjson::PointerError err;
    CHECK(doc.findPointer("/new/key", err) == nullptr);
    CHECK(!err.ok);
    CHECK(doc.isNull());
    CHECK_EQ(doc.size(), size_t(0));
}

TEST(pointer_mutable_find_can_edit_existing_node_without_creating_new_ones) {
    pjson doc;
    doc["obj"]["keep"] = static_cast<int64_t>(1);

    pjson* node = doc.findPointer("/obj/keep");
    CHECK(node != nullptr);
    *node = static_cast<int64_t>(99);
    CHECK_EQ(mustGetInt(doc["obj"]["keep"]), int64_t(99));

    pjson::PointerError err;
    CHECK(doc.findPointer("/obj/missing", err) == nullptr);
    CHECK(!err.ok);
    CHECK_EQ(doc["obj"].size(), size_t(1));
    CHECK(!doc["obj"].hasKey("missing"));
}

TEST(pointer_error_object_is_reused_across_failure_and_success) {
    pjson::unique_ptr doc = parseChecked(R"({"foo":1})");
    pjson::PointerError err;

    CHECK(doc->findPointer("/missing", err) == nullptr);
    CHECK(!err.ok);
    CHECK_EQ(err.code, pjson::PointerError::MissingTarget);
    CHECK(!err.message.empty());

    const pjson* node = doc->findPointer("/foo", err);
    CHECK(node != nullptr);
    CHECK(err.ok);
    CHECK_EQ(err.code, pjson::PointerError::Ok);
    CHECK_EQ(err.pointer, std::string(""));
    CHECK_EQ(err.tokenIndex, size_t(0));
    CHECK_EQ(err.token, std::string(""));
    CHECK_EQ(err.message, std::string(""));
}

//===----------------------------------------------------------------------===//
// JSON Patch: document-level validation failures
//===----------------------------------------------------------------------===//
TEST(patch_document_must_be_array) {
    pjson doc;
    doc["a"] = int64_t(1);
    pjson patch;
    patch["op"] = "add";

    pjson before(doc);
    pjson::PatchError err;
    CHECK(!doc.applyPatch(patch, err));
    CHECK(!err.ok);
    CHECK(err.code == pjson::PatchError::InvalidPatchDocument);
    CHECK(doc == before);
}

TEST(patch_operation_must_be_object) {
    pjson doc;
    doc["a"] = int64_t(1);
    pjson patch = makePatchArray();
    patch[0] = int64_t(5);

    pjson before(doc);
    pjson::PatchError err;
    CHECK(!doc.applyPatch(patch, err));
    CHECK(!err.ok);
    CHECK(err.code == pjson::PatchError::OperationNotObject);
    CHECK_EQ(err.opIndex, size_t(0));
    CHECK(doc == before);
}

TEST(patch_missing_required_members_report_precise_error) {
    pjson doc;
    doc["a"] = int64_t(1);

    pjson missingOp = makePatchArray();
    missingOp[0]["path"] = "/a";
    missingOp[0]["value"] = int64_t(2);
    pjson::PatchError errOp;
    CHECK(!doc.applyPatch(missingOp, errOp));
    CHECK(errOp.code == pjson::PatchError::MissingOp);
    CHECK_EQ(errOp.opIndex, size_t(0));

    pjson missingPath = makePatchArray();
    missingPath[0]["op"] = "remove";
    pjson::PatchError errPath;
    CHECK(!doc.applyPatch(missingPath, errPath));
    CHECK(errPath.code == pjson::PatchError::MissingPath);

    pjson missingFrom = makePatchArray();
    missingFrom[0]["op"] = "move";
    missingFrom[0]["path"] = "/b";
    pjson::PatchError errFrom;
    CHECK(!doc.applyPatch(missingFrom, errFrom));
    CHECK(errFrom.code == pjson::PatchError::MissingFrom);

    pjson missingValue = makePatchArray();
    missingValue[0]["op"] = "add";
    missingValue[0]["path"] = "/b";
    pjson::PatchError errValue;
    CHECK(!doc.applyPatch(missingValue, errValue));
    CHECK(errValue.code == pjson::PatchError::MissingValue);
}

TEST(patch_invalid_op_is_rejected) {
    pjson doc;
    doc["a"] = int64_t(1);
    pjson patch = makePatchArray();
    patch[0]["op"] = "explode";
    patch[0]["path"] = "/a";

    pjson::PatchError err;
    CHECK(!doc.applyPatch(patch, err));
    CHECK(!err.ok);
    CHECK(err.code == pjson::PatchError::InvalidOp);
    CHECK_EQ(err.op, std::string("explode"));
}

//===----------------------------------------------------------------------===//
// JSON Patch: add
//===----------------------------------------------------------------------===//
TEST(patch_add_object_member_and_replace_existing_member) {
    pjson::unique_ptr doc = parseChecked(R"({"a":1})");
    pjson patch = makePatchArray();
    patch[0]["op"] = "add";
    patch[0]["path"] = "/b";
    patch[0]["value"] = static_cast<int64_t>(2);
    patch[1]["op"] = "add";
    patch[1]["path"] = "/a";
    patch[1]["value"] = static_cast<int64_t>(9);

    pjson::PatchError err;
    CHECK(doc->applyPatch(patch, err));
    CHECK(err.ok);
    CHECK_EQ(doc->toString(), std::string("{\"a\":9,\"b\":2}"));
}

TEST(patch_add_root_replaces_whole_document) {
    pjson::unique_ptr doc = parseChecked(R"({"a":1})");
    pjson value;
    value["replaced"] = true;
    value["n"] = static_cast<int64_t>(7);

    pjson patch = makePatchArray();
    patch[0]["op"] = "add";
    patch[0]["path"] = "";
    patch[0]["value"] = value;

    CHECK(doc->applyPatch(patch));
    CHECK_EQ(doc->toString(), std::string("{\"n\":7,\"replaced\":true}"));
}

TEST(patch_add_array_inserts_and_appends) {
    pjson::unique_ptr doc = parseChecked(R"(["a","c"])");
    pjson patch = makePatchArray();
    patch[0]["op"] = "add";
    patch[0]["path"] = "/1";
    patch[0]["value"] = "b";
    patch[1]["op"] = "add";
    patch[1]["path"] = "/-";
    patch[1]["value"] = "d";

    CHECK(doc->applyPatch(patch));
    CHECK_EQ(doc->toString(), std::string("[\"a\",\"b\",\"c\",\"d\"]"));
}

TEST(patch_add_requires_existing_parent_and_valid_array_index) {
    pjson::unique_ptr doc = parseChecked(R"({"a":[1,2]})");
    const pjson before(*doc);

    pjson missingParent = makePatchArray();
    missingParent[0]["op"] = "add";
    missingParent[0]["path"] = "/missing/0";
    missingParent[0]["value"] = static_cast<int64_t>(7);
    pjson::PatchError errParent;
    CHECK(!doc->applyPatch(missingParent, errParent));
    CHECK(errParent.code == pjson::PatchError::TargetMissing);
    CHECK(*doc == before);

    pjson badIndex = makePatchArray();
    badIndex[0]["op"] = "add";
    badIndex[0]["path"] = "/a/01";
    badIndex[0]["value"] = static_cast<int64_t>(7);
    pjson::PatchError errIndex;
    CHECK(!doc->applyPatch(badIndex, errIndex));
    CHECK(errIndex.code == pjson::PatchError::InvalidArrayIndex);
    CHECK(*doc == before);

    pjson outOfRange = makePatchArray();
    outOfRange[0]["op"] = "add";
    outOfRange[0]["path"] = "/a/3";
    outOfRange[0]["value"] = static_cast<int64_t>(7);
    pjson::PatchError errRange;
    CHECK(!doc->applyPatch(outOfRange, errRange));
    CHECK(errRange.code == pjson::PatchError::ArrayIndexOutOfRange);
    CHECK(*doc == before);
}

//===----------------------------------------------------------------------===//
// JSON Patch: remove / replace
//===----------------------------------------------------------------------===//
TEST(patch_remove_object_member_and_array_element) {
    pjson::unique_ptr doc = parseChecked(R"({"a":1,"arr":["x","y","z"]})");
    pjson patch = makePatchArray();
    patch[0]["op"] = "remove";
    patch[0]["path"] = "/a";
    patch[1]["op"] = "remove";
    patch[1]["path"] = "/arr/1";

    CHECK(doc->applyPatch(patch));
    CHECK_EQ(doc->toString(), std::string("{\"arr\":[\"x\",\"z\"]}"));
}

TEST(patch_remove_root_succeeds_and_leaves_null) {
    pjson::unique_ptr doc = parseChecked(R"({"a":1})");
    pjson patch = makePatchArray();
    patch[0]["op"] = "remove";
    patch[0]["path"] = "";

    pjson::PatchError err;
    CHECK(doc->applyPatch(patch, err));
    CHECK(err.ok);
    CHECK(doc->isNull());
}

TEST(patch_remove_and_replace_require_existing_target) {
    pjson::unique_ptr doc = parseChecked(R"({"a":[1,2],"b":1})");
    const pjson before(*doc);

    pjson removeMissing = makePatchArray();
    removeMissing[0]["op"] = "remove";
    removeMissing[0]["path"] = "/missing";
    pjson::PatchError errRemove;
    CHECK(!doc->applyPatch(removeMissing, errRemove));
    CHECK(errRemove.code == pjson::PatchError::TargetMissing);
    CHECK(*doc == before);

    pjson replaceMissing = makePatchArray();
    replaceMissing[0]["op"] = "replace";
    replaceMissing[0]["path"] = "/a/3";
    replaceMissing[0]["value"] = static_cast<int64_t>(7);
    pjson::PatchError errReplace;
    CHECK(!doc->applyPatch(replaceMissing, errReplace));
    CHECK(errReplace.code == pjson::PatchError::ArrayIndexOutOfRange);
    CHECK(*doc == before);
}

TEST(patch_replace_root_and_existing_member) {
    pjson::unique_ptr doc = parseChecked(R"({"a":1,"b":2})");
    pjson replaceWhole;
    replaceWhole["done"] = true;

    pjson patch = makePatchArray();
    patch[0]["op"] = "replace";
    patch[0]["path"] = "/a";
    patch[0]["value"] = static_cast<int64_t>(9);
    patch[1]["op"] = "replace";
    patch[1]["path"] = "";
    patch[1]["value"] = replaceWhole;

    CHECK(doc->applyPatch(patch));
    CHECK_EQ(doc->toString(), std::string("{\"done\":true}"));
}

//===----------------------------------------------------------------------===//
// JSON Patch: move / copy / test
//===----------------------------------------------------------------------===//
TEST(patch_move_object_member_and_same_array_reorder) {
    pjson::unique_ptr doc = parseChecked(R"({"obj":{"a":1},"arr":["a","b","c"]})");
    pjson patch = makePatchArray();
    patch[0]["op"] = "move";
    patch[0]["from"] = "/obj/a";
    patch[0]["path"] = "/obj/b";
    patch[1]["op"] = "move";
    patch[1]["from"] = "/arr/0";
    patch[1]["path"] = "/arr/2";

    CHECK(doc->applyPatch(patch));
    CHECK_EQ(doc->toString(), std::string("{\"arr\":[\"b\",\"c\",\"a\"],\"obj\":{\"b\":1}}"));
}

TEST(patch_move_from_must_exist_and_cannot_move_into_descendant) {
    pjson::unique_ptr doc = parseChecked(R"({"a":{"b":1},"x":0})");
    const pjson before(*doc);

    pjson missingFrom = makePatchArray();
    missingFrom[0]["op"] = "move";
    missingFrom[0]["from"] = "/missing";
    missingFrom[0]["path"] = "/x";
    pjson::PatchError errMissing;
    CHECK(!doc->applyPatch(missingFrom, errMissing));
    CHECK(errMissing.code == pjson::PatchError::TargetMissing);
    CHECK(*doc == before);

    pjson intoDescendant = makePatchArray();
    intoDescendant[0]["op"] = "move";
    intoDescendant[0]["from"] = "/a";
    intoDescendant[0]["path"] = "/a/b/c";
    pjson::PatchError errDescendant;
    CHECK(!doc->applyPatch(intoDescendant, errDescendant));
    CHECK(errDescendant.code == pjson::PatchError::MoveIntoDescendant);
    CHECK(*doc == before);

    pjson rootSource = makePatchArray();
    rootSource[0]["op"] = "move";
    rootSource[0]["from"] = "";
    rootSource[0]["path"] = "/x";
    pjson::PatchError errRootSource;
    CHECK(!doc->applyPatch(rootSource, errRootSource));
    CHECK(errRootSource.code == pjson::PatchError::MoveRootNotAllowed);
    CHECK(*doc == before);
}

TEST(patch_copy_duplicates_value_without_mutating_source) {
    pjson::unique_ptr doc = parseChecked(R"({"src":{"nested":[1,2]},"dst":0})");
    pjson patch = makePatchArray();
    patch[0]["op"] = "copy";
    patch[0]["from"] = "/src";
    patch[0]["path"] = "/dst";

    CHECK(doc->applyPatch(patch));
    CHECK_EQ(doc->toString(),
             std::string("{\"dst\":{\"nested\":[1,2]},\"src\":{\"nested\":[1,2]}}"));

    pjson* copiedArray = doc->findPointer("/dst/nested");
    CHECK(copiedArray != nullptr);
    (*copiedArray)[0] = static_cast<int64_t>(99);
    CHECK_EQ(mustGetInt(*doc->findPointer("/src/nested/0")), int64_t(1));
}

TEST(patch_test_uses_rfc_numeric_equality_and_fails_atomically) {
    pjson::unique_ptr doc = parseChecked(R"({"n":1,"arr":[{"x":1.0}]})");
    const pjson before(*doc);

    pjson pass = makePatchArray();
    pass[0]["op"] = "test";
    pass[0]["path"] = "/n";
    pass[0]["value"] = double(1.0);
    pass[1]["op"] = "test";
    pass[1]["path"] = "/arr/0/x";
    pass[1]["value"] = int64_t(1);
    CHECK(doc->applyPatch(pass));

    pjson fail = makePatchArray();
    fail[0]["op"] = "replace";
    fail[0]["path"] = "/n";
    fail[0]["value"] = static_cast<int64_t>(2);
    fail[1]["op"] = "test";
    fail[1]["path"] = "/arr/0/x";
    fail[1]["value"] = static_cast<int64_t>(2);

    pjson::PatchError err;
    CHECK(!doc->applyPatch(fail, err));
    CHECK(!err.ok);
    CHECK(err.code == pjson::PatchError::TestFailed);
    CHECK_EQ(err.opIndex, size_t(1));
    CHECK(*doc == before);
}

TEST(patch_test_numeric_equality_above_2pow53_and_rounded_inequality) {
    pjson::unique_ptr exact = parseChecked(R"({"n":9007199254740994})");
    pjson patch = makePatchArray();
    patch[0]["op"] = "test";
    patch[0]["path"] = "/n";
    patch[0]["value"] = double(9007199254740994.0);
    CHECK(exact->applyPatch(patch));

    pjson::unique_ptr rounded = parseChecked(R"({"n":9007199254740993})");
    pjson bad = makePatchArray();
    bad[0]["op"] = "test";
    bad[0]["path"] = "/n";
    bad[0]["value"] = double(9007199254740992.0);
    pjson::PatchError err;
    CHECK(!rounded->applyPatch(bad, err));
    CHECK(!err.ok);
    CHECK(err.code == pjson::PatchError::TestFailed);
}

TEST(patch_copy_and_move_can_replace_root) {
    pjson::unique_ptr copied = parseChecked(R"({"a":{"b":1},"x":2})");
    pjson copyPatch = makePatchArray();
    copyPatch[0]["op"] = "copy";
    copyPatch[0]["from"] = "/a";
    copyPatch[0]["path"] = "";
    CHECK(copied->applyPatch(copyPatch));
    CHECK_EQ(copied->toString(), std::string("{\"b\":1}"));

    pjson::unique_ptr moved = parseChecked(R"({"a":{"b":1},"x":2})");
    pjson movePatch = makePatchArray();
    movePatch[0]["op"] = "move";
    movePatch[0]["from"] = "/a";
    movePatch[0]["path"] = "";
    CHECK(moved->applyPatch(movePatch));
    CHECK_EQ(moved->toString(), std::string("{\"b\":1}"));
}

//===----------------------------------------------------------------------===//
// JSON Patch: invalid path / from syntax and full rollback
//===----------------------------------------------------------------------===//
TEST(patch_invalid_path_and_from_bubble_structured_errors) {
    pjson::unique_ptr doc = parseChecked(R"({"a":1,"b":2})");
    const pjson before(*doc);

    pjson badPath = makePatchArray();
    badPath[0]["op"] = "add";
    badPath[0]["path"] = "a";
    badPath[0]["value"] = static_cast<int64_t>(5);
    pjson::PatchError errPath;
    CHECK(!doc->applyPatch(badPath, errPath));
    CHECK(errPath.code == pjson::PatchError::InvalidPath);
    CHECK_EQ(errPath.path, std::string("a"));
    CHECK(*doc == before);

    pjson badFrom = makePatchArray();
    badFrom[0]["op"] = "copy";
    badFrom[0]["from"] = "/~2";
    badFrom[0]["path"] = "/c";
    pjson::PatchError errFrom;
    CHECK(!doc->applyPatch(badFrom, errFrom));
    CHECK(errFrom.code == pjson::PatchError::InvalidFrom);
    CHECK_EQ(errFrom.from, std::string("/~2"));
    CHECK(*doc == before);
}

TEST(patch_error_object_is_reused_across_failure_and_success) {
    pjson::unique_ptr doc = parseChecked(R"({"a":1})");
    pjson::PatchError err;

    pjson failing = makePatchArray();
    failing[0]["op"] = "remove";
    failing[0]["path"] = "/missing";
    CHECK(!doc->applyPatch(failing, err));
    CHECK(!err.ok);
    CHECK_EQ(err.code, pjson::PatchError::TargetMissing);
    CHECK(!err.message.empty());

    pjson succeeding = makePatchArray();
    succeeding[0]["op"] = "replace";
    succeeding[0]["path"] = "/a";
    succeeding[0]["value"] = static_cast<int64_t>(2);
    CHECK(doc->applyPatch(succeeding, err));
    CHECK(err.ok);
    CHECK_EQ(err.code, pjson::PatchError::Ok);
    CHECK_EQ(err.opIndex, size_t(0));
    CHECK_EQ(err.op, std::string(""));
    CHECK_EQ(err.path, std::string(""));
    CHECK_EQ(err.from, std::string(""));
    CHECK_EQ(err.tokenIndex, size_t(0));
    CHECK_EQ(err.token, std::string(""));
    CHECK_EQ(err.message, std::string(""));
}

TEST(patch_atomic_rollback_on_late_failure) {
    pjson::unique_ptr doc = parseChecked(R"({"a":1,"arr":[10,20]})");
    const pjson before(*doc);
    pjson patch = makePatchArray();
    patch[0]["op"] = "replace";
    patch[0]["path"] = "/a";
    patch[0]["value"] = static_cast<int64_t>(9);
    patch[1]["op"] = "add";
    patch[1]["path"] = "/arr/-";
    patch[1]["value"] = static_cast<int64_t>(30);
    patch[2]["op"] = "remove";
    patch[2]["path"] = "/missing";

    pjson::PatchError err;
    CHECK(!doc->applyPatch(patch, err));
    CHECK(!err.ok);
    CHECK_EQ(err.opIndex, size_t(2));
    CHECK(err.code == pjson::PatchError::TargetMissing);
    CHECK(*doc == before);
}

TEST(patch_resource_limits_are_atomic_and_error_is_reusable) {
    pjson::unique_ptr doc = parseChecked(R"({"a":1,"nested":{"x":2}})");
    const pjson before(*doc);
    pjson patch = makePatchArray();
    patch[0]["op"] = "replace";
    patch[0]["path"] = "/a";
    patch[0]["value"] = int64_t(9);
    patch[1]["op"] = "add";
    patch[1]["path"] = "/b";
    patch[1]["value"] = int64_t(3);

    pjson::PatchOptions operations;
    operations.maxOperations = 1;
    pjson::PatchError error;
    CHECK(!doc->applyPatch(patch, error, operations));
    CHECK_EQ(error.code, pjson::PatchError::ResourceLimit);
    CHECK(*doc == before);

    pjson::PatchOptions nodes;
    nodes.maxClonedNodes = 1;
    CHECK(!doc->applyPatch(patch, error, nodes));
    CHECK_EQ(error.code, pjson::PatchError::ResourceLimit);
    CHECK(*doc == before);

    pjson::PatchOptions bytes;
    bytes.maxClonedBytes = 1;
    CHECK(!doc->applyPatch(patch, error, bytes));
    CHECK_EQ(error.code, pjson::PatchError::ResourceLimit);
    CHECK(*doc == before);

    pjson one = makePatchArray();
    one[0]["op"] = "replace";
    one[0]["path"] = "/a";
    one[0]["value"] = int64_t(4);
    CHECK(doc->applyPatch(one, error));
    CHECK(error.ok);
    CHECK_EQ(error.code, pjson::PatchError::Ok);
    CHECK_EQ(mustGetInt(*doc->find("a")), int64_t(4));
}

TEST(patch_large_string_value_and_copy_respect_clone_byte_limit) {
    pjson::unique_ptr doc = parseChecked(R"({"src":"small","keep":1})");
    const pjson before(*doc);
    const std::string large(4096, 'x');

    pjson add = makePatchArray();
    add[0]["op"] = "add";
    add[0]["path"] = "/large";
    add[0]["value"] = large;
    pjson::PatchOptions bytes;
    bytes.maxClonedBytes = 1024;
    pjson::PatchError error;
    CHECK(!doc->applyPatch(add, error, bytes));
    CHECK_EQ(error.code, pjson::PatchError::ResourceLimit);
    CHECK(*doc == before);

    (*doc)["src"] = large;
    const pjson beforeCopy(*doc);
    pjson copy = makePatchArray();
    copy[0]["op"] = "copy";
    copy[0]["from"] = "/src";
    copy[0]["path"] = "/dst";
    CHECK(!doc->applyPatch(copy, error, bytes));
    CHECK_EQ(error.code, pjson::PatchError::ResourceLimit);
    CHECK(*doc == beforeCopy);
}

TEST(patch_work_limit_bounds_deep_pointer_and_test_equality) {
    pjson doc = makeDeepObjectChain(32, int64_t(1));
    const pjson before(doc);
    const std::string path = repeatedObjectPointer("x", 32);
    pjson patch = makePatchArray();
    patch[0]["op"] = "test";
    patch[0]["path"] = path;
    patch[0]["value"] = int64_t(1);

    pjson::PatchOptions work;
    work.maxWork = 16;
    pjson::PatchError error;
    CHECK(!doc.applyPatch(patch, error, work));
    CHECK_EQ(error.code, pjson::PatchError::ResourceLimit);
    CHECK(doc == before);
}

TEST(patch_zero_limits_use_safe_ceilings_for_small_documents) {
    pjson::unique_ptr doc = parseChecked(R"({"a":1})");
    pjson patch = makePatchArray();
    patch[0]["op"] = "replace";
    patch[0]["path"] = "/a";
    patch[0]["value"] = int64_t(2);
    pjson::PatchOptions options;
    options.maxOperations = 0;
    options.maxClonedNodes = 0;
    options.maxClonedBytes = 0;
    options.maxWork = 0;
    CHECK(doc->applyPatch(patch, options));
    CHECK_EQ(mustGetInt(*doc->find("a")), int64_t(2));
}

TEST(patch_deep_pointer_and_patch_are_iterative_safe) {
    const int depth = 1500;
    pjson doc = makeDeepObjectChain(depth, 1);
    const std::string path = repeatedObjectPointer("x", depth);

    const pjson* before = doc.findPointer(path);
    CHECK(before != nullptr);
    CHECK_EQ(mustGetInt(*before), int64_t(1));

    pjson patch = makePatchArray();
    patch[0]["op"] = "replace";
    patch[0]["path"] = path;
    patch[0]["value"] = static_cast<int64_t>(2);

    CHECK(doc.applyPatch(patch));
    const pjson* after = doc.findPointer(path);
    CHECK(after != nullptr);
    CHECK_EQ(mustGetInt(*after), int64_t(2));
}

//===----------------------------------------------------------------------===//
// JSON Merge Patch (RFC 7396)
//===----------------------------------------------------------------------===//
TEST(merge_patch_rfc7396_primary_example) {
    pjson::unique_ptr doc = parseChecked(
        R"({"title":"Goodbye!","author":{"givenName":"John","familyName":"Doe"},"tags":["example","sample"],"content":"This will be unchanged"})");
    pjson::unique_ptr patch = parseChecked(
        R"({"title":"Hello!","phoneNumber":"+01-123-456-7890","author":{"familyName":null},"tags":["example"]})");

    pjson::PatchError err;
    CHECK(doc->applyMergePatch(*patch, err));
    CHECK(err.ok);
    CHECK_EQ(doc->toString(),
             std::string("{\"author\":{\"givenName\":\"John\"},\"content\":\"This will be "
                         "unchanged\",\"phoneNumber\":\"+01-123-456-7890\",\"tags\":[\"example\"],"
                         "\"title\":\"Hello!\"}"));
}

TEST(merge_patch_null_members_remove_object_keys) {
    pjson::unique_ptr doc = parseChecked(R"({"a":1,"b":2,"c":{"x":1,"y":2}})");
    pjson::unique_ptr patch = parseChecked(R"({"a":null,"c":{"y":null}})");

    CHECK(doc->applyMergePatch(*patch));
    CHECK_EQ(doc->toString(), std::string("{\"b\":2,\"c\":{\"x\":1}}"));
}

TEST(merge_patch_non_object_patch_replaces_entire_target) {
    pjson::unique_ptr arrayDoc = parseChecked(R"({"a":1})");
    pjson::unique_ptr arrayPatch = parseChecked(R"([1,2,3])");
    CHECK(arrayDoc->applyMergePatch(*arrayPatch));
    CHECK_EQ(arrayDoc->toString(), std::string("[1,2,3]"));

    pjson::unique_ptr nullDoc = parseChecked(R"({"a":1})");
    pjson nullPatch;
    CHECK(nullDoc->applyMergePatch(nullPatch));
    CHECK(nullDoc->isNull());

    pjson::unique_ptr scalarDoc = parseChecked(R"({"a":1})");
    pjson scalarPatch;
    scalarPatch = static_cast<int64_t>(7);
    CHECK(scalarDoc->applyMergePatch(scalarPatch));
    CHECK_EQ(mustGetInt(*scalarDoc), int64_t(7));
}

TEST(merge_patch_when_target_is_non_object_object_patch_starts_from_empty_object) {
    pjson doc;
    doc = static_cast<int64_t>(5);
    pjson::unique_ptr patch = parseChecked(R"({"a":1,"b":{"c":2}})");

    CHECK(doc.applyMergePatch(*patch));
    CHECK_EQ(doc.toString(), std::string("{\"a\":1,\"b\":{\"c\":2}}"));
}

TEST(merge_patch_arrays_are_replaced_wholesale_not_merged_elementwise) {
    pjson::unique_ptr doc = parseChecked(R"({"arr":[1,2,3],"obj":{"arr":[4,5]}})");
    pjson::unique_ptr patch = parseChecked(R"({"arr":[9],"obj":{"arr":[7,8,9]}})");

    CHECK(doc->applyMergePatch(*patch));
    CHECK_EQ(doc->toString(), std::string("{\"arr\":[9],\"obj\":{\"arr\":[7,8,9]}}"));
}

TEST(merge_patch_empty_object_is_no_op) {
    pjson::unique_ptr doc = parseChecked(R"({"a":1,"b":{"c":2}})");
    const pjson before(*doc);
    pjson::unique_ptr patch = parseChecked(R"({})");

    CHECK(doc->applyMergePatch(*patch));
    CHECK(*doc == before);
}

TEST(merge_patch_resource_limits_are_atomic) {
    pjson::unique_ptr doc = parseChecked(R"({"keep":1,"nested":{"old":true}})");
    const pjson before(*doc);
    pjson::unique_ptr patch = parseChecked(R"({"nested":{"new":2},"added":3})");

    pjson::PatchOptions options;
    options.maxWork = 1;
    pjson::PatchError error;
    CHECK(!doc->applyMergePatch(*patch, error, options));
    CHECK_EQ(error.code, pjson::PatchError::ResourceLimit);
    CHECK(*doc == before);

    options = pjson::PatchOptions();
    options.maxClonedNodes = 1;
    CHECK(!doc->applyMergePatch(*patch, error, options));
    CHECK_EQ(error.code, pjson::PatchError::ResourceLimit);
    CHECK(*doc == before);
}

TEST(merge_patch_preserves_resource_limit_from_member_processing) {
    pjson target;
    pjson::unique_ptr patch = parseChecked(R"({"a":1})");
    pjson::PatchOptions options;
    options.maxClonedNodes = 1;
    pjson::PatchError error;

    CHECK(!target.applyMergePatch(*patch, error, options));
    CHECK_EQ(error.code, pjson::PatchError::ResourceLimit);
    CHECK(target.isNull());
}

TEST(merge_patch_deep_object_merge_is_iterative_safe) {
    const int depth = 1500;
    pjson doc = makeDeepObjectChain(depth, 1);
    pjson patch;
    pjson* cur = &patch;
    for (int i = 0; i < depth - 1; ++i) {
        cur = &((*cur)["x"]);
    }
    (*cur)["x"] = int64_t(2);
    const std::string path = repeatedObjectPointer("x", depth);

    CHECK(doc.applyMergePatch(patch));
    const pjson* leaf = doc.findPointer(path);
    CHECK(leaf != nullptr);
    CHECK_EQ(mustGetInt(*leaf), int64_t(2));
}
