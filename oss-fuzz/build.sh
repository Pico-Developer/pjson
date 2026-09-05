#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 ByteDance Ltd. and/or its affiliates
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

# ---- OSS-Fuzz environment contract --------------------------------------

# Fail early with a useful message when the script is run outside the
# environment that supplies source, workspace, output, and engine settings.
: "${SRC:?OSS-Fuzz must provide SRC}"
: "${WORK:?OSS-Fuzz must provide WORK}"
: "${OUT:?OSS-Fuzz must provide OUT}"
: "${LIB_FUZZING_ENGINE:?OSS-Fuzz must provide LIB_FUZZING_ENGINE}"

# ---- Source and build locations -----------------------------------------

# Production builders clone into $SRC/pjson. The fallback also lets developers
# invoke this copied script from a repository checkout for integration smoke.
PJSON_SOURCE_DIR="${SRC}/pjson"
if [ ! -f "${PJSON_SOURCE_DIR}/CMakeLists.txt" ]; then
    PJSON_SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fi
PJSON_FUZZ_BUILD_DIR="${WORK}/pjson-fuzz-build"

# ---- Configure and compile ----------------------------------------------

# CXX and LIB_FUZZING_ENGINE are selected by OSS-Fuzz for the active sanitizer
# and engine combination; the project must not substitute host defaults.
cmake -S "${PJSON_SOURCE_DIR}" -B "${PJSON_FUZZ_BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_COMPILER="${CXX}" \
    -DPJSON_BUILD_TESTS=OFF \
    -DPJSON_BUILD_EXAMPLES=OFF \
    -DPJSON_BUILD_BENCHMARKS=OFF \
    -DPJSON_BUILD_FUZZERS=ON \
    -DPJSON_FUZZING_ENGINE="${LIB_FUZZING_ENGINE}"
cmake --build "${PJSON_FUZZ_BUILD_DIR}" --parallel --target \
    pjson_fuzz_parse pjson_fuzz_stream pjson_fuzz_serialize pjson_fuzz_schema \
    pjson_fuzz_pointer pjson_fuzz_patch pjson_fuzz_merge_patch

# ---- Runtime bundle -----------------------------------------------------

# Each executable receives matching runtime options and the shared JSON token
# dictionary under the basename convention understood by OSS-Fuzz.
for target in pjson_fuzz_parse pjson_fuzz_stream pjson_fuzz_serialize pjson_fuzz_schema \
              pjson_fuzz_pointer pjson_fuzz_patch pjson_fuzz_merge_patch; do
    cp "${PJSON_FUZZ_BUILD_DIR}/fuzz/${target}" "${OUT}/${target}"
    cp "${PJSON_SOURCE_DIR}/oss-fuzz/${target}.options" "${OUT}/${target}.options"
    cp "${PJSON_SOURCE_DIR}/fuzz/json.dict" "${OUT}/${target}.dict"
done

# Package each checked-in seed directory at the archive root, as required by
# OSS-Fuzz's <target>_seed_corpus.zip discovery convention. The subshell keeps
# the loop's working directory stable between harnesses.
for corpus in parse stream serialize schema pointer patch merge_patch; do
    (
        cd "${PJSON_SOURCE_DIR}/fuzz/corpus/${corpus}"
        zip -q -r "${OUT}/pjson_fuzz_${corpus}_seed_corpus.zip" .
    )
done
