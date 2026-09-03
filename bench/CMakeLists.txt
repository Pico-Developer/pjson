cmake_minimum_required(VERSION 3.21)

set(TARGET_NAME pjsonbench)

project(${TARGET_NAME})

set(SRC_DIR "src")

set(BENCH_SRC_FILES
    ${SRC_DIR}/benchmark_main.cpp
)

if(MSVC)
    set(PJSON_BENCH_WARN_FLAGS /W4)
    set(PJSON_BENCH_TARGET_FLAGS "/W4")
else()
    set(PJSON_BENCH_WARN_FLAGS -Wall -Wextra)
    set(PJSON_BENCH_TARGET_FLAGS "-Wall -Wextra")
endif()

# Capture configure-time provenance in the machine-readable benchmark report.
# A dirty marker is kept separate from the revision so consumers can reject
# locally modified builds without losing the useful commit identity.
find_package(Git QUIET)
get_filename_component(PJSON_BENCH_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(PJSON_BENCH_GIT_COMMIT "unknown")
set(PJSON_BENCH_GIT_DIRTY "unknown")
set(PJSON_BENCH_SOURCE_FINGERPRINT "unknown")
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${PJSON_BENCH_SOURCE_ROOT}"
        RESULT_VARIABLE PJSON_BENCH_GIT_RESULT
        OUTPUT_VARIABLE PJSON_BENCH_GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(NOT PJSON_BENCH_GIT_RESULT EQUAL 0)
        set(PJSON_BENCH_GIT_COMMIT "unknown")
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" status --porcelain
        WORKING_DIRECTORY "${PJSON_BENCH_SOURCE_ROOT}"
        RESULT_VARIABLE PJSON_BENCH_DIRTY_RESULT
        OUTPUT_VARIABLE PJSON_BENCH_DIRTY_OUTPUT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(PJSON_BENCH_DIRTY_RESULT EQUAL 0)
        if(PJSON_BENCH_DIRTY_OUTPUT STREQUAL "")
            set(PJSON_BENCH_GIT_DIRTY "false")
        else()
            set(PJSON_BENCH_GIT_DIRTY "true")
        endif()
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" diff --binary HEAD
        WORKING_DIRECTORY "${PJSON_BENCH_SOURCE_ROOT}"
        RESULT_VARIABLE PJSON_BENCH_DIFF_RESULT
        OUTPUT_VARIABLE PJSON_BENCH_SOURCE_DIFF
        ERROR_QUIET)
    if(PJSON_BENCH_DIFF_RESULT EQUAL 0)
        string(SHA256 PJSON_BENCH_DIFF_HASH "${PJSON_BENCH_SOURCE_DIFF}")
        set(PJSON_BENCH_SOURCE_FINGERPRINT
            "${PJSON_BENCH_GIT_COMMIT}:${PJSON_BENCH_DIFF_HASH}")
    endif()
endif()

if(CMAKE_BUILD_TYPE)
    set(PJSON_BENCH_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
else()
    set(PJSON_BENCH_BUILD_TYPE "multi-config/unspecified")
endif()
string(TOUPPER "${CMAKE_BUILD_TYPE}" PJSON_BENCH_BUILD_TYPE_UPPER)
set(PJSON_BENCH_BUILD_FLAGS "${CMAKE_CXX_FLAGS}")
if(PJSON_BENCH_BUILD_TYPE_UPPER AND
   DEFINED CMAKE_CXX_FLAGS_${PJSON_BENCH_BUILD_TYPE_UPPER})
    string(APPEND PJSON_BENCH_BUILD_FLAGS
        " ${CMAKE_CXX_FLAGS_${PJSON_BENCH_BUILD_TYPE_UPPER}}")
endif()
string(STRIP "${PJSON_BENCH_BUILD_FLAGS}" PJSON_BENCH_BUILD_FLAGS)

# Escape values before placing them in ordinary C++ string literals.
foreach(PJSON_BENCH_CONFIG_VALUE
        PJSON_BENCH_GIT_COMMIT PJSON_BENCH_GIT_DIRTY PJSON_BENCH_SOURCE_FINGERPRINT
        PJSON_BENCH_BUILD_TYPE
        PJSON_BENCH_BUILD_FLAGS PJSON_BENCH_TARGET_FLAGS
        CMAKE_CXX_COMPILER CMAKE_CXX_COMPILER_ID CMAKE_CXX_COMPILER_VERSION
        CMAKE_SYSTEM_NAME CMAKE_SYSTEM_VERSION CMAKE_SYSTEM_PROCESSOR)
    string(REPLACE "\\" "\\\\" ${PJSON_BENCH_CONFIG_VALUE}
        "${${PJSON_BENCH_CONFIG_VALUE}}")
    string(REPLACE "\"" "\\\"" ${PJSON_BENCH_CONFIG_VALUE}
        "${${PJSON_BENCH_CONFIG_VALUE}}")
endforeach()
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/benchmark_build_config.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/benchmark_build_config.h
    @ONLY)

