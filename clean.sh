#!/usr/bin/env bash
#
# Copyright 2025 ByteDance Ltd. and/or its affiliates. All rights reserved.
# Licensed under the Apache License, Version 2.0 (the "License").
#
# Removes everything generated/downloaded by build.sh and by ad-hoc
# CMake/compiler runs, leaving only source files under version control. This
# includes optional JSONTestSuite and benchmark dependency checkouts.
#
set -euo pipefail

# Anchor every destructive path to this checkout, regardless of where the
# caller invokes the script. No caller-supplied cleanup path is accepted.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

# ---- Repository-owned generated trees ----------------------------------

echo ">> Removing build.sh output (out/)"
rm -rf "${SCRIPT_DIR}/out"

echo ">> Removing downloaded test corpora (.test-corpora/)"
rm -rf "${SCRIPT_DIR}/.test-corpora"

echo ">> Removing downloaded benchmark dependencies (.benchmark-deps/)"
rm -rf "${SCRIPT_DIR}/.benchmark-deps"

echo ">> Removing local fuzz runtime state (.fuzz-corpus/ and .fuzz-artifacts/)"
rm -rf "${SCRIPT_DIR}/.fuzz-corpus" "${SCRIPT_DIR}/.fuzz-artifacts"

echo ">> Removing Python bytecode caches"
find "${SCRIPT_DIR}" -type d -name '__pycache__' -not -path '*/.git/*' \
    -prune -exec rm -rf {} + 2>/dev/null || true
find "${SCRIPT_DIR}" -type f \( -name '*.pyc' -o -name '*.pyo' \) \
    -not -path '*/.git/*' -delete 2>/dev/null || true

echo ">> Removing stray CMake build trees"
rm -rf "${SCRIPT_DIR}/build" "${SCRIPT_DIR}/cmake-build-"* 2>/dev/null || true

echo ">> Removing Conan test-package build output"
rm -rf "${SCRIPT_DIR}/test_package/build"
rm -f "${SCRIPT_DIR}/test_package/CMakeUserPresets.json"

# ---- Stray generated files ---------------------------------------------

# These scans stay below the resolved repository root and explicitly prune
# .git so cleanup cannot damage Git's object database or metadata.
echo ">> Removing in-source CMake artifacts (from accidental in-tree configures)"
find "${SCRIPT_DIR}" \
    \( -name 'CMakeCache.txt' \
       -o -name 'CMakeFiles' -type d \
       -o -name 'cmake_install.cmake' \
       -o -name 'CTestTestfile.cmake' \
       -o -name 'compile_commands.json' \
       -o -name 'Makefile' \) \
    -not -path '*/.git/*' -prune -exec rm -rf {} + 2>/dev/null || true

echo ">> Removing compiled objects, libraries, and OS cruft"
find "${SCRIPT_DIR}" \
    \( -name '*.o' -o -name '*.obj' -o -name '*.a' -o -name '*.lib' \
       -o -name '*.so' -o -name '*.dylib' -o -name '*.dll' \
       -o -name '*.exe' -o -name '*.out' -o -name '*.gch' -o -name '*.pch' \
       -o -name '.DS_Store' \) \
    -not -path '*/.git/*' -type f -delete 2>/dev/null || true

echo ">> Clean."
