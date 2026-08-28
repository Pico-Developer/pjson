#!/usr/bin/env bash

# Fetch the exact JSON parser conformance corpus used by pjsontest.

set -euo pipefail

# ---- Repository paths and pinned input ---------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

DEFAULT_DEST="${REPO_ROOT}/.test-corpora/JSONTestSuite"
SOURCE_URL="${PJSON_JSONTESTSUITE_URL:-https://github.com/nst/JSONTestSuite.git}"
PINNED_COMMIT="${PJSON_JSONTESTSUITE_COMMIT:-1ef36fa01286573e846ac449e8683f8833c5b26a}"

# Prints destination, override, and revision details without touching disk.
usage() {
    cat <<EOF
Usage: $(basename "$0") [destination]

Clones or updates nst/JSONTestSuite into a dedicated local test-corpus directory.

Environment overrides:
  PJSON_JSONTESTSUITE_DIR  Destination directory
  PJSON_JSONTESTSUITE_URL  Alternate git remote
  PJSON_JSONTESTSUITE_COMMIT  Alternate pinned commit

Default destination:
  ${DEFAULT_DEST}
EOF
}

DEST="${PJSON_JSONTESTSUITE_DIR:-${DEFAULT_DEST}}"
# ---- Command-line destination ------------------------------------------

if [ "$#" -gt 1 ]; then
    usage >&2
    exit 1
fi
if [ "$#" -eq 1 ]; then
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        -*)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
        *)
            DEST="$1"
            ;;
    esac
fi

# Never allow a typo or empty override to turn the repository root or the
# filesystem root into a Git checkout destination.
case "${DEST}" in
    "${REPO_ROOT}"|/|"" )
        echo "Refusing to use unsafe destination: '${DEST}'" >&2
        exit 1
        ;;
esac

mkdir -p "$(dirname "${DEST}")"

# ---- Idempotent pinned checkout ----------------------------------------

# Preserve local work in an existing clone. Clean checkouts are detached at the
# pinned commit; the fetch itself is depth-limited to minimize network traffic.
if [ -d "${DEST}/.git" ]; then
    echo "Updating JSONTestSuite in ${DEST}"
    if [ -n "$(git -C "${DEST}" status --short)" ]; then
        echo "Destination checkout has local changes; refusing to overwrite ${DEST}" >&2
        exit 1
    fi
    git -C "${DEST}" fetch --depth=1 origin "${PINNED_COMMIT}"
    git -C "${DEST}" checkout --detach "${PINNED_COMMIT}"
elif [ -e "${DEST}" ]; then
    echo "Destination exists and is not a git checkout: ${DEST}" >&2
    exit 1
else
    echo "Cloning JSONTestSuite into ${DEST}"
    git clone --no-checkout --filter=blob:none "${SOURCE_URL}" "${DEST}"
    git -C "${DEST}" fetch --depth=1 origin "${PINNED_COMMIT}"
    git -C "${DEST}" checkout --detach "${PINNED_COMMIT}"
fi

# ---- Corpus integrity checks -------------------------------------------

if [ "$(git -C "${DEST}" rev-parse HEAD)" != "${PINNED_COMMIT}" ]; then
    echo "JSONTestSuite checkout is not at the pinned commit ${PINNED_COMMIT}" >&2
    exit 1
fi

if [ ! -d "${DEST}/test_parsing" ]; then
    echo "Expected test_parsing directory not found under ${DEST}" >&2
    exit 1
fi

echo "JSONTestSuite ready at ${DEST}"
if [ "${DEST}" = "${DEFAULT_DEST}" ]; then
    echo "build.sh will discover this checkout automatically. Run:"
    echo "  ./build.sh --test"
else
    echo "Custom destination: point the test runner at it with:"
    echo "  PJSON_JSONTESTSUITE_DIR=${DEST} ./build.sh --test"
fi