option(PJSON_BENCH_COMPARE "Enable optional third-party JSON benchmark comparisons" OFF)
set(PJSON_BENCH_DEPS_DIR "${CMAKE_SOURCE_DIR}/.benchmark-deps"
    CACHE PATH "Directory containing pinned benchmark comparison dependencies")

add_executable(${TARGET_NAME} ${BENCH_SRC_FILES})
target_link_libraries(${TARGET_NAME} PRIVATE pjson::pjson)
target_include_directories(${TARGET_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_features(${TARGET_NAME} PRIVATE cxx_std_11)
target_compile_options(${TARGET_NAME} PRIVATE ${PJSON_BENCH_WARN_FLAGS})

if(PJSON_BENCH_COMPARE)
    set(PJSON_BENCH_NLOHMANN_INCLUDE
        "${PJSON_BENCH_DEPS_DIR}/nlohmann-json-v3.11.3/single_include")
    set(PJSON_BENCH_RAPIDJSON_INCLUDE
        "${PJSON_BENCH_DEPS_DIR}/rapidjson-v1.1.0/include")
    set(PJSON_BENCH_SIMDJSON_SOURCE_DIR
        "${PJSON_BENCH_DEPS_DIR}/simdjson-v3.12.2")
    set(PJSON_BENCH_SIMDJSON_CMAKE_DIR "${CMAKE_CURRENT_BINARY_DIR}/simdjson")

    if(NOT EXISTS "${PJSON_BENCH_NLOHMANN_INCLUDE}/nlohmann/json.hpp")
        message(FATAL_ERROR
            "PJSON_BENCH_COMPARE=ON but nlohmann/json v3.11.3 was not found under "
            "${PJSON_BENCH_NLOHMANN_INCLUDE}. Fetch the pinned dependencies first.")
    endif()
    if(NOT EXISTS "${PJSON_BENCH_RAPIDJSON_INCLUDE}/rapidjson/document.h")
        message(FATAL_ERROR
            "PJSON_BENCH_COMPARE=ON but RapidJSON v1.1.0 was not found under "
            "${PJSON_BENCH_RAPIDJSON_INCLUDE}. Fetch the pinned dependencies first.")
    endif()
    if(NOT EXISTS "${PJSON_BENCH_SIMDJSON_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "PJSON_BENCH_COMPARE=ON but simdjson v3.12.2 was not found under "
            "${PJSON_BENCH_SIMDJSON_SOURCE_DIR}. Fetch the pinned dependencies first.")
    endif()

    set(SIMDJSON_DEVELOPER_MODE OFF CACHE BOOL "" FORCE)
    set(SIMDJSON_GOOGLE_BENCHMARKS OFF CACHE BOOL "" FORCE)
    set(SIMDJSON_ENABLE_THREADS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${PJSON_BENCH_SIMDJSON_SOURCE_DIR}" "${PJSON_BENCH_SIMDJSON_CMAKE_DIR}")

    target_include_directories(${TARGET_NAME} SYSTEM PRIVATE
        "${PJSON_BENCH_NLOHMANN_INCLUDE}"
        "${PJSON_BENCH_RAPIDJSON_INCLUDE}"
    )
    target_link_libraries(${TARGET_NAME} PRIVATE simdjson::simdjson)
    # simdjson's public DOM API uses std::string_view. Comparison mode therefore
    # requires C++17, while the pjson library and dependency-free benchmark
    # remain C++11.
    target_compile_features(${TARGET_NAME} PRIVATE cxx_std_17)
    target_compile_definitions(${TARGET_NAME} PRIVATE PJSON_BENCH_COMPARE=1)
endif()
