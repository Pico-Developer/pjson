// SPDX-FileCopyrightText: 2026 ByteDance Ltd. and/or its affiliates
// SPDX-License-Identifier: Apache-2.0
#ifndef PRAVEENJSON_SCHEMA_DIALECT_H
#define PRAVEENJSON_SCHEMA_DIALECT_H

#include "pjson_schema.h"

#include <string>

namespace ByteDance {
    namespace pjson_schema_detail {
        enum Vocabulary {
            VCore = 1U << 0U,
            VApplicator = 1U << 1U,
            VUnevaluated = 1U << 2U,
            VValidation = 1U << 3U,
            VMetadata = 1U << 4U,
            VFormatAnnotation = 1U << 5U,
            VFormatAssertion = 1U << 6U,
            VContent = 1U << 7U
        };

        struct DialectPolicy {
            unsigned vocabularies;
            bool refSiblings;
            bool assertFormats;
            DialectPolicy();
        };

        const char* documentedSubsetDialect();
        const char* documentedSubsetVocabulary();
        const char* draft2020Dialect();
        bool hasVocabulary(const DialectPolicy& aPolicy, Vocabulary aVocabulary);
        bool vocabularyForUri(const std::string& aUri, Vocabulary& aVocabulary);
        DialectPolicy subsetPolicy(const pJsonSchemaValidator::Options& aOptions);
        DialectPolicy draft2020Policy(const pJsonSchemaValidator::Options& aOptions);
    } // namespace pjson_schema_detail
} // namespace ByteDance

#endif
