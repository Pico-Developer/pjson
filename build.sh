#!/usr/bin/env bash
#
# Copyright 2025 ByteDance Ltd. and/or its affiliates. All rights reserved.
# Licensed under the Apache License, Version 2.0 (the "License").
#
# Cross-platform build driver for pjson (Linux, macOS, Windows via Git Bash /
# MSYS2). Builds the library, the unit tests, and the examples in BOTH Release
# and Debug, collecting everything under out/:
#
#   out/
#     release/{lib,bin,bin/examples}   Release library, tests, examples
#     debug/{lib,bin,bin/examples}     Debug   library, tests, examples
#     include/pjson.h                  public header
#     build-release/ build-debug/      CMake build trees
#
# Usage:
#   ./build.sh                Do everything (same as --all)
#   ./build.sh --all          Clean, check formatting, build Release + a
#                             sanitized Debug, fetch conformance corpora if missing,
#                             run tests/comparison benchmarks, replay the fuzz
#                             seed corpora when supported, validate docs and
#                             packages/licensing, and run clang-tidy
#   ./build.sh --test         Also run the test suite
#   ./build.sh --bench        Build, then run the Release benchmark suite
#   ./build.sh --bench-compare
#                             Build/run the optional third-party comparison
#                             benchmark suite (fetches pinned deps if needed)
#   ./build.sh --fuzz         Build libFuzzer targets and replay their seed
#                             corpora with a deterministic run budget
#   ./build.sh --docs         Build and validate the Doxygen API reference
#   ./build.sh --package      Validate static/shared installs and pkg-config
#   ./build.sh --license      Validate SPDX/REUSE licensing metadata
#   ./build.sh --clean        Remove out/ before building
#   ./build.sh --asan         Single Debug build with Address/UB sanitizers
#   ./build.sh --release-only Build only the Release configuration
#   ./build.sh --debug-only   Build only the Debug configuration
#   ./build.sh --format       Reformat all sources with clang-format (in place)
#   ./build.sh --check        Verify formatting without changing files
#   ./build.sh --tidy         Run clang-tidy static analysis (fails on findings)
#   ./build.sh --bench-input PATH
#                             Add an extra JSON file for benchmark coverage
#   ./build.sh --bench-json PATH
#                             Write machine-readable benchmark results
#   ./build.sh --auto         Never prompt; auto-install/download dependencies
#
# Flags combine freely. Missing tools and optional JSON/JSON-Schema conformance
# corpora are detected and, with your confirmation, installed/downloaded; pass
# --auto to do so without prompting (useful for CI).
# Contributors (and CI) can simply run:
#
#   ./build.sh            # or, equivalently: ./build.sh --all
#
# which is a superset of the older "--clean --asan --test --check --tidy".
#
set -euo pipefail

# Resolve the repository root (directory containing this script) so the build
# works regardless of the caller's current directory.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

OUT_DIR="${SCRIPT_DIR}/out"

# ---------------------------------------------------------------------------
# Command-line selection.
# ---------------------------------------------------------------------------

# Prints the supported workflow flags and their dependency/download behavior.
usage() {
    cat <<'USAGE'
Usage: ./build.sh [flags]
  --all           Do everything: clean, check formatting, build Release + a
                  sanitized Debug, fetch JSON/JSON-Schema test corpora and
                  benchmark dependencies if missing, run tests/comparison
                  benchmarks, bounded fuzz corpus smoke when supported, docs,
                  package consumers, SPDX/REUSE metadata, and clang-tidy
                  (also the default when no flags are given)
  --test          Build, then run the test suite
  --bench         Build, then run the Release benchmark suite
  --bench-compare Build, then run the Release benchmark comparison suite
  --bench-input   Add an extra JSON file to the benchmark corpus (repeatable)
  --bench-json    Write a versioned JSON benchmark report to PATH
  --fuzz          Build libFuzzer targets and run bounded corpus smoke tests
  --docs          Build and validate the generated API reference
  --package       Run static/shared install and pkg-config consumer smoke tests
  --license       Validate SPDX/REUSE licensing metadata
  --clean         Remove out/ before building
  --asan          Single Debug build with Address/UB sanitizers
  --release-only  Build only the Release configuration
  --debug-only    Build only the Debug configuration
  --format        Reformat all sources with clang-format (in place)
  --check         Verify formatting without changing files (fails if dirty)
  --tidy          Run clang-tidy static analysis (fails on findings)
  --auto          Never prompt; auto-install/download missing dependencies
  --help          Show this help

With no flags, ./build.sh behaves like ./build.sh --all. Otherwise both Release
and Debug are built (library + tests + examples + benchmarks) into out/, and
only the steps you ask for run. Flags combine freely, e.g.:
  ./build.sh --clean --asan --test --bench --fuzz --check --tidy

Missing tools (cmake and, when selected, pkg-config, clang-format, clang-tidy,
Doxygen, Python, or the pinned REUSE checker) are detected and offered for
installation/download through the system package manager or into `out/`.
During --test and --all runs, missing JSONTestSuite and
JSON-Schema-Test-Suite corpora are similarly offered for download. Pass --auto
to install/download without prompting. Benchmarks always run from the Release
build; add extra JSON files with --bench-input PATH.
--bench-compare fetches pinned nlohmann/json, RapidJSON, and simdjson sources
into .benchmark-deps/. It runs when requested directly and as part of --all.
USAGE
}

DO_CLEAN=0
DO_TEST=0
DO_BENCH=0
DO_BENCH_COMPARE=0
DO_FUZZ=0
DO_DOCS=0
DO_PACKAGE=0
DO_LICENSE=0
DO_ASAN=0
DO_FORMAT=0
DO_CHECK=0
DO_TIDY=0
DO_ALL=0
AUTO=0
RELEASE_ONLY=0
DEBUG_ONLY=0
BENCH_INPUTS=()
BENCH_JSON=""

