cmake_minimum_required(VERSION 3.21)

set(TARGET_NAME pjsonbench)

project(${TARGET_NAME})

set(SRC_DIR "src")

set(BENCH_SRC_FILES
    ${SRC_DIR}/benchmark_main.cpp
)

option(PJSON_BENCH_COMPARE "Enable optional third-party JSON benchmark comparisons" OFF)
set(PJSON_BENCH_DEPS_DIR "${CMAKE_SOURCE_DIR}/.benchmark-deps"
    CACHE PATH "Directory containing pinned benchmark comparison dependencies")

if(MSVC)
    set(PJSON_BENCH_WARN_FLAGS /W4)
else()
    set(PJSON_BENCH_WARN_FLAGS -Wall -Wextra)
endif()

add_executable(${TARGET_NAME} ${BENCH_SRC_FILES})
target_link_libraries(${TARGET_NAME} PRIVATE pjson::pjson)
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
