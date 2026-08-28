# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.21)

# ---- Required inputs ---------------------------------------------------

# This script removes and recreates named children of PJSON_WORK_DIR. Require
# an explicit path and reject roots or any source ancestor before mutation.
if(NOT DEFINED PJSON_SOURCE_DIR OR PJSON_SOURCE_DIR STREQUAL "")
    message(FATAL_ERROR "PJSON_SOURCE_DIR must name the pjson source tree")
endif()
if(NOT DEFINED PJSON_WORK_DIR OR PJSON_WORK_DIR STREQUAL "")
    message(FATAL_ERROR "PJSON_WORK_DIR must name a disposable smoke-test directory")
endif()

get_filename_component(PJSON_SOURCE_DIR "${PJSON_SOURCE_DIR}" ABSOLUTE)
get_filename_component(PJSON_WORK_DIR "${PJSON_WORK_DIR}" ABSOLUTE)
file(TO_CMAKE_PATH "${PJSON_SOURCE_DIR}" PJSON_SOURCE_DIR)
file(TO_CMAKE_PATH "${PJSON_WORK_DIR}" PJSON_WORK_DIR)
cmake_path(NORMAL_PATH PJSON_SOURCE_DIR)
cmake_path(NORMAL_PATH PJSON_WORK_DIR)

# Resolve symlinks even when the final work directory does not exist: resolve
# its nearest existing ancestor, then append the missing path components.
function(pjson_resolve_path path output_variable)
    set(existing_path "${path}")
    set(missing_components)
    while(NOT EXISTS "${existing_path}")
        cmake_path(GET existing_path FILENAME component)
        if(component STREQUAL "")
            message(FATAL_ERROR "Unable to resolve path safely: ${path}")
        endif()
        list(PREPEND missing_components "${component}")
        cmake_path(GET existing_path PARENT_PATH existing_path)
    endwhile()
    file(REAL_PATH "${existing_path}" resolved_path)
    foreach(component IN LISTS missing_components)
        cmake_path(APPEND resolved_path "${component}")
    endforeach()
    cmake_path(NORMAL_PATH resolved_path)
    set(${output_variable} "${resolved_path}" PARENT_SCOPE)
endfunction()

