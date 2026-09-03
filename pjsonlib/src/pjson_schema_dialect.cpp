// SPDX-FileCopyrightText: 2026 ByteDance Ltd. and/or its affiliates
// SPDX-License-Identifier: Apache-2.0
#include "pjson_schema_dialect.h"

namespace ByteDance {
    namespace pjson_schema_detail {
        namespace {
            const char kSubsetDialect[] = "urn:bytedance:pjson:schema:documented-subset:2";
            const char kSubsetVocabulary[] =
                "urn:bytedance:pjson:schema:vocabulary:documented-subset:2";
            const char kDraft2020[] = "https://json-schema.org/draft/2020-12/schema";
        } // namespace

        DialectPolicy::DialectPolicy()
                : vocabularies(0)
                , refSiblings(false)
                , assertFormats(false) {}

        const char* documentedSubsetDialect() {
            return kSubsetDialect;
        }
        const char* documentedSubsetVocabulary() {
            return kSubsetVocabulary;
        }
        const char* draft2020Dialect() {
            return kDraft2020;
        }

        bool hasVocabulary(const DialectPolicy& policy, Vocabulary vocabulary) {
            return (policy.vocabularies & static_cast<unsigned>(vocabulary)) != 0U;
        }

        bool vocabularyForUri(const std::string& uri, Vocabulary& vocabulary) {
            static const char prefix[] = "https://json-schema.org/draft/2020-12/vocab/";
            if (uri.compare(0, sizeof(prefix) - 1, prefix) != 0)
                return false;
            const std::string name = uri.substr(sizeof(prefix) - 1);
            if (name == "core")
                vocabulary = VCore;
            else if (name == "applicator")
                vocabulary = VApplicator;
            else if (name == "unevaluated")
                vocabulary = VUnevaluated;
            else if (name == "validation")
                vocabulary = VValidation;
            else if (name == "meta-data")
                vocabulary = VMetadata;
            else if (name == "format-annotation")
                vocabulary = VFormatAnnotation;
            else if (name == "content")
                vocabulary = VContent;
            else
                return false;
            return true;
        }

        DialectPolicy subsetPolicy(const pJsonSchemaValidator::Options& options) {
            DialectPolicy policy;
            policy.vocabularies = VCore | VApplicator | VUnevaluated | VValidation | VMetadata |
                                  VFormatAnnotation | VFormatAssertion | VContent;
            policy.refSiblings = options.refSiblings;
            policy.assertFormats = options.validateFormats;
            return policy;
        }

        DialectPolicy draft2020Policy(const pJsonSchemaValidator::Options& options) {
            DialectPolicy policy;
            policy.vocabularies = VCore | VApplicator | VUnevaluated | VValidation | VMetadata |
                                  VFormatAnnotation | VContent;
            policy.refSiblings = true;
            policy.assertFormats = options.validateFormats;
            return policy;
        }
    } // namespace pjson_schema_detail
} // namespace ByteDance
