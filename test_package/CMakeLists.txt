# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.21)
project(pjson_package_test LANGUAGES CXX)

# ---- Packaged dependency discovery -------------------------------------

# Conan's CMakeDeps generator supplies this config package from the exact
# package reference under test.
find_package(pjson CONFIG REQUIRED)

# ---- External consumer --------------------------------------------------

# Build as a C++11 client linked only through the package's exported target.
add_executable(pjson_package_test src/pjson_package_test.cpp)
target_compile_features(pjson_package_test PRIVATE cxx_std_11)
target_link_libraries(pjson_package_test PRIVATE pjson::pjson)