pjson_resolve_path("${PJSON_SOURCE_DIR}" PJSON_SOURCE_DIR)
pjson_resolve_path("${PJSON_WORK_DIR}" PJSON_WORK_DIR)
if(NOT EXISTS "${PJSON_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "PJSON_SOURCE_DIR is not a pjson source tree")
endif()
cmake_path(GET PJSON_WORK_DIR ROOT_PATH PJSON_WORK_ROOT)
if(PJSON_WORK_DIR STREQUAL PJSON_WORK_ROOT)
    message(FATAL_ERROR "PJSON_WORK_DIR must not be a filesystem root")
endif()
cmake_path(IS_PREFIX PJSON_WORK_DIR "${PJSON_SOURCE_DIR}" NORMALIZE
    PJSON_WORK_IS_SOURCE_ANCESTOR)
if(PJSON_WORK_IS_SOURCE_ANCESTOR)
    message(FATAL_ERROR
        "PJSON_WORK_DIR must not be the source tree or one of its ancestors")
endif()

set(producer_build "${PJSON_WORK_DIR}/producer-build")
set(stage_prefix "${PJSON_WORK_DIR}/stage")
set(relocated_prefix "${PJSON_WORK_DIR}/relocated")
set(consumer_build "${PJSON_WORK_DIR}/consumer-build")
set(pkgconfig_consumer_build "${PJSON_WORK_DIR}/pkgconfig-consumer-build")
set(consumer_source "${PJSON_SOURCE_DIR}/tests/install-consumer")

# Runs an external configure/build/install/test command and turns any non-zero
# result into an immediate CMake script failure with a phase-specific message.
function(run_checked description)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE result
        COMMAND_ECHO STDOUT
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${description} failed with exit code ${result}")
    endif()
endfunction()

# ---- Fresh producer installation ---------------------------------------

file(REMOVE_RECURSE
    "${producer_build}"
    "${stage_prefix}"
    "${relocated_prefix}"
    "${consumer_build}"
    "${pkgconfig_consumer_build}"
)
file(MAKE_DIRECTORY "${PJSON_WORK_DIR}")

set(producer_configure
    "${CMAKE_COMMAND}"
    -S "${PJSON_SOURCE_DIR}"
    -B "${producer_build}"
    -DPJSON_BUILD_TESTS=OFF
    -DPJSON_BUILD_EXAMPLES=OFF
    -DPJSON_BUILD_BENCHMARKS=OFF
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX=${stage_prefix}
)
if(DEFINED PJSON_BUILD_SHARED_LIBS)
    list(APPEND producer_configure -DBUILD_SHARED_LIBS=${PJSON_BUILD_SHARED_LIBS})
endif()
if(DEFINED PJSON_INSTALL_LIBDIR AND NOT PJSON_INSTALL_LIBDIR STREQUAL "")
    list(APPEND producer_configure -DCMAKE_INSTALL_LIBDIR=${PJSON_INSTALL_LIBDIR})
endif()
if(DEFINED PJSON_GENERATOR AND NOT PJSON_GENERATOR STREQUAL "")
    list(APPEND producer_configure -G "${PJSON_GENERATOR}")
endif()

run_checked("pjson package configure" ${producer_configure})
run_checked("pjson package build"
    "${CMAKE_COMMAND}" --build "${producer_build}" --config Release --parallel)
run_checked("pjson package install"
    "${CMAKE_COMMAND}" --install "${producer_build}" --config Release)

# ---- Relocation and artifact validation --------------------------------

# A consumer that succeeds only after the original prefix has disappeared is
# stronger evidence that both the CMake package and pkg-config file relocate.
file(RENAME "${stage_prefix}" "${relocated_prefix}")

file(GLOB_RECURSE config_files LIST_DIRECTORIES FALSE
    "${relocated_prefix}/*/cmake/pjson/pjsonConfig.cmake")
file(GLOB_RECURSE version_files LIST_DIRECTORIES FALSE
    "${relocated_prefix}/*/cmake/pjson/pjsonConfigVersion.cmake")
file(GLOB_RECURSE target_files LIST_DIRECTORIES FALSE
    "${relocated_prefix}/*/cmake/pjson/pjsonTargets.cmake")
file(GLOB_RECURSE package_cmake_files LIST_DIRECTORIES FALSE
    "${relocated_prefix}/*/cmake/pjson/*.cmake")
file(GLOB_RECURSE pc_files LIST_DIRECTORIES FALSE
    "${relocated_prefix}/*/pkgconfig/pjson.pc")
file(GLOB_RECURSE library_files LIST_DIRECTORIES FALSE
    "${relocated_prefix}/libpjson.*"
    "${relocated_prefix}/libpjson*.dylib"
    "${relocated_prefix}/pjson.lib"
    "${relocated_prefix}/pjson.dll")
foreach(required_files IN ITEMS config_files version_files target_files pc_files library_files)
    list(LENGTH ${required_files} required_file_count)
    if(required_file_count EQUAL 0)
        message(FATAL_ERROR "Installed package is missing ${required_files}")
    endif()
endforeach()
if(NOT EXISTS "${relocated_prefix}/include/pjson.h")
    message(FATAL_ERROR "Installed package is missing include/pjson.h")
endif()
foreach(metadata_file IN LISTS package_cmake_files pc_files)
    file(READ "${metadata_file}" metadata_contents)
    string(FIND "${metadata_contents}" "${stage_prefix}" old_prefix_position)
    string(FIND "${metadata_contents}" "${PJSON_SOURCE_DIR}" source_position)
    if(NOT old_prefix_position EQUAL -1 OR NOT source_position EQUAL -1)
        message(FATAL_ERROR "Installed metadata is not relocatable: ${metadata_file}")
    endif()
endforeach()

# ---- CMake-package consumer --------------------------------------------

list(GET config_files 0 config_file)
get_filename_component(config_dir "${config_file}" DIRECTORY)

set(consumer_configure
    "${CMAKE_COMMAND}"
    -S "${consumer_source}"
    -B "${consumer_build}"
    -DCMAKE_BUILD_TYPE=Release
    -Dpjson_DIR=${config_dir}
)
if(DEFINED PJSON_GENERATOR AND NOT PJSON_GENERATOR STREQUAL "")
    list(APPEND consumer_configure -G "${PJSON_GENERATOR}")
endif()
run_checked("installed-package consumer configure" ${consumer_configure})
run_checked("installed-package consumer build"
    "${CMAKE_COMMAND}" --build "${consumer_build}" --config Release --parallel)
run_checked("installed-package consumer run"
    "${CMAKE_CTEST_COMMAND}" --test-dir "${consumer_build}" -C Release --output-on-failure)

# ---- pkg-config consumer ------------------------------------------------

# Constrain both pkg-config search variables to the relocated tree so a system
# installation cannot make the smoke test pass accidentally.
if(NOT DEFINED PJSON_PKG_CONFIG_EXECUTABLE OR
   PJSON_PKG_CONFIG_EXECUTABLE STREQUAL "")
    find_program(PJSON_PKG_CONFIG_EXECUTABLE NAMES pkg-config pkgconf)
endif()
if(PJSON_PKG_CONFIG_EXECUTABLE)
    list(GET pc_files 0 pc_file)
    get_filename_component(pc_dir "${pc_file}" DIRECTORY)
    run_checked("relocated pkg-config validation"
        "${CMAKE_COMMAND}" -E env
        "PKG_CONFIG_PATH=${pc_dir}"
        "PKG_CONFIG_LIBDIR=${pc_dir}"
        "${PJSON_PKG_CONFIG_EXECUTABLE}" --validate pjson)
    run_checked("relocated pkg-config version check"
        "${CMAKE_COMMAND}" -E env
        "PKG_CONFIG_PATH=${pc_dir}"
        "PKG_CONFIG_LIBDIR=${pc_dir}"
        "${PJSON_PKG_CONFIG_EXECUTABLE}" --exact-version=1.0.0 pjson)

    set(pkgconfig_consumer_configure
        "${CMAKE_COMMAND}" -E env
        "PKG_CONFIG_PATH=${pc_dir}"
        "PKG_CONFIG_LIBDIR=${pc_dir}"
        "${CMAKE_COMMAND}"
        -S "${consumer_source}"
        -B "${pkgconfig_consumer_build}"
        -DCMAKE_BUILD_TYPE=Release
        -DPJSON_CONSUMER_USE_PKGCONFIG=ON
    )
    if(DEFINED PJSON_GENERATOR AND NOT PJSON_GENERATOR STREQUAL "")
        list(APPEND pkgconfig_consumer_configure -G "${PJSON_GENERATOR}")
    endif()
    run_checked("pkg-config consumer configure" ${pkgconfig_consumer_configure})
    run_checked("pkg-config consumer build"
        "${CMAKE_COMMAND}" --build "${pkgconfig_consumer_build}" --config Release --parallel)
    run_checked("pkg-config consumer run"
        "${CMAKE_CTEST_COMMAND}" --test-dir "${pkgconfig_consumer_build}"
        -C Release --output-on-failure)
elseif(PJSON_REQUIRE_PKG_CONFIG)
    message(FATAL_ERROR "pkg-config or pkgconf is required for this smoke test")
else()
    message(STATUS "pkg-config was not found; skipping that consumer path")
endif()

message(STATUS "Relocatable install-consumer smoke test passed: ${relocated_prefix}")