# No flags at all is a friendly shortcut for --all (do everything).
if [ "$#" -eq 0 ]; then
    DO_ALL=1
fi

while [ "$#" -gt 0 ]; do
    case "$1" in
        --all)          DO_ALL=1 ;;
        --clean)        DO_CLEAN=1 ;;
        --test)         DO_TEST=1 ;;
        --bench)        DO_BENCH=1 ;;
        --bench-compare) DO_BENCH=1; DO_BENCH_COMPARE=1 ;;
        --fuzz)         DO_FUZZ=1 ;;
        --docs)         DO_DOCS=1 ;;
        --package)      DO_PACKAGE=1 ;;
        --license)      DO_LICENSE=1 ;;
        --asan)         DO_ASAN=1 ;;
        --release-only) RELEASE_ONLY=1 ;;
        --debug-only)   DEBUG_ONLY=1 ;;
        --format)       DO_FORMAT=1 ;;
        --check)        DO_CHECK=1 ;;
        --tidy)         DO_TIDY=1 ;;
        --bench-input)
            shift
            if [ "$#" -eq 0 ]; then
                echo "Missing value for --bench-input" >&2
                usage >&2
                exit 2
            fi
            BENCH_INPUTS+=("$1")
            ;;
        --bench-input=*)
            BENCH_INPUTS+=("${1#--bench-input=}")
            ;;
        --bench-json)
            shift
            if [ "$#" -eq 0 ]; then
                echo "Missing value for --bench-json" >&2
                usage >&2
                exit 2
            fi
            BENCH_JSON="$1"
            ;;
        --bench-json=*)
            BENCH_JSON="${1#--bench-json=}"
            if [ -z "${BENCH_JSON}" ]; then
                echo "Missing value for --bench-json" >&2
                usage >&2
                exit 2
            fi
            ;;
        --auto|--yes|-y) AUTO=1 ;;
        -h|--help)      usage; exit 0 ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

# --all expands to the full contributor sweep. --auto (non-interactive tool
# install) is intentionally left independent so it can be combined with --all.
if [ "${DO_ALL}" -eq 1 ]; then
    DO_CLEAN=1
    DO_CHECK=1
    DO_ASAN=1
    DO_TEST=1
    DO_BENCH=1
    DO_BENCH_COMPARE=1
    DO_FUZZ=1
    DO_DOCS=1
    DO_PACKAGE=1
    DO_LICENSE=1
    DO_TIDY=1
fi

if [ "${RELEASE_ONLY}" -eq 1 ] && [ "${DEBUG_ONLY}" -eq 1 ]; then
    echo "--release-only and --debug-only cannot be combined." >&2
    exit 2
fi
if [ "${DO_ASAN}" -eq 1 ] && [ "${RELEASE_ONLY}" -eq 1 ]; then
    echo "--asan requires a Debug build and cannot be combined with --release-only." >&2
    exit 2
fi
if [ "${DO_BENCH}" -eq 1 ] && [ "${DEBUG_ONLY}" -eq 1 ]; then
    echo "--bench/--bench-compare require a Release build and cannot use --debug-only." >&2
    exit 2
fi

# ---------------------------------------------------------------------------
# Reproducible comparison-benchmark inputs.
# ---------------------------------------------------------------------------

BENCH_DEPS_DIR="${SCRIPT_DIR}/.benchmark-deps"

# Fetches the exact third-party sources used by comparison benchmarks, then
# rejects dirty or revision-mismatched checkouts before they enter a build.
fetch_benchmark_compare_deps() {
    local nlohmann_dir="${BENCH_DEPS_DIR}/nlohmann-json-v3.11.3"
    local rapidjson_dir="${BENCH_DEPS_DIR}/rapidjson-v1.1.0"
    local simdjson_dir="${BENCH_DEPS_DIR}/simdjson-v3.12.2"
    local need_fetch=0

    if [ ! -f "${nlohmann_dir}/single_include/nlohmann/json.hpp" ]; then
        need_fetch=1
    fi
    if [ ! -f "${rapidjson_dir}/include/rapidjson/document.h" ]; then
        need_fetch=1
    fi
    if [ ! -f "${simdjson_dir}/CMakeLists.txt" ]; then
        need_fetch=1
    fi
    ensure_tool git 1
    if [ "${need_fetch}" -ne 0 ]; then
        echo ">> Benchmark comparison dependencies are not installed."
        echo "   Proposed download: ${BENCH_DEPS_DIR}"
        echo "   Pinned versions: nlohmann/json v3.11.3, RapidJSON v1.1.0, simdjson v3.12.2"

        if [ "${AUTO}" -eq 1 ]; then
            echo "   --auto: downloading without prompting."
        else
            printf "   Download them now? [y/N] "
            local reply=""
            read -r reply || reply=""
            case "${reply}" in
                y|Y|yes|YES) ;;
                *) echo ">> --bench-compare requires those pinned sources." >&2; exit 1 ;;
            esac
        fi

        mkdir -p "${BENCH_DEPS_DIR}"
        # Each destination is a fixed child of .benchmark-deps. Removing an
        # incomplete clone here cannot affect caller-selected paths.
        if [ ! -d "${nlohmann_dir}/.git" ]; then
            rm -rf "${nlohmann_dir}"
            git clone --depth 1 --branch v3.11.3 https://github.com/nlohmann/json.git "${nlohmann_dir}"
        fi
        if [ ! -d "${rapidjson_dir}/.git" ]; then
            rm -rf "${rapidjson_dir}"
            git clone --depth 1 --branch v1.1.0 https://github.com/Tencent/rapidjson.git "${rapidjson_dir}"
        fi
        if [ ! -d "${simdjson_dir}/.git" ]; then
            rm -rf "${simdjson_dir}"
            git clone --depth 1 --branch v3.12.2 https://github.com/simdjson/simdjson.git "${simdjson_dir}"
        fi
    fi

    if [ -n "$(git -C "${nlohmann_dir}" status --porcelain --untracked-files=no)" ] ||
       [ -n "$(git -C "${rapidjson_dir}" status --porcelain --untracked-files=no)" ] ||
       [ -n "$(git -C "${simdjson_dir}" status --porcelain --untracked-files=no)" ]; then
        echo ">> A benchmark dependency checkout has local modifications." >&2
        echo "   Remove .benchmark-deps and retry to restore pinned sources." >&2
        exit 1
    fi

    local nlohmann_commit rapidjson_commit simdjson_commit
    nlohmann_commit="$(git -C "${nlohmann_dir}" rev-parse HEAD)"
    rapidjson_commit="$(git -C "${rapidjson_dir}" rev-parse HEAD)"
    simdjson_commit="$(git -C "${simdjson_dir}" rev-parse HEAD)"
    if [ "${nlohmann_commit}" != "9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03" ] ||
       [ "${rapidjson_commit}" != "f54b0e47a08782a6131cc3d60f94d038fa6e0a51" ] ||
       [ "${simdjson_commit}" != "797e61742c9dbabed421dac77b9c3d8acc463afe" ]; then
        echo ">> Benchmark dependency revision mismatch; remove .benchmark-deps and retry." >&2
        exit 1
    fi
}

