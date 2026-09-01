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
// Robustness / fuzz tests. These are deterministic (fixed RNG seeds) so a
// failure is reproducible, and are written to run cleanly under
// AddressSanitizer / UndefinedBehaviorSanitizer (see build.sh --asan). They
// stress the paths a contributor is most likely to break: random valid
// documents through the full round-trip, random byte soup through the parser,
// random mutation sequences, and random schemas.
//
#include "pjson.h"
#include "test_harness.h"
#include "test_util.h"

#include <random>
#include <string>
#include <vector>

using namespace ByteDance;
using pjson_test::parse;

namespace {

    // Builds a random JSON value up to the given depth. Uses only depths well
    // within the default parse guard so serialize/parse/copy never overflow.
    void buildRandom(pjson& node, std::mt19937& rng, int depth) {
        std::uniform_int_distribution<int> kind(0, depth > 0 ? 6 : 4);
        switch (kind(rng)) {
            case 0:
                node.reset();
                break;
            case 1:
                node = (rng() & 1) != 0;
                break;
            case 2:
                node = static_cast<int64_t>(
                    std::uniform_int_distribution<long long>(-1000000000LL, 1000000000LL)(rng));
                break;
            case 3:
                node = std::uniform_real_distribution<double>(-1e6, 1e6)(rng);
                break;
            case 4: {
                // Draw complete UTF-8 fragments, never individual bytes from a
                // multibyte code point. The generated DOM must serialize to a
                // strictly valid RFC 8259 document.
                static const char* fragments[] = {"a",  "b", " ",    "\"",       "\\", "\n",
                                                  "\t", "/", "\x01", "\xC3\xA9", "z"};
                std::uniform_int_distribution<int> len(0, 10);
                std::uniform_int_distribution<int> pick(
                    0, static_cast<int>(sizeof(fragments) / sizeof(fragments[0])) - 1);
                std::string s;
                int n = len(rng);
                for (int i = 0; i < n; ++i)
                    s += fragments[pick(rng)];
                node = s;
                break;
            }
            case 5: {
                node.resetTo(pjson::jsonArray);
                std::uniform_int_distribution<int> len(0, 5);
                int n = len(rng);
                for (int i = 0; i < n; ++i)
                    buildRandom(node[i], rng, depth - 1);
                break;
            }
            default: {
                node.resetTo(pjson::jsonObject);
                std::uniform_int_distribution<int> len(0, 5);
                int n = len(rng);
                for (int i = 0; i < n; ++i)
                    buildRandom(node["k" + std::to_string(i)], rng, depth - 1);
                break;
            }
        }
    }

} // namespace

//===----------------------------------------------------------------------===//
// Random valid documents survive serialize -> parse -> serialize unchanged,
// in both compact and pretty form, and equal themselves after a round-trip.
//===----------------------------------------------------------------------===//
TEST(fuzz_valid_document_round_trip) {
    std::mt19937 rng(0xABCDEF01u);
    for (int iter = 0; iter < 2000; ++iter) {
        pjson doc;
        buildRandom(doc, rng, 5);

        std::string compact = doc.toString();
        auto rc = parse(compact);
        CHECK(rc != nullptr);
        if (rc) {
            CHECK_EQ(rc->toString(), compact);
            CHECK(*rc == doc); // structural equality holds
        }

        std::string pretty = doc.toString(pjson::SerializeOptions::prettyPrinted());
        auto rp = parse(pretty);
        CHECK(rp != nullptr);
        if (rp)
            CHECK_EQ(rp->toString(), compact);
    }
}

//===----------------------------------------------------------------------===//
// Random byte strings never crash or throw the parser; anything that does
// parse must re-serialize/re-parse consistently.
//===----------------------------------------------------------------------===//
TEST(fuzz_random_bytes_parser) {
    std::mt19937 rng(0x1234ABCDu);
    std::uniform_int_distribution<int> byte(0, 255);
    std::uniform_int_distribution<int> len(0, 60);
    for (int iter = 0; iter < 5000; ++iter) {
        std::string s;
        int n = len(rng);
        for (int i = 0; i < n; ++i)
            s += static_cast<char>(byte(rng));
        auto p = parse(s); // must not throw / crash
        if (p) {
            std::string out = p->toString();
            auto p2 = parse(out);
            CHECK(p2 != nullptr);
            if (p2)
                CHECK_EQ(p2->toString(), out);
        }
    }
    CHECK(true); // reaching here means no crash across all iterations
}

//===----------------------------------------------------------------------===//
// Random byte strings biased toward JSON punctuation exercise the structural
// error paths harder.
//===----------------------------------------------------------------------===//
TEST(fuzz_json_flavored_bytes) {
    std::mt19937 rng(0x55AA55AAu);
    static const char alphabet[] = "{}[]:,\"\\0123456789.eE+-tfnul truefalsenull \t\n";
    std::uniform_int_distribution<int> pick(0, static_cast<int>(sizeof(alphabet)) - 2);
    std::uniform_int_distribution<int> len(0, 40);
    for (int iter = 0; iter < 5000; ++iter) {
        std::string s;
        int n = len(rng);
        for (int i = 0; i < n; ++i)
            s += alphabet[pick(rng)];
        auto p = parse(s);
        if (p) {
            auto p2 = parse(p->toString());
            CHECK(p2 != nullptr);
            if (p2)
                CHECK(*p2 == *p);
        }
    }
    CHECK(true);
}

