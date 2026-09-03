//
// Copyright 2025 ByteDance Ltd. and/or its affiliates. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License");
//
#include "pjson_schema_regex.h"
#include "pjson_schema_util.h"

#include <cstddef>

namespace ByteDance {
    namespace pjson_schema_detail {
        namespace {
            bool isAsciiDigit(char ch) {
                return ch >= '0' && ch <= '9';
            }

            bool isAsciiHex(char ch) {
                return isAsciiDigit(ch) || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
            }

            bool parseFixedDigits(const std::string& value, size_t offset, size_t count,
                                  int& result) {
                if (offset > value.size() || count > value.size() - offset)
                    return false;
                result = 0;
                for (size_t i = 0; i < count; ++i) {
                    if (!isAsciiDigit(value[offset + i]))
                        return false;
                    result = result * 10 + (value[offset + i] - '0');
                }
                return true;
            }

            bool isLeapYear(int year) {
                return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
            }

            bool validDate(const std::string& value) {
                if (value.size() != 10 || value[4] != '-' || value[7] != '-')
                    return false;
                int year = 0, month = 0, day = 0;
                if (!parseFixedDigits(value, 0, 4, year) || !parseFixedDigits(value, 5, 2, month) ||
                    !parseFixedDigits(value, 8, 2, day) || month < 1 || month > 12 || day < 1)
                    return false;
                static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
                int maxDay = days[month - 1];
                if (month == 2 && isLeapYear(year))
                    maxDay = 29;
                return day <= maxDay;
            }

            bool validTime(const std::string& value) {
                if (value.size() < 9 || value[2] != ':' || value[5] != ':')
                    return false;
                int hour = 0, minute = 0, second = 0;
                if (!parseFixedDigits(value, 0, 2, hour) ||
                    !parseFixedDigits(value, 3, 2, minute) ||
                    !parseFixedDigits(value, 6, 2, second) || hour > 23 || minute > 59 ||
                    second > 60)
                    return false;
                size_t pos = 8;
                if (pos < value.size() && value[pos] == '.') {
                    ++pos;
                    const size_t fractionStart = pos;
                    while (pos < value.size() && isAsciiDigit(value[pos]))
                        ++pos;
                    if (pos == fractionStart)
                        return false;
                }
                int offsetMinutes = 0;
                if (pos < value.size() && (value[pos] == 'Z' || value[pos] == 'z')) {
                    ++pos;
                } else {
                    if (pos + 6 != value.size() || (value[pos] != '+' && value[pos] != '-') ||
                        value[pos + 3] != ':')
                        return false;
                    int offsetHour = 0, offsetMinute = 0;
                    if (!parseFixedDigits(value, pos + 1, 2, offsetHour) ||
                        !parseFixedDigits(value, pos + 4, 2, offsetMinute) || offsetHour > 23 ||
                        offsetMinute > 59)
                        return false;
                    offsetMinutes = offsetHour * 60 + offsetMinute;
                    if (value[pos] == '-')
                        offsetMinutes = -offsetMinutes;
                    pos += 6;
                }
                if (pos != value.size())
                    return false;
                if (second == 60) {
                    int utcMinute = (hour * 60 + minute - offsetMinutes) % (24 * 60);
                    if (utcMinute < 0)
                        utcMinute += 24 * 60;
                    if (utcMinute != 23 * 60 + 59)
                        return false;
                }
                return true;
            }

            bool validIPv4(const std::string& value) {
                size_t pos = 0;
                for (int part = 0; part < 4; ++part) {
                    const size_t begin = pos;
                    int octet = 0;
                    while (pos < value.size() && isAsciiDigit(value[pos])) {
                        octet = octet * 10 + (value[pos] - '0');
                        if (octet > 255)
                            return false;
                        ++pos;
                    }
                    const size_t digits = pos - begin;
                    if (digits == 0 || digits > 3 || (digits > 1 && value[begin] == '0'))
                        return false;
                    if (part != 3) {
                        if (pos >= value.size() || value[pos] != '.')
                            return false;
                        ++pos;
                    }
                }
                return pos == value.size();
            }

            bool parseIPv6Side(const std::string& side, bool mayContainIPv4, int& units) {
                if (side.empty())
                    return true;
                size_t start = 0;
                while (start <= side.size()) {
                    const size_t colon = side.find(':', start);
                    const size_t end = colon == std::string::npos ? side.size() : colon;
                    if (end == start)
                        return false;
                    const std::string token = side.substr(start, end - start);
                    if (token.find('.') != std::string::npos) {
                        if (!mayContainIPv4 || end != side.size() || !validIPv4(token))
                            return false;
                        units += 2;
                    } else {
                        if (token.size() > 4)
                            return false;
                        for (size_t i = 0; i < token.size(); ++i) {
                            if (!isAsciiHex(token[i]))
                                return false;
                        }
                        ++units;
                    }
                    if (colon == std::string::npos)
                        break;
                    start = colon + 1;
                    if (start == side.size())
                        return false;
                }
                return true;
            }

            bool validIPv6(const std::string& value) {
                if (value.empty())
                    return false;
                const size_t compression = value.find("::");
                if (compression != std::string::npos &&
                    value.find("::", compression + 2) != std::string::npos)
                    return false;
                int units = 0;
                if (compression == std::string::npos)
                    return parseIPv6Side(value, true, units) && units == 8;
                const std::string left = value.substr(0, compression);
                const std::string right = value.substr(compression + 2);
                if (!parseIPv6Side(left, false, units) || !parseIPv6Side(right, true, units))
                    return false;
                return units < 8;
            }

            bool validUuid(const std::string& value) {
                if (value.size() != 36 || value[8] != '-' || value[13] != '-' || value[18] != '-' ||
                    value[23] != '-')
                    return false;
                for (size_t i = 0; i < value.size(); ++i) {
                    if (i == 8 || i == 13 || i == 18 || i == 23)
                        continue;
                    if (!isAsciiHex(value[i]))
                        return false;
                }
                return true;
            }
        } // namespace

        bool knownFormatValid(const std::string& format, const std::string& value, bool& known) {
            known = true;
            if (format == "date")
                return validDate(value);
            if (format == "time")
                return validTime(value);
            if (format == "date-time")
                return value.size() > 11 && (value[10] == 'T' || value[10] == 't') &&
                       validDate(value.substr(0, 10)) && validTime(value.substr(11));
            if (format == "ipv4")
                return validIPv4(value);
            if (format == "ipv6")
                return validIPv6(value);
            if (format == "uuid")
                return validUuid(value);
            if (format == "regex")
                return validEcmaRegex(value);
            known = false;
            return true;
        }
    } // namespace pjson_schema_detail
} // namespace ByteDance