# ---------------------------------------------------------------------------
# Tool bootstrap.
#
# Detects the OS package manager once, then ensure_tool <cmd> [required]
# locates a tool (PATH, Homebrew LLVM prefix, or versioned binary) and, if
# missing, offers to install it. With --auto it installs without prompting.
# The path to the resolved tool is returned in the global RESOLVED_TOOL.
# ---------------------------------------------------------------------------

PKG_INSTALL=""
PKG_KIND=""

# Detects and caches the first supported system package manager.
detect_pkg_manager() {
    if [ -n "${PKG_KIND}" ]; then
        return
    fi
    if command -v brew >/dev/null 2>&1; then
        PKG_KIND="brew";    PKG_INSTALL="brew install"
    elif command -v apt-get >/dev/null 2>&1; then
        PKG_KIND="apt";     PKG_INSTALL="sudo apt-get install -y"
    elif command -v dnf >/dev/null 2>&1; then
        PKG_KIND="dnf";     PKG_INSTALL="sudo dnf install -y"
    elif command -v yum >/dev/null 2>&1; then
        PKG_KIND="yum";     PKG_INSTALL="sudo yum install -y"
    elif command -v pacman >/dev/null 2>&1; then
        PKG_KIND="pacman";  PKG_INSTALL="sudo pacman -S --noconfirm"
    elif command -v zypper >/dev/null 2>&1; then
        PKG_KIND="zypper";  PKG_INSTALL="sudo zypper install -y"
    elif command -v apk >/dev/null 2>&1; then
        PKG_KIND="apk";     PKG_INSTALL="sudo apk add"
    elif command -v choco >/dev/null 2>&1; then
        PKG_KIND="choco";   PKG_INSTALL="choco install -y"
    elif command -v winget >/dev/null 2>&1; then
        PKG_KIND="winget";  PKG_INSTALL="winget install -e --id"
    else
        PKG_KIND="none";    PKG_INSTALL=""
    fi
}

# Maps a logical tool to the package name for the detected manager.
package_for() {
    local tool="$1"
    case "${tool}" in
        cmake)
            case "${PKG_KIND}" in
                choco)   echo "cmake" ;;
                winget)  echo "Kitware.CMake" ;;
                *)       echo "cmake" ;;
            esac ;;
        git)
            case "${PKG_KIND}" in
                winget) echo "Git.Git" ;;
                *)      echo "git" ;;
            esac ;;
        pkg-config)
            case "${PKG_KIND}" in
                brew)     echo "pkgconf" ;;
                apt)      echo "pkg-config" ;;
                dnf|yum)  echo "pkgconf-pkg-config" ;;
                pacman)   echo "pkgconf" ;;
                zypper)   echo "pkg-config" ;;
                apk)      echo "pkgconf" ;;
                choco)    echo "pkgconfiglite" ;;
                *)        echo "" ;;
            esac ;;
        clang-format|clang-tidy)
            case "${PKG_KIND}" in
                brew)     echo "llvm" ;;
                apt)      echo "clang-format clang-tidy" ;;
                dnf|yum)  echo "clang-tools-extra" ;;
                pacman)   echo "clang" ;;
                zypper)   echo "clang-tools" ;;
                apk)      echo "clang-extra-tools" ;;
                choco)    echo "llvm" ;;
                winget)   echo "LLVM.LLVM" ;;
                *)        echo "" ;;
            esac ;;
        *) echo "${tool}" ;;
    esac
}

# Extra directories to search beyond PATH (Homebrew LLVM is keg-only; Windows
# LLVM installs into Program Files).
extra_bin_dirs() {
    echo "/opt/homebrew/opt/llvm/bin"
    echo "/usr/local/opt/llvm/bin"
    echo "/opt/homebrew/bin"
    echo "/usr/local/bin"
    echo "/c/Program Files/LLVM/bin"
    echo "/c/Program Files/CMake/bin"
}

