<!-- SPDX-FileCopyrightText: 2026 ByteDance Ltd. and/or its affiliates -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Changelog

All notable changes to pjson are documented in this file. The format is based
on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and releases follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Introduced the versioned C++11 API for pjson 1.0, including compile-time
  version macros and `pjson::getVersion()`.
- Added strict RFC 8259 DOM and SAX parsing from strings, byte spans, and
  streams, with structured line/column diagnostics, duplicate-key policies,
  and configurable input, node, and nesting limits.
- Added configurable compact and pretty serialization, direct stream output,
  non-ASCII escaping, key-order selection, and output-size limits.
- Added non-mutating lookup and strict typed access, including negative array
  indexing, borrowed string views, type predicates, container queries, erase,
  clear, swap, and deep structural equality.
- Added RFC 6901 JSON Pointer lookup, atomic RFC 6902 JSON Patch, and atomic
  RFC 7396 Merge Patch with structured errors and resource budgets.
- Added a documented JSON Schema subset with local references, object and
  composition vocabularies, known string formats, collected errors, exact
  numeric comparisons, and configurable validation budgets.
- Added allocator-aware DOM construction, parsing, copying, and ownership with
  provenance-preserving deletion.
- Added relocatable CMake and pkg-config packages, the `pjson::pjson` target,
  Conan 2 and vcpkg recipes, and consumer installation tests.
- Added tutorials, runnable examples, migration guides, generated API
  documentation, comparative benchmarks, cross-platform CI, sanitizer and
  conformance testing, and four libFuzzer/OSS-Fuzz targets.
- Added security, release, versioning, licensing, governance, and contributor
  documentation, GitHub contribution templates, and REUSE licensing checks.

### Changed

- Parsing is now always RFC 8259-strict and rejects duplicate object keys by
  default; callers may explicitly keep the first or last duplicate.
- Integer and floating-point values now use `int64_t` and `double`
  representations and APIs instead of `int` and `float`.
- Compact serialization now emits no insignificant whitespace, while pretty
  serialization uses conventional nested indentation instead of the previous
  key-aligned format.
- Tree serialization, copying, equality comparison, destruction, and deep
  Merge Patch traversal now avoid recursive whole-tree walks.
- CMake builds repository tests, examples, and benchmarks by default only for
  top-level developer builds; `add_subdirectory()` consumers receive just the
  library unless they opt in.
- The public header is declaration-focused, with implementation helpers kept
  in the library source.

### Fixed

- Correctly escape JSON strings and object keys and decode JSON escapes,
  Unicode code points, and surrogate pairs so empty and escaped strings
  round-trip.
- Reject malformed, truncated, trailing-garbage, invalid UTF-8, invalid escape,
  invalid number, and out-of-range numeric input without exposing partial parse
  results.
- Correct negative array indexing and prevent pathological indexed access from
  causing unbounded array growth.
- Preserve numeric kind and round-trip finite binary64 values, and compare
  mixed integer/double values exactly beyond the binary64 exact-integer range.
- Correct schema Unicode-length counting, numeric-bound and `multipleOf`
  precision behavior, malformed keyword handling, format validation, and
  speculative combinator error reporting.
- Preserve destination state and structured errors on allocation, output-budget,
  and Patch/Merge Patch failures.

### Security

- Added bounded parser, serializer, Patch/Merge Patch, and schema-validation
  work to limit depth, memory and output amplification, reference traversal,
  regular-expression backtracking, and diagnostic growth for untrusted input.
- Added strict UTF-8 and escape validation, safe duplicate-key defaults, regex
  validation and caching, constant-space speculative validation, and atomic
  mutation behavior.

### Removed

- Removed the raw-pointer `CreateFromString()` parsing API in favor of
  allocator-aware `parse()` APIs returning `pjson::unique_ptr`.
- Removed mutable container exposure and coercive accessors: `PJSONARRAY`,
  `PJSONMAP`, `getArray()`, `getMap()`, `getInt()`, `getFloat()`, `getBool()`,
  `getString()`, `at()`, `getIfExist()`, and `getArrayValues()`.
- Removed the standalone JSON/Base64 encoding and decoding helpers.
- Removed `int`, `float`, C-string-vector, and corresponding vector convenience
  assignment/append overloads; use explicit `int64_t`, `double`, and supported
  vector types.
- Renamed the public type tags `jsonNumberFloat` and `jsonMap` to
  `jsonNumberDouble` and `jsonObject`.

## [0.0.3] - 2025-05-30

### Fixed

- Removed a redundant `lib` prefix from CMake target output names.

## [0.0.2] - 2025-05-30

### Added

- Generic JSON and Base64 encoding/decoding helpers.
- Object-key existence checks before value extraction.

## [0.0.1] - 2025-04-24

### Added

- Initial pjson source release.

[Unreleased]: https://github.com/Pico-Developer/pjson/compare/release-0.0.3...HEAD
[0.0.3]: https://github.com/Pico-Developer/pjson/compare/release-0.0.2...release-0.0.3
[0.0.2]: https://github.com/Pico-Developer/pjson/compare/release-0.0.1...release-0.0.2
[0.0.1]: https://github.com/Pico-Developer/pjson/releases/tag/release-0.0.1
