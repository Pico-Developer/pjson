# SPDX-License-Identifier: Apache-2.0

find_package(Doxygen REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# ---- Documentation inputs ----------------------------------------------

# Read the public version macro so generated pages always match the library.
set(PJSON_PUBLIC_HEADER "${PROJECT_SOURCE_DIR}/pjsonlib/include/pjson.h")
set(PJSON_PARSER_PUBLIC_HEADER "${PROJECT_SOURCE_DIR}/pjsonlib/include/pjson_parser.h")
set(PJSON_SCHEMA_PUBLIC_HEADER "${PROJECT_SOURCE_DIR}/pjsonlib/include/pjson_schema.h")
file(STRINGS "${PJSON_PUBLIC_HEADER}" PJSON_VERSION_DEFINE
     REGEX "^#define PJSON_VERSION \"[^\"]+\"$")
string(REGEX REPLACE "^#define PJSON_VERSION \"([^\"]+)\"$" "\\1"
       PJSON_DOCS_VERSION "${PJSON_VERSION_DEFINE}")

set(PJSON_DOCS_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/reference")
set(PJSON_DOXYGEN_FILTER "${CMAKE_CURRENT_SOURCE_DIR}/scripts/doxygen-filter.py")
set(PJSON_DOCS_VALIDATOR "${CMAKE_CURRENT_SOURCE_DIR}/scripts/validate-reference.py")
set(PJSON_DOCS_STYLESHEET "${CMAKE_CURRENT_SOURCE_DIR}/reference/pjson.css")
set(PJSON_DOCS_MAINPAGE "${CMAKE_CURRENT_SOURCE_DIR}/reference/mainpage.md")
set(PJSON_DOCS_API_NOTES "${CMAKE_CURRENT_SOURCE_DIR}/reference/pjson-api.dox")
set(PJSON_DOCS_INDEX "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
set(PJSON_DOCS_ALLOCATORS "${CMAKE_CURRENT_SOURCE_DIR}/12-custom-allocators.md")
set(PJSON_DOCS_MIGRATIONS
    "${CMAKE_CURRENT_SOURCE_DIR}/migration-from-nlohmann-json.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/migration-from-rapidjson.md")

configure_file(Doxyfile.in "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile" @ONLY)

# ---- Shared generation pipeline ----------------------------------------

# Both targets clean the output before running Doxygen, then validate the
# generated XML and HTML against the expected public API surface.
set(PJSON_DOCS_COMMANDS
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${PJSON_DOCS_OUTPUT_DIR}"
    COMMAND "${DOXYGEN_EXECUTABLE}" "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
    COMMAND "${Python3_EXECUTABLE}" "${PJSON_DOCS_VALIDATOR}"
            --xml "${PJSON_DOCS_OUTPUT_DIR}/xml" --html "${PJSON_DOCS_OUTPUT_DIR}/html")
set(PJSON_DOCS_DEPENDS
        "${PJSON_PUBLIC_HEADER}"
        "${PJSON_PARSER_PUBLIC_HEADER}"
        "${PJSON_SCHEMA_PUBLIC_HEADER}"
        "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
        "${PJSON_DOXYGEN_FILTER}"
        "${PJSON_DOCS_VALIDATOR}"
        "${PJSON_DOCS_STYLESHEET}"
        "${PJSON_DOCS_MAINPAGE}"
        "${PJSON_DOCS_API_NOTES}"
        "${PJSON_DOCS_INDEX}"
        "${PJSON_DOCS_ALLOCATORS}"
        ${PJSON_DOCS_MIGRATIONS})

# ---- Incremental and always-run targets --------------------------------

# The regular target may use its timestamp stamp for quick incremental builds.
# The check target always runs, making it appropriate for CI and pre-submit use.
add_custom_command(
    OUTPUT "${PJSON_DOCS_OUTPUT_DIR}/.validated"
    ${PJSON_DOCS_COMMANDS}
    COMMAND "${CMAKE_COMMAND}" -E touch "${PJSON_DOCS_OUTPUT_DIR}/.validated"
    DEPENDS ${PJSON_DOCS_DEPENDS}
    COMMENT "Generating and validating the pjson API reference"
    VERBATIM)
add_custom_target(pjson-docs ALL DEPENDS "${PJSON_DOCS_OUTPUT_DIR}/.validated")

add_custom_target(
    pjson-docs-check
    ${PJSON_DOCS_COMMANDS}
    DEPENDS ${PJSON_DOCS_DEPENDS}
    COMMENT "Rebuilding and validating the pjson API reference"
    VERBATIM)