# Finds a command on PATH, in the extra dirs, or as a versioned binary
# (clang-format-18, ...). Echoes the resolved path, or nothing if not found.
find_tool() {
    local tool="$1"
    if command -v "${tool}" >/dev/null 2>&1; then
        command -v "${tool}"
        return
    fi
    if [ "${tool}" = "pkg-config" ] && command -v pkgconf >/dev/null 2>&1; then
        command -v pkgconf
        return
    fi
    local d
    for d in $(extra_bin_dirs); do
        if [ -x "${d}/${tool}" ]; then
            echo "${d}/${tool}"
            return
        fi
        local cand
        cand=$(ls "${d}/${tool}"-* 2>/dev/null | sort -V | tail -1 || true)
        if [ -n "${cand}" ] && [ -x "${cand}" ]; then
            echo "${cand}"
            return
        fi
    done
}

# ensure_tool <tool> <required:0|1> -> sets RESOLVED_TOOL, returns non-zero when
# an optional tool is unavailable (a required one aborts the script).
RESOLVED_TOOL=""
ensure_tool() {
    local tool="$1"
    local required="${2:-0}"
    RESOLVED_TOOL=""

    local found
    found="$(find_tool "${tool}")"
    if [ -n "${found}" ]; then
        RESOLVED_TOOL="${found}"
        return 0
    fi

    detect_pkg_manager
    local pkg
    pkg="$(package_for "${tool}")"

    if [ "${PKG_KIND}" = "none" ] || [ -z "${pkg}" ]; then
        echo ">> '${tool}' not found and no known package manager to install it." >&2
        if [ "${required}" -eq 1 ]; then
            echo "   Please install '${tool}' manually and re-run." >&2
            exit 1
        fi
        echo "   Skipping the step that needs '${tool}'." >&2
        return 1
    fi

    echo ">> '${tool}' is not installed."
    echo "   Proposed install: ${PKG_INSTALL} ${pkg}   (via ${PKG_KIND})"
    if [ "${AUTO}" -ne 1 ]; then
        printf "   Install it now? [y/N] "
        local reply=""
        read -r reply || reply=""
        case "${reply}" in
            y|Y|yes|YES) ;;
            *)
                echo "   Skipped."
                if [ "${required}" -eq 1 ]; then
                    echo "   '${tool}' is required to continue; aborting." >&2
                    exit 1
                fi
                return 1 ;;
        esac
    else
        echo "   --auto: installing without prompting."
    fi

    echo ">> Installing ${pkg} ..."
    # shellcheck disable=SC2086
    if ! ${PKG_INSTALL} ${pkg}; then
        echo ">> Install of '${pkg}' failed." >&2
        [ "${required}" -eq 1 ] && exit 1
        return 1
    fi

    found="$(find_tool "${tool}")"
    if [ -n "${found}" ]; then
        RESOLVED_TOOL="${found}"
        return 0
    fi
    echo ">> '${tool}' still not found after install." >&2
    [ "${required}" -eq 1 ] && exit 1
    return 1
}

# All source files to format / lint.
source_files() {
    find "${SCRIPT_DIR}/pjsonlib" "${SCRIPT_DIR}/pjsontest" "${SCRIPT_DIR}/examples" \
        "${SCRIPT_DIR}/bench" "${SCRIPT_DIR}/fuzz" "${SCRIPT_DIR}/test_package" \
        "${SCRIPT_DIR}/tests" \
        \( -name '*.cpp' -o -name '*.h' \) -type f \
        ! -path '*/third_party/*' | sort
}

# ---------------------------------------------------------------------------
# Formatting / linting (run before the build so style issues surface first).
# ---------------------------------------------------------------------------

# Formats every maintained C/C++ source in place when clang-format is available.
run_format() {
    ensure_tool clang-format 0 || return 0
    echo ">> Formatting sources with ${RESOLVED_TOOL}"
    source_files | while read -r f; do
        "${RESOLVED_TOOL}" -i "${f}"
    done
    echo "   Done."
}

# Reports every source that clang-format would change and fails as one batch.
run_check() {
    ensure_tool clang-format 0 || return 0
    echo ">> Checking formatting with ${RESOLVED_TOOL} --dry-run -Werror"
    local bad=0
    while read -r f; do
        if ! "${RESOLVED_TOOL}" --dry-run -Werror "${f}" >/dev/null 2>&1; then
            echo "   NEEDS FORMATTING: ${f}"
            bad=1
        fi
    done < <(source_files)
    if [ "${bad}" -ne 0 ]; then
        echo ">> Formatting check failed. Run: ./build.sh --format" >&2
        exit 1
    fi
    echo "   All files are correctly formatted."
}

TIDY_TOOL=""
if [ "${DO_FORMAT}" -eq 1 ]; then run_format; fi
if [ "${DO_CHECK}" -eq 1 ];  then run_check;  fi
if [ "${DO_TIDY}" -eq 1 ];   then ensure_tool clang-tidy 0 && TIDY_TOOL="${RESOLVED_TOOL}"; fi

# ---------------------------------------------------------------------------
# Build. cmake (and its bundled ctest) are required.
# ---------------------------------------------------------------------------
ensure_tool cmake 1
CMAKE="${RESOLVED_TOOL}"
CTEST="$(dirname "${CMAKE}")/ctest"
[ -x "${CTEST}" ] || CTEST="ctest"
PKG_CONFIG_TOOL=""
if [ "${DO_PACKAGE}" -eq 1 ]; then
    ensure_tool pkg-config 1
    PKG_CONFIG_TOOL="${RESOLVED_TOOL}"
fi

if [ "${DO_CLEAN}" -eq 1 ]; then
    echo ">> Cleaning ${OUT_DIR}"
    rm -rf "${OUT_DIR}"
