#!/usr/bin/env bash

# Fetch the exact JSON Schema draft-07 conformance corpus used by pjsontest.

set -euo pipefail

# ---- Repository paths and pinned input ---------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

PINNED_COMMIT="3c25e5f709192aadf67cf7f2eb19771a57131fec"
SOURCE_URL="${PJSON_JSON_SCHEMA_TEST_SUITE_URL:-https://github.com/json-schema-org/JSON-Schema-Test-Suite.git}"
DEFAULT_DEST="${REPO_ROOT}/.test-corpora/JSON-Schema-Test-Suite"

# Prints destination, override, and revision details without touching disk.
usage() {
    cat <<EOF
Usage: $(basename "$0") [destination]

Clones or updates JSON-Schema-Test-Suite to the pinned commit used by pjsontest.

Environment overrides:
  PJSON_JSON_SCHEMA_TEST_SUITE_DIR  Destination directory
  PJSON_JSON_SCHEMA_TEST_SUITE_URL  Alternate git remote

Pinned commit:
  ${PINNED_COMMIT}

Default destination:
  ${DEFAULT_DEST}
EOF
}

DEST="${PJSON_JSON_SCHEMA_TEST_SUITE_DIR:-${DEFAULT_DEST}}"
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
    "${REPO_ROOT}"|/|"")
        echo "Refusing to use unsafe destination: '${DEST}'" >&2
        exit 1
        ;;
esac

mkdir -p "$(dirname "${DEST}")"

# ---- Idempotent pinned checkout ----------------------------------------

# Preserve local work in an existing clone. Clean checkouts are detached at the
# pinned commit; the fetch itself is depth-limited to minimize network traffic.
if [ -d "${DEST}/.git" ]; then
    echo "Updating JSON-Schema-Test-Suite in ${DEST}"
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
    echo "Cloning JSON-Schema-Test-Suite into ${DEST}"
    git clone --no-checkout "${SOURCE_URL}" "${DEST}"
    git -C "${DEST}" fetch --depth=1 origin "${PINNED_COMMIT}"
    git -C "${DEST}" checkout --detach "${PINNED_COMMIT}"
fi

# ---- Corpus integrity checks -------------------------------------------

if [ ! -d "${DEST}/tests/draft7" ]; then
    echo "Expected tests/draft7 directory not found under ${DEST}" >&2
    exit 1
fi

if [ "$(git -C "${DEST}" rev-parse HEAD)" != "${PINNED_COMMIT}" ]; then
    echo "JSON-Schema-Test-Suite checkout is not at the pinned commit ${PINNED_COMMIT}" >&2
    exit 1
fi

echo "JSON-Schema-Test-Suite ready at ${DEST}"
echo "Pinned commit: ${PINNED_COMMIT}"
echo "Run tests with:"
echo "  export PJSON_JSON_SCHEMA_TEST_SUITE_DIR=${DEST}"
echo "  ctest --test-dir ${REPO_ROOT}/out/build-debug"
echo "    -R schema_official_draft7_optional -V"
