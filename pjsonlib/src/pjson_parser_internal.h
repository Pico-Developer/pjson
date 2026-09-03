// SPDX-FileCopyrightText: 2026 ByteDance Ltd. and/or its affiliates
// SPDX-License-Identifier: Apache-2.0
#ifndef PRAVEENJSON_PARSER_INTERNAL_H
#define PRAVEENJSON_PARSER_INTERNAL_H

#include "pjson_internal.h"
#include "pjson_parser.h"

namespace ByteDance {
    struct pJsonParserImpl {
        static const int DepthHardLimit = 1024;

        pjson::Allocator* allocator;
        pJsonParser::Options options;

        pJsonParserImpl(pjson::Allocator& aAllocator, const pJsonParser::Options& aOptions)
                : allocator(&aAllocator)
                , options(aOptions) {}

        struct ParseCtx {
            const char* src;
            size_t pos;
            size_t end;
            pJsonParser::Options::DuplicateKeyPolicy duplicateKeys;
            pJsonParser::Options::NumberPolicy numberPolicy;
            int depth;
            int maxDepth;
            size_t nodeCount;
            size_t maxNodes;
            pjson::Allocator* allocator;
            bool failed;
            size_t errPos;
            std::string errMsg;
        };

        struct ParsedNumber {
            enum Kind { SignedInteger, UnsignedInteger, FloatingPoint };
            Kind kind;
            int64_t signedValue;
            uint64_t unsignedValue;
            double floatingValue;
        };

        static bool isWhitespace(char aChar);
        static void appendUtf8(uint32_t aCodePoint, std::string& aOut);
        static bool hex4(const char* aSource, size_t aStart, uint32_t& aOut);
        static bool parseDouble(const std::string& aText, double& aValue,
                                bool* aUnderflowToZero = nullptr);
        static bool convertNumberToken(const std::string& aText, bool aIsFloat,
                                       pJsonParser::Options::NumberPolicy aPolicy,
                                       ParsedNumber& aResult, const char*& aErrorMessage);

        static bool fail(ParseCtx& aContext, size_t aPosition, const char* aMessage);
        static pjson* newNode(ParseCtx& aContext);
        static bool peek(ParseCtx& aContext, char& aOut);
        static bool skipColon(ParseCtx& aContext);
        static bool parseValue(ParseCtx& aContext, pjson*& aOut);
        static bool parseString(ParseCtx& aContext, pjson*& aOut);
        static bool extractString(ParseCtx& aContext, std::string& aOut);
        static bool decodeStringBody(ParseCtx& aContext, std::string& aOut, bool aStopAtQuote);
        static bool parseKeyword(ParseCtx& aContext, pjson*& aOut);
        static bool parseNumber(ParseCtx& aContext, pjson*& aOut);
        static bool parseArray(ParseCtx& aContext, pjson*& aOut);
        static bool parseObject(ParseCtx& aContext, pjson*& aOut);

        static pjson parseTop(const char* aSource, size_t aSize,
                              const pJsonParser::Options& aOptions, pJsonParser::Error* aError,
                              pjson::Allocator& aAllocator);
        static pjson parseStream(std::istream& aInput, const pJsonParser::Options& aOptions,
                                 pJsonParser::Error* aError, pjson::Allocator& aAllocator);
        static bool parseSaxTop(const char* aSource, size_t aSize,
                                pJsonParser::SaxHandler& aHandler,
                                const pJsonParser::Options& aOptions, pJsonParser::Error* aError);
        static bool parseSaxStream(std::istream& aInput, pJsonParser::SaxHandler& aHandler,
                                   const pJsonParser::Options& aOptions,
                                   pJsonParser::Error* aError);
    };
} // namespace ByteDance

#endif // PRAVEENJSON_PARSER_INTERNAL_H