fi

# Prefer Ninja if available (optional; the default generator works fine).
GEN_ARG=""
if command -v ninja >/dev/null 2>&1; then
    GEN_ARG="-GNinja"
fi

# build_one <Config> <extra-cmake-args...>
# Configures out/build-<cfg>, builds it, and copies artifacts to out/<cfg>/.
build_one() {
    local cfg="$1"; shift
    local lc
    lc="$(echo "${cfg}" | tr '[:upper:]' '[:lower:]')"
    local bdir="${OUT_DIR}/build-${lc}"
    local dest="${OUT_DIR}/${lc}"

    echo ">> Configuring ${cfg}"
    "${CMAKE}" -S "${SCRIPT_DIR}" -B "${bdir}" \
        -DCMAKE_BUILD_TYPE="${cfg}" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        ${GEN_ARG} "$@"

    echo ">> Building ${cfg}"
    # --config matters for multi-config generators (VS/Xcode); harmless for
    # single-config ones.
    "${CMAKE}" --build "${bdir}" --config "${cfg}" --parallel

    echo ">> Collecting ${cfg} artifacts into ${dest}"
    mkdir -p "${dest}/lib" "${dest}/bin/examples" "${OUT_DIR}/include"
    # Library (libpjson.a / pjson.lib), test runner, and example executables.
    find "${bdir}" \( -name 'libpjson.a' -o -name 'pjson.lib' \) \
        -exec cp {} "${dest}/lib/" \; 2>/dev/null || true
    find "${bdir}" -type f \( -name 'pjsontest' -o -name 'pjsontest.exe' \) \
        -exec cp {} "${dest}/bin/" \; 2>/dev/null || true
    find "${bdir}" -type f \( -name 'pjsonbench' -o -name 'pjsonbench.exe' \) \
        -exec cp {} "${dest}/bin/" \; 2>/dev/null || true
    # Example binaries live under examples/ in the build tree.
    find "${bdir}/examples" -maxdepth 2 -type f \
        \( -perm -u+x -o -name '*.exe' \) ! -name '*.o' ! -name '*.obj' \
        -exec cp {} "${dest}/bin/examples/" \; 2>/dev/null || true
    cp "${SCRIPT_DIR}/pjsonlib/include/pjson.h" "${OUT_DIR}/include/"

    LAST_BUILD_DIR="${bdir}"
}

LAST_BUILD_DIR=""
BENCH_COMPARE_CMAKE=OFF
if [ "${DO_BENCH_COMPARE}" -eq 1 ]; then
    fetch_benchmark_compare_deps
    BENCH_COMPARE_CMAKE=ON
fi

# The build driver always produces the repository's normal developer targets;
# flags select which additional checks run. State that contract explicitly so
# it does not depend on top-level option defaults.
PJSON_REPOSITORY_TARGET_ARGS=(
    -DPJSON_BUILD_TESTS=ON
    -DPJSON_BUILD_EXAMPLES=ON
    -DPJSON_BUILD_BENCHMARKS=ON
    -DBUILD_TESTING=ON
)

if [ "${DO_ASAN}" -eq 1 ]; then
    # --all wants the full picture, so it still produces an optimized Release
    # alongside the sanitized Debug (unless the user narrowed the configs).
    # Plain --asan produces one sanitized Debug build.
    if { [ "${DO_ALL}" -eq 1 ] || [ "${DO_BENCH}" -eq 1 ]; } &&
       [ "${DEBUG_ONLY}" -ne 1 ]; then
        build_one Release "${PJSON_REPOSITORY_TARGET_ARGS[@]}" \
            -DPJSON_BENCH_COMPARE="${BENCH_COMPARE_CMAKE}" \
            -DPJSON_BENCH_DEPS_DIR="${BENCH_DEPS_DIR}"
    fi
    echo ">> Sanitizers enabled (AddressSanitizer + UndefinedBehaviorSanitizer)"
    build_one Debug "${PJSON_REPOSITORY_TARGET_ARGS[@]}" \
        -DPJSON_SANITIZE=ON -DPJSON_BENCH_COMPARE=OFF
else
    if [ "${DEBUG_ONLY}" -ne 1 ]; then
        build_one Release "${PJSON_REPOSITORY_TARGET_ARGS[@]}" \
            -DPJSON_BENCH_COMPARE="${BENCH_COMPARE_CMAKE}" \
            -DPJSON_BENCH_DEPS_DIR="${BENCH_DEPS_DIR}"
    fi
    if [ "${RELEASE_ONLY}" -ne 1 ]; then
        build_one Debug "${PJSON_REPOSITORY_TARGET_ARGS[@]}" \
            -DPJSON_BENCH_COMPARE=OFF
    fi
fi

# Static analysis uses the last configured tree's compile_commands.json.
if [ -n "${TIDY_TOOL}" ] && [ -n "${LAST_BUILD_DIR}" ]; then
    echo ">> Running clang-tidy (${TIDY_TOOL})"
    TIDY_FAIL=0
    TIDY_EXTRA_ARGS=()
    # Homebrew LLVM is not built against Apple's SDK include layout. Point
    # clang-tidy at the active SDK so standard C++ headers resolve on macOS.
    if [ "$(uname -s)" = "Darwin" ] && command -v xcrun >/dev/null 2>&1; then
        TIDY_EXTRA_ARGS+=(--extra-arg=-isysroot --extra-arg="$(xcrun --show-sdk-path)")
    fi
    while read -r f; do
        echo "   tidy: ${f}"
        if ! "${TIDY_TOOL}" -p "${LAST_BUILD_DIR}" --warnings-as-errors='*' \
             "${TIDY_EXTRA_ARGS[@]}" "${f}"; then
            TIDY_FAIL=1
        fi
    done < <(find "${SCRIPT_DIR}/pjsonlib" -name '*.cpp' -type f | sort)
    if [ "${TIDY_FAIL}" -ne 0 ]; then
        echo ">> clang-tidy reported findings." >&2
        exit 1
    else
        echo "   clang-tidy: no findings."
    fi
