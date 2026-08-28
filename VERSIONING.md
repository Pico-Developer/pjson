<!-- SPDX-FileCopyrightText: 2026 ByteDance Ltd. and/or its affiliates -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Versioning Policy

pjson uses [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html). The
current source/development version is **1.0.0**, which remains unreleased until
the `release-1.0.0` tag is published. The latest published tag is `release-0.0.3`;
historical `0.0.x` releases predate this stability policy.

## Version meaning

- **MAJOR** changes may require users to update source code or intentionally
  change documented behavior.
- **MINOR** changes add backward-compatible public functionality.
- **PATCH** changes contain backward-compatible fixes, documentation, or
  internal improvements.

Security fixes normally use the smallest compatible version increment. When a
safe fix cannot preserve compatibility, the security advisory and release notes
must call that out explicitly.

## Compatibility boundary

The public contract includes:

- declarations, types, constants, and macros in the installed `pjson.h`;
- documented behavior of parsing, serialization, validation, and mutation;
- the `pjson::pjson` CMake target and installed package names; and
- documented compile-time requirements, including C++11 support.

Private implementation details, tests, benchmarks, examples, diagnostics not
documented as stable, and repository layout outside installed artifacts are not
public API.

Semantic versioning describes source and behavioral compatibility. pjson does
not currently promise a stable C++ ABI across releases, compilers, standard
libraries, compiler flags, or build configurations. Rebuild pjson and dependent
C++ binaries together when upgrading.

## Deprecation

When practical, a public API scheduled for removal is deprecated for at least
one minor release and documented in the changelog. Immediate removal is
reserved for cases where retaining the behavior would be unsafe or materially
misleading. Removal of a supported public API requires a major release.

## Version sources

For each release, the following values must agree:

- the top-level CMake project version;
- `PJSON_VERSION`, `PJSON_VERSION_MAJOR`, `PJSON_VERSION_MINOR`, and
  `PJSON_VERSION_PATCH` in `pjson.h`;
- package-manager or distribution metadata; and
- the release heading in `CHANGELOG.md`.

The release checklist verifies these values before a tag is created. A mismatch
is a release-blocking defect.

## Tags and pre-releases

Release tags follow the repository's established
`release-MAJOR.MINOR.PATCH` form, for example `release-1.0.0`. Pre-release
identifiers use Semantic Versioning syntax, for example
`release-1.1.0-rc.1`. Published tags are immutable; a correction is released
under a new version rather than moving an existing tag.

Development happens on `main`. Until a release tag is published, its changes
remain under the `Unreleased` heading in `CHANGELOG.md`.
