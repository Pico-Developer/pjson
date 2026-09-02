# SPDX-FileCopyrightText: 2026 ByteDance Ltd. and/or its affiliates
# SPDX-License-Identifier: Apache-2.0

if(NOT DEFINED PJSON_TEST_EXECUTABLE OR NOT DEFINED PJSON_TEST_OUTPUT)
    message(FATAL_ERROR "PJSON_TEST_EXECUTABLE and PJSON_TEST_OUTPUT are required")
endif()

execute_process(
    COMMAND "${PJSON_TEST_EXECUTABLE}" --list-tests
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "pjsontest discovery failed (${result}): ${error}")
endif()

string(REPLACE "\r\n" "\n" output "${output}")
string(REPLACE "\n" ";" tests "${output}")
set(content "# Generated from the compiled pjsontest registry.\n")
set(seen)
foreach(test_name IN LISTS tests)
    if(test_name STREQUAL "")
        continue()
    endif()
    if(NOT test_name MATCHES "^[A-Za-z0-9_]+$")
        message(FATAL_ERROR "Invalid registered pjson test name: ${test_name}")
    endif()
    if(test_name IN_LIST seen)
        message(FATAL_ERROR "Duplicate registered pjson test name: ${test_name}")
    endif()
    list(APPEND seen "${test_name}")
    string(APPEND content
        "add_test([=[pjson.${test_name}]=] [=[${PJSON_TEST_EXECUTABLE}]=] --run-test [=[${test_name}]=])\n")
endforeach()

list(LENGTH seen count)
if(count EQUAL 0)
    message(FATAL_ERROR "The compiled pjsontest registry is empty")
endif()

file(WRITE "${PJSON_TEST_OUTPUT}.tmp" "${content}")
file(RENAME "${PJSON_TEST_OUTPUT}.tmp" "${PJSON_TEST_OUTPUT}")
message(STATUS "Discovered ${count} pjson test cases from the compiled registry")