fi

echo ">> Done. Artifacts under ${OUT_DIR}/ (release/ and/or debug/, plus include/)."

# ---------------------------------------------------------------------------
# Test and benchmark execution.
# ---------------------------------------------------------------------------

if [ "${DO_TEST}" -eq 1 ] && [ -n "${LAST_BUILD_DIR}" ]; then
    echo ">> Running tests (${LAST_BUILD_DIR})"
    # Leak detection: LeakSanitizer ships with ASan on Linux but is absent from
    # Apple's runtime (setting detect_leaks=1 there aborts with "not supported").
    # Enable it where it exists so leaks fail the suite; elsewhere ASan still
    # catches use-after-free / overflow. Override by exporting ASAN_OPTIONS.
    case "$(uname -s)" in
        Linux*) ASAN_LEAK="detect_leaks=1" ;;
        *)      ASAN_LEAK="detect_leaks=0" ;;
    esac
    # Auto-discover the persistent default corpus. An explicitly exported
    # PJSON_JSONTESTSUITE_DIR always wins for custom/CI locations.
    DEFAULT_JSONTESTSUITE_DIR="${SCRIPT_DIR}/.test-corpora/JSONTestSuite"
    JSONTESTSUITE_DIR="${PJSON_JSONTESTSUITE_DIR:-}"
    if [ -z "${JSONTESTSUITE_DIR}" ] &&
       [ -d "${DEFAULT_JSONTESTSUITE_DIR}/test_parsing" ]; then
        JSONTESTSUITE_DIR="${DEFAULT_JSONTESTSUITE_DIR}"
        echo ">> JSONTestSuite found (${JSONTESTSUITE_DIR})"
    fi
    if [ "${DO_TEST}" -eq 1 ] && [ -z "${JSONTESTSUITE_DIR}" ]; then
        FETCH_CORPUS=0
        echo ">> JSONTestSuite is not installed."
        echo "   Proposed download: ${DEFAULT_JSONTESTSUITE_DIR}"
        if [ "${AUTO}" -eq 1 ]; then
            echo "   --auto: downloading without prompting."
            FETCH_CORPUS=1
        else
            printf "   Download it now? [y/N] "
            reply=""
            read -r reply || reply=""
            case "${reply}" in
                y|Y|yes|YES) FETCH_CORPUS=1 ;;
                *) echo "   Skipped; inline RFC 8259 conformance tests will still run." ;;
            esac
        fi
        if [ "${FETCH_CORPUS}" -eq 1 ]; then
            ensure_tool git 1
            if ! "${SCRIPT_DIR}/scripts/fetch-json-test-suite.sh"; then
                echo ">> JSONTestSuite download failed; the full --all sweep is incomplete." >&2
                exit 1
            fi
            JSONTESTSUITE_DIR="${DEFAULT_JSONTESTSUITE_DIR}"
        fi
    fi

    # The draft-07 schema corpus uses the same persistent, auto-discovered
    # convention as JSONTestSuite. An explicit environment variable wins.
    DEFAULT_JSON_SCHEMA_SUITE_DIR="${SCRIPT_DIR}/.test-corpora/JSON-Schema-Test-Suite"
    JSON_SCHEMA_SUITE_DIR="${PJSON_JSON_SCHEMA_TEST_SUITE_DIR:-}"
    if [ -z "${JSON_SCHEMA_SUITE_DIR}" ] &&
       [ -d "${DEFAULT_JSON_SCHEMA_SUITE_DIR}/tests/draft7" ]; then
        JSON_SCHEMA_SUITE_DIR="${DEFAULT_JSON_SCHEMA_SUITE_DIR}"
        echo ">> JSON-Schema-Test-Suite found (${JSON_SCHEMA_SUITE_DIR})"
    fi
    if [ "${DO_TEST}" -eq 1 ] && [ -z "${JSON_SCHEMA_SUITE_DIR}" ]; then
        FETCH_SCHEMA_CORPUS=0
        echo ">> JSON-Schema-Test-Suite is not installed."
        echo "   Proposed download: ${DEFAULT_JSON_SCHEMA_SUITE_DIR}"
        if [ "${AUTO}" -eq 1 ]; then
            echo "   --auto: downloading without prompting."
            FETCH_SCHEMA_CORPUS=1
        else
            printf "   Download it now? [y/N] "
            reply=""
            read -r reply || reply=""
            case "${reply}" in
                y|Y|yes|YES) FETCH_SCHEMA_CORPUS=1 ;;
                *) echo "   Skipped; inline schema tests will still run." ;;
            esac
        fi
        if [ "${FETCH_SCHEMA_CORPUS}" -eq 1 ]; then
            ensure_tool git 1
            if ! "${SCRIPT_DIR}/scripts/fetch-json-schema-test-suite.sh"; then
                echo ">> JSON-Schema-Test-Suite download failed; the full --all sweep is incomplete." >&2
                exit 1
            fi
            JSON_SCHEMA_SUITE_DIR="${DEFAULT_JSON_SCHEMA_SUITE_DIR}"
        fi
    fi
    ASAN_OPTIONS="${ASAN_OPTIONS:-${ASAN_LEAK}}" \
    LSAN_OPTIONS="${LSAN_OPTIONS:-}" \
    UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}" \
    PJSON_JSONTESTSUITE_DIR="${JSONTESTSUITE_DIR}" \
    PJSON_JSON_SCHEMA_TEST_SUITE_DIR="${JSON_SCHEMA_SUITE_DIR}" \
        "${CTEST}" --test-dir "${LAST_BUILD_DIR}" --output-on-failure
