//
// Copyright 2025 ByteDance Ltd. and/or its affiliates. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License");
//
#ifndef PRAVEENJSON_SCHEMA_UTIL_H
#define PRAVEENJSON_SCHEMA_UTIL_H

#include "pjson.h"

#include <cstddef>
#include <string>

namespace ByteDance {
    namespace pjson_schema_detail {
        std::string strOf(const pjson& aValue);
        bool boolOf(const pjson& aValue);
        std::string numberText(const pjson& aValue);
        double numberAsDouble(const pjson& aValue);
        int utf8Len(const char* aSource, size_t aPosition, size_t aEnd);
        std::string typeName(const pjson& aNode);
        bool typeMatches(const pjson& aNode, const std::string& aType);
        bool isSafeRegex(const std::string& aPattern);
        std::string formatNumber(const pjson& aValue);
        bool schemaSize(const pjson& aValue, size_t& aResult, bool& aAboveRange);
        bool isExactMultiple(const pjson& aValue, const pjson& aDivisor);
        bool knownFormatValid(const std::string& aFormat, const std::string& aValue, bool& aKnown);
        bool percentDecodeFragment(const std::string& aFragment, std::string& aDecoded);
        bool uriHasScheme(const std::string& aUri);
        bool validAnchorName(const std::string& aName);
        std::string stripFragment(const std::string& aUri);
        void splitReference(const std::string& aUri, std::string& aDocument,
                            std::string& aFragment);
        std::string resolveUri(const std::string& aBaseWithFragment, const std::string& aReference);
    } // namespace pjson_schema_detail
} // namespace ByteDance

#endif // PRAVEENJSON_SCHEMA_UTIL_H
