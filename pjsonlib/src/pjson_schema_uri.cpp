//
// Copyright 2025 ByteDance Ltd. and/or its affiliates. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License");
//
#include "pjson_schema_util.h"

#include <cstddef>
#include <vector>

namespace ByteDance {
    namespace pjson_schema_detail {
        namespace {
            bool isAsciiDigit(char ch) {
                return ch >= '0' && ch <= '9';
            }
            bool isAsciiHex(char ch) {
                return isAsciiDigit(ch) || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
            }

            std::string normalizePath(const std::string& path) {
                const bool absolute = !path.empty() && path[0] == '/';
                std::vector<std::string> segments;
                size_t begin = 0;
                while (begin <= path.size()) {
                    const size_t slash = path.find('/', begin);
                    const std::string segment = path.substr(
                        begin, slash == std::string::npos ? std::string::npos : slash - begin);
                    if (segment.empty() || segment == ".") {
                    } else if (segment == "..") {
                        if (!segments.empty())
                            segments.pop_back();
                    } else {
                        segments.push_back(segment);
                    }
                    if (slash == std::string::npos)
                        break;
                    begin = slash + 1;
                }
                std::string result = absolute ? "/" : std::string();
                for (size_t i = 0; i < segments.size(); ++i) {
                    if (!result.empty() && result[result.size() - 1] != '/')
                        result += '/';
                    result += segments[i];
                }
                if (!path.empty() && path[path.size() - 1] == '/' &&
                    (result.empty() || result[result.size() - 1] != '/'))
                    result += '/';
                return result;
            }

            void splitPathSuffix(const std::string& value, std::string& path, std::string& suffix) {
                const size_t marker = value.find_first_of("?#");
                path = marker == std::string::npos ? value : value.substr(0, marker);
                suffix = marker == std::string::npos ? std::string() : value.substr(marker);
            }
        } // namespace

        bool percentDecodeFragment(const std::string& fragment, std::string& decoded) {
            decoded.clear();
            for (size_t i = 0; i < fragment.size(); ++i) {
                if (fragment[i] != '%') {
                    decoded += fragment[i];
                    continue;
                }
                if (i + 2 >= fragment.size() || !isAsciiHex(fragment[i + 1]) ||
                    !isAsciiHex(fragment[i + 2]))
                    return false;
                const char hi = fragment[i + 1];
                const char lo = fragment[i + 2];
                const int high =
                    isAsciiDigit(hi) ? hi - '0' : (hi >= 'a' ? hi - 'a' + 10 : hi - 'A' + 10);
                const int low =
                    isAsciiDigit(lo) ? lo - '0' : (lo >= 'a' ? lo - 'a' + 10 : lo - 'A' + 10);
                decoded += static_cast<char>((high << 4) | low);
                i += 2;
            }
            return true;
        }

        bool uriHasScheme(const std::string& uri) {
            if (uri.empty() ||
                !((uri[0] >= 'A' && uri[0] <= 'Z') || (uri[0] >= 'a' && uri[0] <= 'z')))
                return false;
            for (size_t i = 1; i < uri.size(); ++i) {
                const char c = uri[i];
                if (c == ':')
                    return true;
                if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '+' || c == '-' || c == '.'))
                    return false;
            }
            return false;
        }

        bool validAnchorName(const std::string& name) {
            if (name.empty() || !((name[0] >= 'A' && name[0] <= 'Z') ||
                                  (name[0] >= 'a' && name[0] <= 'z') || name[0] == '_'))
                return false;
            for (size_t i = 1; i < name.size(); ++i) {
                const char c = name[i];
                if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '_' || c == '-' || c == '.' || c == ':'))
                    return false;
            }
            return true;
        }

        std::string stripFragment(const std::string& uri) {
            const size_t hash = uri.find('#');
            return hash == std::string::npos ? uri : uri.substr(0, hash);
        }

        void splitReference(const std::string& uri, std::string& document, std::string& fragment) {
            const size_t hash = uri.find('#');
            document = hash == std::string::npos ? uri : uri.substr(0, hash);
            fragment = hash == std::string::npos ? std::string() : uri.substr(hash + 1);
        }

        std::string resolveUri(const std::string& baseWithFragment, const std::string& reference) {
            const std::string base = stripFragment(baseWithFragment);
            if (reference.empty())
                return base;
            if (uriHasScheme(reference))
                return reference;
            if (reference[0] == '#')
                return base + reference;

            const size_t colon = base.find(':');
            if (colon == std::string::npos)
                return normalizePath(reference);
            const std::string scheme = base.substr(0, colon + 1);
            const std::string remainder = base.substr(colon + 1);
            if (remainder.compare(0, 2, "//") != 0)
                return scheme + reference;
            if (reference.compare(0, 2, "//") == 0)
                return scheme + reference;

            const size_t authorityEnd = remainder.find('/', 2);
            const std::string authority =
                authorityEnd == std::string::npos ? remainder : remainder.substr(0, authorityEnd);
            const std::string basePath = authorityEnd == std::string::npos
                                             ? std::string("/")
                                             : remainder.substr(authorityEnd);
            std::string referencePath;
            std::string referenceSuffix;
            splitPathSuffix(reference, referencePath, referenceSuffix);
            std::string cleanBasePath;
            std::string ignoredSuffix;
            splitPathSuffix(basePath, cleanBasePath, ignoredSuffix);
            if (reference[0] == '?')
                return scheme + authority + cleanBasePath + reference;
            if (!referencePath.empty() && referencePath[0] == '/')
                return scheme + authority + normalizePath(referencePath) + referenceSuffix;
            const size_t slash = cleanBasePath.rfind('/');
            const std::string directory =
                slash == std::string::npos ? std::string() : cleanBasePath.substr(0, slash + 1);
            return scheme + authority + normalizePath(directory + referencePath) + referenceSuffix;
        }
    } // namespace pjson_schema_detail
} // namespace ByteDance
