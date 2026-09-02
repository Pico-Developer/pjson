<!-- SPDX-FileCopyrightText: 2026 ByteDance Ltd. and/or its affiliates -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Changelog

All notable changes to pjson are documented in this file. The format is based
on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and releases follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed

- **BREAKING (API):** JSON Schema validation is no longer a member of `pjson`.
  The `pjson::validate()` overloads and the nested `pjson::SchemaError` /
  `pjson::SchemaOptions` types are removed. Validation now lives in a standalone
  `ByteDance::pJsonSchemaValidator` class declared in the new `<pjson_schema.h>`
  header (built from `pjson_schema.cpp`). Compile a schema once —
  `pJsonSchemaValidator v(schema[, pJsonSchemaValidator::Options()]);` — then
  call `v.validate(instance[, errors])`. The former `SchemaError` and
  `SchemaOptions` are now `pJsonSchemaValidator::Error` and
  `pJsonSchemaValidator::Options`. The validator is a pure consumer of pjson's
  public API and touches no library internals, so the core DOM no longer links
  the schema/regex machinery. A new public `pjson::tryCompareNumber()` exposes
  the exact cross-kind numeric ordering the validator needs.
- Added an explicit schema-compilation contract. pjson now names its supported
  subset dialect/vocabulary, honors root `$schema`, supports a configurable
  default dialect, rejects unsupported dialects and required vocabularies, and
  exposes `isSchemaValid()`, `schemaErrors()`, and `dialect()`. Schema errors are
  categorized as `SchemaCompilation` versus `InstanceValidation`.
- Added a pinned, manifest-driven Draft 2020-12 conformance gate. After the
  reference and unevaluated-keyword work below, 1,245 supported cases run and
  52 cases are explicitly skipped with reasons so coverage cannot silently shrink.
- Added `$id` resource bases, `$anchor`, `$dynamicAnchor`, `$ref`, `$dynamicRef`,
  and an explicit function-pointer resolver. pjson performs no implicit I/O;
  resolution is bounded by reference, document, byte, work, and depth limits.
  `Options::modernSubset()` enables modern `$ref` sibling semantics while the
  default retains the prior Draft 7 behavior.
- Added Draft 2020-12 evaluation-annotation propagation and enforcement for
  `unevaluatedItems` and `unevaluatedProperties` across references, dynamic
  references, combinators, conditionals, `contains`, and container applicators.
  The official gate now executes 1,245 cases across 372 groups.
- Moved pJsonSchemaValidator storage behind a private implementation pointer;
  schemas are copied to the default allocator, removing dependence on the
  caller's schema allocator lifetime.

## [2.0.0] - 2026-08-31

This release responds to the external production-readiness requirements captured
in `docs/featurerequest.md`; see `docs/featurerequest-response.md` for a
per-requirement disposition. It contains correctness fixes, an ABI-breaking
numeric-model change, and new APIs, so it is a major version bump.

### Added

- Added an exact unsigned-integer representation (`jsonNumberUInt`): `uint64_t`
  assignment/append/vectors, `isUInt()`, `isInteger()`, `tryGet(uint64_t&)`, the
  `SaxHandler::onUInt(uint64_t)` event, and exact signed/unsigned/double
  comparison and decimal serialization without converting through `double`.
- Added a structured `ParseError::Code` category (syntax, invalid encoding,
  duplicate key, number range, depth/input/node limits, allocation failure,
  stream error, callback error, invalid argument) alongside the existing
  message and byte/line/column coordinates.
- Added non-allocating traversal: `forEachMember` and `forEachElement`
  (const and mutable) that visit borrowed children without copying keys.
- Added construction and mutation primitives: `null()`, `object()`, `array()`
  factories, `operator=(std::nullptr_t)`, `pushBack()` (copy and move),
  `insertOrAssign()`, `reserve()`, checked `at()` for keys and indices, and
  `contains()`.
