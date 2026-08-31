<!-- SPDX-FileCopyrightText: 2026 ByteDance Ltd. and/or its affiliates -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Release Process

This checklist is for project maintainers. Releases use Semantic Versioning and
the `MAJOR.MINOR.PATCH` tag convention described in
[`VERSIONING.md`](VERSIONING.md).

## 1. Prepare the release

1. Choose the version from the user-visible changes since the previous release.
2. Confirm that the release commit will come from `main`, the worktree is clean,
   and required GitHub checks are green.
3. Resolve or explicitly defer every release-blocking bug and private security
   advisory. Never expose embargoed details in a public release pull request.
4. Update every version source listed in `VERSIONING.md` and search the tree for
   stale copies of the previous version.
5. Move the accumulated `Unreleased` changelog entries into a heading for the
   new version and UTC release date. Leave a fresh `Unreleased` heading above
   it. Verify all changelog links.

The release preparation should be reviewed as a pull request. Avoid unrelated
changes in that pull request so the release diff is auditable.

## 2. Verify the candidate

Run the repository's complete local verification from the release commit:

```sh
./build.sh --all --auto
```

Verify that every distributed file has complete, valid SPDX metadata and that
all referenced license texts are present:

```sh
python3 -m pip install --disable-pip-version-check reuse==6.2.0
reuse lint
```

The full sweep covers compilation, registered tests, formatting, sanitizers,
benchmarks, clang-tidy, documentation generation, package checks, and bounded
fuzz corpus replay when supported. The `--package` phase validates relocatable
static and shared installs through both CMake-package and pkg-config consumers.
The sweep offers to fetch both pinned conformance corpora; `--auto` accepts them
without prompting. An explicit fuzz run is required on a release machine with
Clang and libFuzzer so a missing runtime cannot be treated as an optional
full-sweep skip:

```sh
./build.sh --fuzz --auto
```

Fetch the separately pinned official JSON Schema corpus and run its supported
draft-07 manifest against the release candidate:

```sh
./scripts/fetch-json-schema-test-suite.sh
PJSON_JSON_SCHEMA_TEST_SUITE_DIR="$PWD/.test-corpora/JSON-Schema-Test-Suite" \
  ctest --test-dir out/build-debug --output-on-failure \
  -R '^pjson\.schema_official_draft7_optional$'
```

The full sweep already builds and validates the release API reference. To rerun
that focused check (which requires Doxygen and Python 3):

```sh
cmake -S . -B out/build-docs \
  -DPJSON_BUILD_TESTS=OFF \
  -DPJSON_BUILD_EXAMPLES=OFF \
  -DPJSON_BUILD_BENCHMARKS=OFF \
  -DPJSON_BUILD_DOCS=ON
cmake --build out/build-docs --target pjson-docs-check
PJSON_RELEASE_TMP_DIR="$(mktemp -d)"
tar -C out/build-docs/docs/reference/html \
  -czf "${PJSON_RELEASE_TMP_DIR}/pjson-api-reference.tar.gz" .
tar -tzf "${PJSON_RELEASE_TMP_DIR}/pjson-api-reference.tar.gz" |
  grep -q '^./index.html$'
```

Verify both static and shared relocatable installs through external CMake and
pkg-config consumers, including the CMake config/version/targets metadata and
the installed header and library. Requiring pkg-config here prevents that
consumer path from being silently skipped. Run the package smoke tests from a
clean working directory:

```sh
PJSON_RELEASE_TMP_DIR="$(mktemp -d)"
cmake -DPJSON_SOURCE_DIR="$PWD" \
  -DPJSON_WORK_DIR="${PJSON_RELEASE_TMP_DIR}/package-smoke/static" \
  -DPJSON_INSTALL_LIBDIR=lib64 \
  -DPJSON_REQUIRE_PKG_CONFIG=ON \
  -P cmake/RunInstallConsumer.cmake
cmake -DPJSON_SOURCE_DIR="$PWD" \
  -DPJSON_WORK_DIR="${PJSON_RELEASE_TMP_DIR}/package-smoke/shared" \
  -DPJSON_BUILD_SHARED_LIBS=ON \
  -DPJSON_INSTALL_LIBDIR=lib64 \
  -DPJSON_REQUIRE_PKG_CONFIG=ON \
  -P cmake/RunInstallConsumer.cmake
test -f "${PJSON_RELEASE_TMP_DIR}/package-smoke/static/relocated/share/licenses/pjson/LICENSE"
test -f "${PJSON_RELEASE_TMP_DIR}/package-smoke/shared/relocated/share/licenses/pjson/LICENSE"
```

Run the package-manager checks with Conan 2 and a current vcpkg checkout. These
are release checks, not optional contributor-tool discovery; install the tools
on the release machine before continuing. Use an empty disposable directory for
`CONAN_HOME`; `mktemp -d` is portable across the supported Unix release hosts:

```sh
python3 -m py_compile conanfile.py test_package/conanfile.py
export CONAN_HOME="$(mktemp -d)"
conan profile detect --force
conan create . -s build_type=Release --build=missing
python3 -m json.tool packaging/vcpkg/ports/pjson/vcpkg.json >/dev/null
"$VCPKG_ROOT/vcpkg" install pjson \
  --overlay-ports="$PWD/packaging/vcpkg/ports"
PJSON_RELEASE_TMP_DIR="$(mktemp -d)"
cmake -S tests/install-consumer \
  -B "${PJSON_RELEASE_TMP_DIR}/vcpkg-consumer" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build "${PJSON_RELEASE_TMP_DIR}/vcpkg-consumer" \
  --config Release --parallel
ctest --test-dir "${PJSON_RELEASE_TMP_DIR}/vcpkg-consumer" \
  -C Release --output-on-failure
```

The Conan test package and vcpkg overlay must build the release checkout and
consume the installed `pjson::pjson` target. Treat any recipe, consumer,
relocation, metadata, or documentation failure as a release blocker. Record the
commands, tool versions, platforms, and results in the release pull request.

Review the candidate diff and provenance before continuing:

```sh
git status --short
git diff <previous-release-tag>...HEAD
git log --oneline <previous-release-tag>..HEAD
```

## 3. Tag and publish

After the release pull request is merged and the exact release commit is checked
out, create an annotated tag. Sign it when the maintainer has a configured,
project-recognized signing key.

```sh
git tag -a X.Y.Z -m "pjson X.Y.Z"
git push origin X.Y.Z
```

Create a GitHub release from that exact tag. Use `pjson X.Y.Z` as the title and
copy the matching changelog section into the release notes. The documentation
workflow builds and attaches `pjson-api-reference.tar.gz` from the release tag;
wait for that workflow and treat a missing or failed archive as a release
failure. Attach SHA-256 checksums for any additional downloadable artifacts. Do
not rebuild or replace artifacts from a different commit.

## 4. Verify publication

- Confirm the tag resolves to the reviewed commit.
- Download each published artifact and verify its checksum and version.
- Build or consume at least one downloaded artifact in a clean directory.
- Open the downloaded API reference's `index.html` and confirm its version and
  migration pages match the tag.
- Check that changelog comparison links and package metadata resolve correctly.
- Announce the release only after these checks pass.

## 5. After the release

Confirm that `main` has an empty `Unreleased` changelog section ready for future
work. Monitor installation and compatibility reports, and publish a patch release
for release-specific defects.

Never move or silently replace a published tag or artifact. If a release is
unsafe, mark it clearly in GitHub, notify users through the security advisory or
release notes, and publish a corrected version with a new tag.
