// SPDX-FileCopyrightText: 2026 ByteDance Ltd. and/or its affiliates
// SPDX-License-Identifier: Apache-2.0
#ifndef PRAVEENJSON_SCHEMA_REGEX_H
#define PRAVEENJSON_SCHEMA_REGEX_H

#include <string>

namespace ByteDance {
    namespace pjson_schema_detail {
        class EcmaRegex {
        public:
            enum Result { Match, NoMatch, Invalid, WorkLimit };

            EcmaRegex();
            ~EcmaRegex();
            bool compile(const std::string& aPattern);
            Result search(const std::string& aSubject) const;

        private:
            EcmaRegex(const EcmaRegex&);
            EcmaRegex& operator=(const EcmaRegex&);
            struct Impl;
            Impl* _impl;
        };

        bool validEcmaRegex(const std::string& aPattern);
    } // namespace pjson_schema_detail
} // namespace ByteDance

#endif
