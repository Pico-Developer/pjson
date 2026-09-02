cmake_minimum_required(VERSION 3.21)

set (TARGET_NAME pjsontest)

project (${TARGET_NAME})
find_package(Threads REQUIRED)

# ---- Test sources -------------------------------------------------------

set (SRC_DIR "src")
set (INCLUDE_DIR "include")

# The unit test suite: the harness entry point plus every tests_*.cpp module,
# all linked into a single executable that self-registers its TEST() cases.
set (TEST_SRC_FILES
${SRC_DIR}/test_main.cpp
${SRC_DIR}/tests_core.cpp
${SRC_DIR}/tests_build.cpp
${SRC_DIR}/tests_parse.cpp
${SRC_DIR}/tests_strings.cpp
${SRC_DIR}/tests_roundtrip.cpp
${SRC_DIR}/tests_features.cpp
${SRC_DIR}/tests_schema.cpp
${SRC_DIR}/tests_malformed.cpp
${SRC_DIR}/tests_mutation.cpp
${SRC_DIR}/tests_schema_complex.cpp
${SRC_DIR}/tests_api_edge.cpp
${SRC_DIR}/tests_fuzz.cpp
${SRC_DIR}/tests_pathological.cpp
${SRC_DIR}/tests_conformance.cpp
${SRC_DIR}/tests_schema_vocabulary.cpp
${SRC_DIR}/tests_schema_official.cpp
${SRC_DIR}/tests_storage.cpp
${SRC_DIR}/tests_allocator.cpp
${SRC_DIR}/tests_streaming.cpp
${SRC_DIR}/tests_serialize_access.cpp
${SRC_DIR}/tests_pointer_patch.cpp
${SRC_DIR}/tests_embedded_nul.cpp
${SRC_DIR}/tests_aliasing.cpp
${SRC_DIR}/tests_numbers.cpp
${SRC_DIR}/tests_depth_frontends.cpp
${SRC_DIR}/tests_error_model.cpp
${SRC_DIR}/tests_dom_api.cpp
${SRC_DIR}/tests_serialize_limits.cpp
${SRC_DIR}/tests_schema_2020.cpp
)

# ---- Test target --------------------------------------------------------

set(${TARGET_NAME}_libs
    "pjson"
	)

set (INC_DIRS ${INC_DIR}
${CMAKE_CURRENT_SOURCE_DIR}
${INCLUDE_DIR}
"../pjsonlib/include"
)

set (CMAKE_CXX_STANDARD 11)
set (CMAKE_CXX_STANDARD_REQUIRED ON)
if (MSVC)
    set (PJSON_TEST_WARN_FLAGS /W4)
else()
    set (PJSON_TEST_WARN_FLAGS -Wall -Wextra)
endif()

# Single assertion-based executable returns non-zero on any failing case.
add_executable(${TARGET_NAME} ${TEST_SRC_FILES})
target_include_directories(${TARGET_NAME} PUBLIC ${INC_DIRS})
target_link_libraries(${TARGET_NAME} ${${TARGET_NAME}_libs} Threads::Threads)
target_compile_options(${TARGET_NAME} PRIVATE ${PJSON_TEST_WARN_FLAGS})
target_compile_definitions(${TARGET_NAME} PRIVATE
    PJSON_TEST_DEFAULT_JSONTESTSUITE_DIR="${CMAKE_SOURCE_DIR}/.test-corpora/JSONTestSuite"
    PJSON_TEST_DEFAULT_JSON_SCHEMA_TEST_SUITE_DIR="${CMAKE_SOURCE_DIR}/.test-corpora/JSON-Schema-Test-Suite")

enable_testing()

# ---- CTest case discovery ----------------------------------------------

# Keep one test executable, but expose each self-registered TEST() separately to
# CTest. Extracting TEST(name) declarations at configure time lets CMake resolve
# the executable correctly for single- and multi-config generators on every OS.
set(PJSON_REGISTERED_TESTS)
foreach(test_source IN LISTS TEST_SRC_FILES)
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/${test_source}" test_declarations
         REGEX "TEST[(][A-Za-z0-9_]+[)]")
    foreach(declaration IN LISTS test_declarations)
        string(REGEX MATCH "TEST[(]([A-Za-z0-9_]+)[)]" unused "${declaration}")
        set(test_name "${CMAKE_MATCH_1}")
        if(test_name IN_LIST PJSON_REGISTERED_TESTS)
            message(FATAL_ERROR "Duplicate pjson test name: ${test_name}")
        endif()
        list(APPEND PJSON_REGISTERED_TESTS "${test_name}")
        add_test(NAME "pjson.${test_name}" COMMAND ${TARGET_NAME} --run-test "${test_name}")
    endforeach()
endforeach()
list(LENGTH PJSON_REGISTERED_TESTS PJSON_TEST_COUNT)
message(STATUS "Registered ${PJSON_TEST_COUNT} pjson test cases with CTest")
