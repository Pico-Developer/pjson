# SPDX-License-Identifier: Apache-2.0
# ---- Project and common toolchain policy --------------------------------

cmake_minimum_required (VERSION 3.21)

project(pjson VERSION 3.0.0 DESCRIPTION "Praveen's JSON library for C++" LANGUAGES CXX)

# Keep package/runtime version authorities synchronized at configure time. The
# release process updates them together; a mismatch is a hard configuration
# error rather than a silently inconsistent distribution.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/pjsonlib/include/pjson.h"
     PJSON_HEADER_VERSION_LINE REGEX "^#define PJSON_VERSION \"[^\"]+\"$")
string(REGEX REPLACE "^#define PJSON_VERSION \"([^\"]+)\"$" "\\1"
       PJSON_HEADER_VERSION "${PJSON_HEADER_VERSION_LINE}")
set(PJSON_VERSION_SOURCES PJSON_HEADER_VERSION)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/pjsonlib/include/pjson.h"
     PJSON_ABI_VERSION_LINE REGEX "^#define PJSON_ABI_VERSION [0-9]+$")
string(REGEX REPLACE "^#define PJSON_ABI_VERSION ([0-9]+)$" "\\1"
       PJSON_ABI_VERSION "${PJSON_ABI_VERSION_LINE}")
if(NOT "${PJSON_ABI_VERSION}" STREQUAL "${PROJECT_VERSION_MAJOR}")
    message(FATAL_ERROR
        "ABI version mismatch: PJSON_ABI_VERSION=${PJSON_ABI_VERSION}, "
        "project major=${PROJECT_VERSION_MAJOR}")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/conanfile.py")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/conanfile.py" PJSON_CONAN_RECIPE)
    string(REGEX MATCH "version = \"([^\"]+)\"" PJSON_CONAN_VERSION_MATCH
           "${PJSON_CONAN_RECIPE}")
    set(PJSON_CONAN_VERSION "${CMAKE_MATCH_1}")
    list(APPEND PJSON_VERSION_SOURCES PJSON_CONAN_VERSION)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/packaging/vcpkg/ports/pjson/vcpkg.json")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/packaging/vcpkg/ports/pjson/vcpkg.json"
         PJSON_VCPKG_MANIFEST)
    string(REGEX MATCH "\"version-semver\"[ \t]*:[ \t]*\"([^\"]+)\""
           PJSON_VCPKG_VERSION_MATCH "${PJSON_VCPKG_MANIFEST}")
    set(PJSON_VCPKG_VERSION "${CMAKE_MATCH_1}")
    list(APPEND PJSON_VERSION_SOURCES PJSON_VCPKG_VERSION)
endif()
foreach(PJSON_VERSION_SOURCE IN LISTS PJSON_VERSION_SOURCES)
    if(NOT "${${PJSON_VERSION_SOURCE}}" STREQUAL "${PROJECT_VERSION}")
        message(FATAL_ERROR
            "Version mismatch: ${PJSON_VERSION_SOURCE}=${${PJSON_VERSION_SOURCE}}, "
            "CMake project=${PROJECT_VERSION}")
    endif()
endforeach()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Repository-only targets are useful when pjson is the configured project, but
# must stay out of a parent project's default build when consumed with
# add_subdirectory(). These project-specific switches deliberately do not write
# to the parent-owned BUILD_TESTING cache entry.
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(PJSON_DEVELOPER_DEFAULT ON)
    # Preserve the conventional top-level BUILD_TESTING switch without
    # introducing it into a parent project's cache during add_subdirectory().
    include(CTest)
else()
    set(PJSON_DEVELOPER_DEFAULT OFF)
endif()
if(DEFINED BUILD_TESTING AND NOT BUILD_TESTING)
    set(PJSON_TEST_DEFAULT OFF)
else()
    set(PJSON_TEST_DEFAULT ${PJSON_DEVELOPER_DEFAULT})
endif()
option(PJSON_BUILD_TESTS "Build the pjson test suite" ${PJSON_TEST_DEFAULT})
option(PJSON_BUILD_EXAMPLES "Build the pjson examples" ${PJSON_DEVELOPER_DEFAULT})
option(PJSON_BUILD_BENCHMARKS "Build the pjson benchmarks" ${PJSON_DEVELOPER_DEFAULT})
unset(PJSON_DEVELOPER_DEFAULT)
unset(PJSON_TEST_DEFAULT)
if(PJSON_BUILD_TESTS AND (NOT DEFINED BUILD_TESTING OR BUILD_TESTING))
    enable_testing()
endif()

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED On)
set(CMAKE_CXX_EXTENSIONS Off)

# ---- Optional instrumentation ------------------------------------------

# Optional AddressSanitizer + UndefinedBehaviorSanitizer build (see build.sh
# --asan). Applied to every target so the library and tests are instrumented.
# Only meaningful for GCC/Clang; ignored on MSVC.
option(PJSON_SANITIZE "Build with Address/UB sanitizers" OFF)
if(PJSON_SANITIZE AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

option(PJSON_BUILD_FUZZERS "Build coverage-guided libFuzzer targets" OFF)
set(PJSON_FUZZING_ENGINE "" CACHE STRING
    "External fuzzing engine linker input (for example OSS-Fuzz LIB_FUZZING_ENGINE)")

# ---- Project targets ----------------------------------------------------

# Build the library in every configuration. Tests participate in CTest only
# when both the project switch and the standard BUILD_TESTING switch are on.
add_subdirectory ("pjsonlib")
if(PJSON_BUILD_FUZZERS)
    add_subdirectory("fuzz")
endif()
if(PJSON_BUILD_TESTS AND (NOT DEFINED BUILD_TESTING OR BUILD_TESTING))
    add_subdirectory ("pjsontest")
endif()
if(PJSON_BUILD_EXAMPLES)
    add_subdirectory ("examples")
endif()
if(PJSON_BUILD_BENCHMARKS)
    add_subdirectory ("bench")
endif()

option(PJSON_BUILD_DOCS "Build and validate the browsable API reference" OFF)
if(PJSON_BUILD_DOCS)
    add_subdirectory("docs")
endif()
