# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.21)
project(pjson_install_consumer LANGUAGES CXX)

# ---- Installed dependency discovery ------------------------------------

# Exercise either supported consumer surface without changing the executable:
# the exported CMake package is the default, while packaging tests opt into the
# generated pkg-config metadata.
option(PJSON_CONSUMER_USE_PKGCONFIG "Consume pjson through pkg-config" OFF)

if(PJSON_CONSUMER_USE_PKGCONFIG)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(pjson REQUIRED IMPORTED_TARGET pjson>=1.0)
    set(PJSON_CONSUMER_TARGET PkgConfig::pjson)
else()
    find_package(pjson 1.0 CONFIG REQUIRED)
    set(PJSON_CONSUMER_TARGET pjson::pjson)
endif()

# ---- Consumer executable ------------------------------------------------

add_executable(pjson_install_consumer main.cpp)
target_link_libraries(pjson_install_consumer PRIVATE ${PJSON_CONSUMER_TARGET})
set_target_properties(pjson_install_consumer PROPERTIES
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# Register a runtime check in addition to proving that discovery and linking
# work during configuration and compilation.
enable_testing()
add_test(NAME pjson.install_consumer COMMAND pjson_install_consumer)