fi

# Runs the optimized benchmark binary, resolving both single- and multi-config
# generator output layouts before forwarding optional corpus/compare flags.
if [ "${DO_BENCH}" -eq 1 ]; then
    RELEASE_BUILD_DIR="${OUT_DIR}/build-release"
    RELEASE_BENCH="${OUT_DIR}/release/bin/pjsonbench"
    if [ ! -x "${RELEASE_BENCH}" ] && [ -x "${RELEASE_BUILD_DIR}/bench/pjsonbench" ]; then
        RELEASE_BENCH="${RELEASE_BUILD_DIR}/bench/pjsonbench"
    elif [ ! -x "${RELEASE_BENCH}" ] && [ -x "${RELEASE_BUILD_DIR}/bench/Release/pjsonbench.exe" ]; then
        RELEASE_BENCH="${RELEASE_BUILD_DIR}/bench/Release/pjsonbench.exe"
    elif [ ! -x "${RELEASE_BENCH}" ] && [ -x "${OUT_DIR}/release/bin/pjsonbench.exe" ]; then
        RELEASE_BENCH="${OUT_DIR}/release/bin/pjsonbench.exe"
    fi

    if [ ! -x "${RELEASE_BENCH}" ]; then
        echo ">> Release benchmark executable not found. Build Release first." >&2
        exit 1
    fi

    BENCH_ARGS=()
    if [ "${#BENCH_INPUTS[@]}" -gt 0 ]; then
        for bench_input in "${BENCH_INPUTS[@]}"; do
            BENCH_ARGS+=(--input "${bench_input}")
        done
    fi
    if [ "${DO_BENCH_COMPARE}" -eq 1 ]; then
        BENCH_ARGS+=(--compare)
    fi
    if [ -n "${BENCH_JSON}" ]; then
        BENCH_ARGS+=(--json "${BENCH_JSON}")
    fi

    echo ">> Running benchmarks (Release)"
    if [ "${#BENCH_ARGS[@]}" -gt 0 ]; then
        "${RELEASE_BENCH}" "${BENCH_ARGS[@]}"
    else
        "${RELEASE_BENCH}"
    fi
fi

# ---------------------------------------------------------------------------
# Optional fuzz, documentation, and packaging validation.
# ---------------------------------------------------------------------------

# Probes for a usable Clang/libFuzzer pair, builds every harness, and
# replays each checked-in seed corpus with deterministic bounds.
run_fuzz_smoke() {
    case "$(uname -s)" in
        Linux*|Darwin*) ;;
        *)
            if [ "${DO_ALL}" -eq 1 ]; then
                echo ">> Skipping fuzz smoke: local libFuzzer builds support Linux/macOS."
                return 0
            fi
            echo ">> --fuzz is supported on Linux/macOS with Clang and libFuzzer." >&2
            return 1
            ;;
    esac

    local fuzz_cxx="${CXX:-}"
    if [ -z "${fuzz_cxx}" ]; then
        # Apple's Command Line Tools sometimes identify as Clang while omitting
        # libFuzzer. Prefer a full Homebrew LLVM when it is installed.
        local candidate
        for candidate in /opt/homebrew/opt/llvm/bin/clang++ \
                         /usr/local/opt/llvm/bin/clang++ clang++; do
            if [ -x "${candidate}" ] || command -v "${candidate}" >/dev/null 2>&1; then
                fuzz_cxx="${candidate}"
                break
            fi
        done
    fi
    if [ -z "${fuzz_cxx}" ]; then
        if [ "${DO_ALL}" -eq 1 ]; then
            echo ">> Skipping fuzz smoke: no Clang C++ compiler found."
            return 0
        fi
        echo ">> --fuzz requires Clang with the libFuzzer runtime." >&2
        return 1
    fi

    local compiler_id
    compiler_id="$("${fuzz_cxx}" --version 2>/dev/null | head -1 || true)"
    case "${compiler_id}" in
        *clang*) ;;
        *)
            if [ "${DO_ALL}" -eq 1 ]; then
                echo ">> Skipping fuzz smoke: ${fuzz_cxx} is not Clang."
                return 0
            fi
            echo ">> --fuzz requires Clang with the libFuzzer runtime." >&2
            return 1
            ;;
    esac

    local fuzz_cc="${CC:-}"
    if [ -z "${fuzz_cc}" ]; then
        fuzz_cc="${fuzz_cxx%++}"
        if [ ! -x "${fuzz_cc}" ]; then
            fuzz_cc="$(find_tool clang)"
        fi
    fi

    local probe_dir="${OUT_DIR}/fuzz-probe"
    mkdir -p "${probe_dir}"
    if ! printf '%s\n' \
        '#include <cstddef>' \
        '#include <cstdint>' \
        'extern "C" int LLVMFuzzerTestOneInput(const uint8_t*, size_t) { return 0; }' \
        | "${fuzz_cxx}" -x c++ -std=c++11 -fsanitize=fuzzer,address,undefined - \
            -o "${probe_dir}/probe" >/dev/null 2>&1; then
        if [ "${DO_ALL}" -eq 1 ]; then
            echo ">> Skipping fuzz smoke: ${fuzz_cxx} has no usable libFuzzer runtime."
            return 0
        fi
        echo ">> --fuzz requires a Clang toolchain with a usable libFuzzer runtime." >&2
        return 1
    fi

    local fuzz_build_dir="${OUT_DIR}/build-fuzz"
    echo ">> Configuring libFuzzer targets (${fuzz_cxx})"
    CC="${fuzz_cc}" CXX="${fuzz_cxx}" \
        "${CMAKE}" -S "${SCRIPT_DIR}" -B "${fuzz_build_dir}" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_CXX_COMPILER="${fuzz_cxx}" \
        -DPJSON_BUILD_TESTS=OFF \
        -DPJSON_BUILD_EXAMPLES=OFF \
        -DPJSON_BUILD_BENCHMARKS=OFF \
        -DPJSON_BUILD_FUZZERS=ON \
        ${GEN_ARG}
    "${CMAKE}" --build "${fuzz_build_dir}" --parallel --target \
        pjson_fuzz_parse pjson_fuzz_stream pjson_fuzz_serialize pjson_fuzz_schema \
        pjson_fuzz_pointer pjson_fuzz_patch pjson_fuzz_merge_patch

    local target corpus_dir
    for target in parse stream serialize schema pointer patch merge_patch; do
        corpus_dir="${OUT_DIR}/fuzz-corpus/${target}"
        mkdir -p "${corpus_dir}" "${OUT_DIR}/fuzz-artifacts/${target}"
        echo ">> Fuzz corpus smoke: pjson_fuzz_${target}"
        "${fuzz_build_dir}/fuzz/pjson_fuzz_${target}" \
            -runs=1000 -seed=1337 -max_len=65536 -timeout=5 -verbosity=0 \
            -dict="${SCRIPT_DIR}/fuzz/json.dict" \
            -artifact_prefix="${OUT_DIR}/fuzz-artifacts/${target}/" \
            "${corpus_dir}" "${SCRIPT_DIR}/fuzz/corpus/${target}"
    done
}

