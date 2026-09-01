# SPDX-License-Identifier: Apache-2.0

set(TARGET_NAME pjson)

# ---- Library sources and compiler policy --------------------------------

set (SRC_DIR "src")
set (INCLUDE_DIR "include")

set (SRC_FILES ${SRC_FILES}
${SRC_DIR}/pjson.cpp
${SRC_DIR}/pjson_schema.cpp
)

# Warning flags differ by compiler: GCC/Clang use -Wall -Wextra, MSVC uses /W4.
if (MSVC)
    set (PJSON_WARN_FLAGS /W4)
else()
    set (PJSON_WARN_FLAGS -Wall -Wextra)
endif()

# ---- Library target -----------------------------------------------------

add_library(${TARGET_NAME} ${SRC_FILES})
add_library(pjson::pjson ALIAS ${TARGET_NAME})

target_compile_features(${TARGET_NAME} PUBLIC cxx_std_11)
target_compile_options(${TARGET_NAME} PRIVATE ${PJSON_WARN_FLAGS})
set_target_properties(${TARGET_NAME} PROPERTIES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    CXX_EXTENSIONS OFF
    EXPORT_NAME pjson
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)

# Consumers get the include dir whether they build in-tree or install.
target_include_directories(${TARGET_NAME} PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/${INCLUDE_DIR}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

# ---- Install and export rules ------------------------------------------
# Allow downstreams to `find_package(pjson)` after installing, or to
# `add_subdirectory()` this repo and link `pjson::pjson` directly.
include(GNUInstallDirs)

install(TARGETS ${TARGET_NAME}
    EXPORT pjsonTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

install(FILES
    ${INCLUDE_DIR}/pjson.h
    ${INCLUDE_DIR}/pjson_schema.h
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

set(PJSON_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/pjson"
    CACHE STRING "Directory for pjson CMake package files")
mark_as_advanced(PJSON_INSTALL_CMAKEDIR)

install(EXPORT pjsonTargets
    FILE pjsonTargets.cmake
    NAMESPACE pjson::
    DESTINATION "${PJSON_INSTALL_CMAKEDIR}"
)

configure_package_config_file(
    "${CMAKE_CURRENT_LIST_DIR}/../cmake/pjsonConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/pjsonConfig.cmake"
    INSTALL_DESTINATION "${PJSON_INSTALL_CMAKEDIR}"
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/pjsonConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/pjsonConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/pjsonConfigVersion.cmake"
    DESTINATION "${PJSON_INSTALL_CMAKEDIR}"
)

# Derive pkg-config's prefix from pcfiledir instead of baking in the configure
# prefix. This keeps the metadata valid after an installed tree is relocated,
# including when CMAKE_INSTALL_LIBDIR is changed to lib64 or a nested path.
set(PJSON_INSTALL_PKGCONFIGDIR "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
# Keep this calculation lexical and relative.  Synthesizing a root with a
# leading slash is not a native absolute path on Windows, so file(RELATIVE_PATH)
# rejects it during configuration.
set(PJSON_PC_PREFIX_FROM_PCFILEDIR ".")
cmake_path(RELATIVE_PATH PJSON_PC_PREFIX_FROM_PCFILEDIR
    BASE_DIRECTORY "${PJSON_INSTALL_PKGCONFIGDIR}")
cmake_path(NORMAL_PATH PJSON_PC_PREFIX_FROM_PCFILEDIR)
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/../cmake/pjson.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/pjson.pc"
    @ONLY
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/pjson.pc"
    DESTINATION "${PJSON_INSTALL_PKGCONFIGDIR}"
)

install(FILES "${CMAKE_CURRENT_LIST_DIR}/../LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/pjson"
)