- Added `SerializeOptions::NonFinitePolicy` (`RejectNonFinite` default,
  `NonFiniteToNull`, `NonFiniteToString`) governing NaN/infinity output.
- Added `ParseOptions::NumberPolicy` (`RejectUnrepresentableNumbers` default,
  `AllowLossyNumbers`) governing numbers outside the exact 64-bit and binary64
  ranges.
- Added JSON Schema Draft 2020-12 applicator keywords to the validator:
  `if`/`then`/`else`, `prefixItems`, `contains`/`minContains`/`maxContains`, and
  `dependentSchemas`, plus a strict fail-closed subset mode
  (`SchemaOptions::strict()` / `strictSubset`) that rejects unsupported standard
  keywords instead of ignoring them.

### Changed

- **BREAKING (API):** `parse()` and `parseStream()` now return a `pjson` value
  instead of `pjson::unique_ptr`; the `pjson::unique_ptr` typedef and
  `ValueDeleter` are removed. Detect failure with a `ParseError` out-param
  (`err.ok`) rather than a null check — the terse overloads return a JSON `null`
  value on failure. Move the returned value to transfer ownership. This removes
  the only smart pointer from the public API.
- **BREAKING (ABI):** `pjson::jsonType` gained `jsonNumberUInt` and the value
  storage grew a `uint64_t` member. Existing enumerator values are unchanged, but
  the class layout changed; dependents must be rebuilt against this header.
- **BREAKING (behavior):** integer tokens above `INT64_MAX` now parse to the
  exact unsigned representation (up to `UINT64_MAX`) instead of a lossy `double`.
  Tokens outside `[INT64_MIN, UINT64_MAX]`, and non-finite floating values, are
  now rejected by default; opt in with `ParseOptions::AllowLossyNumbers`.
- **BREAKING (behavior):** serializing a stored non-finite `double` now fails
  with a structured error by default instead of silently emitting `null`. Use
  `SerializeOptions::NonFiniteToNull` to keep the old behavior.
- Object key access, lookup, `hasKey`, `erase`, and keyed `tryGet` are now
  length-aware for `std::string`, preserving names containing embedded U+0000;
  `const char*` overloads keep documented NUL-terminated behavior.
- The parser now clamps a configured `maxDepth` to a stack-safe hard ceiling, so
  even an `INT_MAX` request cannot overflow the native stack.

### Fixed

- Fixed a heap-use-after-free in move assignment when the source aliased an
  ancestor or descendant of the destination; overlapping `swap()` is now a safe
  no-op.
- Rejected duplicate object keys are now reported immediately at the duplicate
  key's own offset, before its value subtree is parsed or allocated.

### Removed

- Removed the public `pjson::unique_ptr` typedef and `pjson::ValueDeleter`;
  parsing returns a `pjson` value. A `new pjson()` root is still freed by an
  ordinary `std::unique_ptr<pjson>` or by normal scope.

### Security

- Made configurable nesting limits memory-safe: excessive depth returns a
  structured resource-limit error rather than exhausting the stack, across the
  string, byte-span, DOM-stream, buffered-SAX, and incremental-SAX front ends.


## [1.0.0] - 2026-08-31

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
- Prevent schema-validation stack exhaustion by resolving consecutive local
  references iteratively and enforcing a conservative recursive-depth ceiling.
- Make relocatable pkg-config metadata generation portable to Windows and
  nested installation library directories.
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

[Unreleased]: https://github.com/Pico-Developer/pjson/compare/2.0.0...HEAD
[2.0.0]: https://github.com/Pico-Developer/pjson/compare/1.0.0...2.0.0
[1.0.0]: https://github.com/Pico-Developer/pjson/compare/release-0.0.3...1.0.0
[0.0.3]: https://github.com/Pico-Developer/pjson/compare/release-0.0.2...release-0.0.3
[0.0.2]: https://github.com/Pico-Developer/pjson/compare/release-0.0.1...release-0.0.2
[0.0.1]: https://github.com/Pico-Developer/pjson/releases/tag/release-0.0.1