if [ "${DO_FUZZ}" -eq 1 ]; then
    run_fuzz_smoke
fi

# Builds the Doxygen reference and runs its API-surface validator.
run_docs_check() {
    ensure_tool doxygen 1
    ensure_tool python3 1
    local docs_build_dir="${OUT_DIR}/build-docs"
    echo ">> Building and validating API reference"
    "${CMAKE}" -S "${SCRIPT_DIR}" -B "${docs_build_dir}" \
        -DPJSON_BUILD_TESTS=OFF \
        -DPJSON_BUILD_EXAMPLES=OFF \
        -DPJSON_BUILD_BENCHMARKS=OFF \
        -DPJSON_BUILD_DOCS=ON ${GEN_ARG}
    "${CMAKE}" --build "${docs_build_dir}" --target pjson-docs-check --parallel
}

# Exercises relocated static/shared installations through external consumer
# projects rather than only inspecting generated metadata.
run_package_checks() {
    local package_root="${OUT_DIR}/package-smoke"
    echo ">> Validating relocatable static package"
    "${CMAKE}" -DPJSON_SOURCE_DIR="${SCRIPT_DIR}" \
        -DPJSON_WORK_DIR="${package_root}/static" \
        -DPJSON_INSTALL_LIBDIR=lib64 \
        -DPJSON_PKG_CONFIG_EXECUTABLE="${PKG_CONFIG_TOOL}" \
        -DPJSON_REQUIRE_PKG_CONFIG=ON \
        -P "${SCRIPT_DIR}/cmake/RunInstallConsumer.cmake"

    echo ">> Validating relocatable shared package"
    "${CMAKE}" -DPJSON_SOURCE_DIR="${SCRIPT_DIR}" \
        -DPJSON_WORK_DIR="${package_root}/shared" \
        -DPJSON_BUILD_SHARED_LIBS=ON \
        -DPJSON_INSTALL_LIBDIR=lib64 \
        -DPJSON_PKG_CONFIG_EXECUTABLE="${PKG_CONFIG_TOOL}" \
        -DPJSON_REQUIRE_PKG_CONFIG=ON \
        -P "${SCRIPT_DIR}/cmake/RunInstallConsumer.cmake"
}

if [ "${DO_DOCS}" -eq 1 ]; then
    run_docs_check
fi

if [ "${DO_PACKAGE}" -eq 1 ]; then
    run_package_checks
fi

# Validates every tracked file's SPDX metadata. Prefer an existing installation;
# otherwise install the pinned Python package into out/ so the tool remains
# repository-local and clean.sh removes it.
run_license_check() {
    ensure_tool python3 1
    local python_tool="${RESOLVED_TOOL}"
    local reuse_target="${OUT_DIR}/tools/reuse"

    echo ">> Validating SPDX/REUSE licensing metadata"
    if "${python_tool}" -m reuse --version >/dev/null 2>&1; then
        "${python_tool}" -m reuse lint
        return
    fi

    echo "   Python package 'reuse==6.2.0' is not installed."
    echo "   Proposed download: ${reuse_target}"
    if [ "${AUTO}" -ne 1 ]; then
        printf "   Download it now? [y/N] "
        local reply=""
        read -r reply || reply=""
        case "${reply}" in
            y|Y|yes|YES) ;;
            *) echo ">> --license requires reuse 6.2.0." >&2; exit 1 ;;
        esac
    else
        echo "   --auto: downloading without prompting."
    fi

    "${python_tool}" -m pip install --disable-pip-version-check \
        --target "${reuse_target}" reuse==6.2.0
    PYTHONPATH="${reuse_target}${PYTHONPATH:+:${PYTHONPATH}}" \
        "${python_tool}" -m reuse lint
}

if [ "${DO_LICENSE}" -eq 1 ]; then
    run_license_check
fi