//===----------------------------------------------------------------------===//
// Random mutation sequences (add / overwrite / erase / clear) keep the tree
// self-consistent and always serializable + round-trippable.
//===----------------------------------------------------------------------===//
TEST(fuzz_mutation_sequence) {
    std::mt19937 rng(0x0BADC0DEu);
    for (int iter = 0; iter < 300; ++iter) {
        pjson doc;
        doc.resetTo(pjson::jsonObject);
        int ops = std::uniform_int_distribution<int>(1, 40)(rng);
        for (int o = 0; o < ops; ++o) {
            int action = std::uniform_int_distribution<int>(0, 5)(rng);
            std::string key = "k" + std::to_string(std::uniform_int_distribution<int>(0, 9)(rng));
            switch (action) {
                case 0:
                    doc[key] = static_cast<int64_t>(rng());
                    break;
                case 1:
                    doc[key] = std::string("v");
                    break;
                case 2:
                    doc[key] = std::vector<int64_t>({1, 2, 3});
                    break;
                case 3:
                    doc[key][std::uniform_int_distribution<int>(0, 5)(rng)] = int64_t(7);
                    break;
                case 4:
                    doc.erase(key);
                    break;
                default:
                    if (doc.find(key))
                        doc[key].clear();
                    break;
            }
        }
        // Whatever state we ended in must serialize and round-trip.
        std::string s = doc.toString();
        auto rt = parse(s);
        CHECK(rt != nullptr);
        if (rt)
            CHECK(*rt == doc);
    }
}

//===----------------------------------------------------------------------===//
// Validating random documents against random schemas never crashes and always
// yields a definite pass/fail (with errors collected only on failure).
//===----------------------------------------------------------------------===//
TEST(fuzz_schema_validation_never_crashes) {
    std::mt19937 rng(0xFEEDFACEu);
    // A pool of small schema fragments to combine.
    const char* fragments[] = {
        R"({"type":"object"})",
        R"({"type":"array","items":{"type":"integer"}})",
        R"({"required":["k0","k1"]})",
        R"({"properties":{"k0":{"type":"string"},"k1":{"type":"integer","minimum":0}}})",
        R"({"minProperties":1,"maxProperties":4})",
        R"({"enum":[1,"v",true,null]})",
        R"({"anyOf":[{"type":"string"},{"type":"integer"}]})",
        R"({"not":{"required":["k9"]}})",
        R"({"additionalProperties":false,"properties":{"k0":{}}})",
        R"(true)",
        R"(false)",
        R"({})",
    };
    const int nFragments = static_cast<int>(sizeof(fragments) / sizeof(fragments[0]));

    for (int iter = 0; iter < 1000; ++iter) {
        auto schema = parse(fragments[std::uniform_int_distribution<int>(0, nFragments - 1)(rng)]);
        CHECK(schema != nullptr);

        pjson doc;
        buildRandom(doc, rng, 3);

        std::vector<pjson_test::SchemaError> errors;
        bool ok = pjson_test::schemaValidate(doc, *schema, errors);
        // The contract: ok == errors.empty(). Also validate() (no errors arg)
        // must agree with the collecting form.
        CHECK_EQ(ok, errors.empty());
        CHECK_EQ(ok, pjson_test::schemaValidate(doc, *schema));
    }
}

//===----------------------------------------------------------------------===//
// Malformed schemas must be handled deterministically without crashing. Most
// wrong-shaped keywords are ignored; invalid regex syntax fails validation.
//===----------------------------------------------------------------------===//
TEST(fuzz_malformed_schemas_tolerated) {
    const char* ignoredSchemas[] = {
        R"({"type":123})",      R"({"required":"notarray"})", R"({"properties":"no"})",
        R"({"items":42})",      R"({"enum":"notarray"})",     R"({"minimum":"5"})",
        R"({"minLength":-1})",  R"({"minItems":-3})",         R"({"multipleOf":0})",
        R"({"multipleOf":-2})", R"({"allOf":"x"})",           R"({"anyOf":{}})",
    };
    for (const char* bs : ignoredSchemas) {
        auto schema = parse(bs);
        CHECK(schema != nullptr);
        const char* values[] = {"5", "\"str\"", "[1,2,3]", R"({"k0":1})", "true", "null"};
        for (const char* v : values) {
            auto d = parse(v);
            std::vector<pjson_test::SchemaError> errors;
            CHECK(pjson_test::schemaValidate(*d, *schema, errors));
            CHECK(errors.empty());
        }
    }

    auto invalidRegex = parse(R"({"pattern":"([unclosed"})");
    auto stringValue = parse("\"value\"");
    std::vector<pjson_test::SchemaError> errors;
    CHECK(!pjson_test::schemaValidate(*stringValue, *invalidRegex, errors));
    CHECK(!errors.empty());
}

//===----------------------------------------------------------------------===//
// Copy / move of random documents produce independent, equal trees.
//===----------------------------------------------------------------------===//
TEST(fuzz_copy_move_independence) {
    std::mt19937 rng(0x99887766u);
    for (int iter = 0; iter < 500; ++iter) {
        pjson a;
        buildRandom(a, rng, 4);

        pjson b(a); // copy ctor
        CHECK(a == b);

        pjson c;
        c = a; // copy assign
        CHECK(a == c);

        std::string before = a.toString();
        pjson d(std::move(c)); // move ctor
        CHECK_EQ(d.toString(), before);
        CHECK(c.isNull()); // moved-from is null

        // Mutating the copy must not disturb the original.
        b.resetTo(pjson::jsonArray);
        b += int64_t(12345);
        CHECK_EQ(a.toString(), before);
    }
}
