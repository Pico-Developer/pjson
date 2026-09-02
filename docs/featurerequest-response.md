<!-- SPDX-FileCopyrightText: 2026 ByteDance Ltd. and/or its affiliates -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Response to pjson Production-Readiness Requirements

This document responds to every requirement in
[`docs/featurerequest.md`](featurerequest.md). It records, for each item, a
disposition and the concrete work done (or the reason it was deferred or judged
not applicable). The requirements themselves are a well-constructed, largely
accurate audit; a small number rest on assumptions that did not match the
1.0.0 baseline, and those are called out explicitly.

Work landed in this pass targets the release now versioned **2.0.0** (the
unsigned-integer numeric model and the non-finite serialization default are
breaking changes, so the major version was bumped per SemVer). The unit suite
grew from 431 to 522 cases; all pass under normal Debug and Release builds and
under AddressSanitizer + UndefinedBehaviorSanitizer.

## Legend

- **Implemented** — done in this pass, with tests.
- **Partially implemented** — core of the requirement done; remainder scoped
  and noted.
- **Already satisfied** — the 1.0.0 baseline already met it; verified.
- **Deferred** — valid, but out of scope for this pass; tracked in `Todo.md`.
- **Not accurate / adjusted** — the requirement's premise did not hold against
  the baseline, or conflicts with a documented design choice; explained.

---

## 4. P0 correctness and safety

### PJSON-COR-001 — Preserve object keys byte-for-byte — Implemented
Confirmed defect A.1 was real: the `std::string` member/find/hasKey/erase paths
delegated through `c_str()`, so `"a"` and `"a\u0000b"` collided. The
`std::string` overloads are now the length-aware primary implementations
(`operator[]`, `find`, `hasKey`, `erase`, keyed `tryGet`); `const char*`
overloads keep documented NUL-terminated behavior. Pointer/Patch/equality/
serialization already operated on decoded `std::string` names and now preserve
these keys end to end. Regression matrix: `pjsontest/src/tests_embedded_nul.cpp`
(empty names; U+0000 at start/middle/end; parse round-trip; pointer + equality;
the documented `const char*` truncation contract).

### PJSON-COR-002 — Make aliasing mutations memory-safe — Implemented
Confirmed defect A.2 was real. Move assignment previously called `reset()`
before reading the source, freeing it when the source was a descendant. It now
snapshots the source's storage into a same-allocator temporary first, then swaps
(`pjson::operator=(pjson&&)`). `swap()` gained an ancestor/descendant guard
(`containsNode`) and rejects overlapping swaps as a safe no-op; internal
non-aliased swaps use a new `swapStorageUnchecked`/`_swapStorage` fast path.
Tests: `pjsontest/src/tests_aliasing.cpp` covers self copy/move, root-from-
descendant, descendant-from-root, sibling assigns, and root/descendant swap;
the whole suite passes under ASan/UBSan.

### PJSON-NUM-001 — Never silently corrupt an accepted number — Implemented
Confirmed defect A.3 was real (UINT64_MAX became `1.8446744073709552e+19`).
Added the `jsonNumberUInt` kind and full unsigned surface: `uint64_t`
assignment/append/vectors, `isUInt()`/`isInteger()`, `tryGet(uint64_t&)`,
`SaxHandler::onUInt`, exact signed/unsigned/double comparison
(`_compareNumbers` rewritten), and decimal serialization via `std::to_string`
without a `double` round-trip. Tokens in `[INT64_MIN, INT64_MAX]` stay signed;
`(INT64_MAX, UINT64_MAX]` are unsigned; an explicit `uint64_t` assignment keeps
unsigned identity even for small values. Tokens outside the exact range are
rejected by default (`ParseOptions::RejectUnrepresentableNumbers`) or, with
`AllowLossyNumbers`, stored as the nearest double. Tests:
`pjsontest/src/tests_numbers.cpp`, and both SAX/DOM front ends agree
(`tests_depth_frontends.cpp`).

### PJSON-NUM-002 — Handle non-finite floats explicitly — Implemented
The old behavior (stored NaN/Inf silently serialized as `null`) is replaced by
`SerializeOptions::NonFinitePolicy`. The default `RejectNonFinite` fails
serialization with a structured error (`toString` throws
`std::invalid_argument`; `write` sets `failbit`) identically for compact,
pretty, and streaming output. `NonFiniteToNull` restores the legacy mapping and
`NonFiniteToString` emits `"NaN"`/`"Infinity"`/`"-Infinity"`. Double formatting
remains locale-independent. Tests: `tests_numbers.cpp`
(`non_finite_serialization_policy`, `non_finite_stream_policy`).

### PJSON-NUM-003 — Define finite float conversion precisely — Implemented
Parsing uses the classic-locale standard-library conversion and rejects
overflow and nonzero-to-zero underflow by default; `AllowLossyNumbers` is the
explicit opt-in for underflow and out-of-range integers. Formatting tests
precisions from `digits10` through `max_digits10`, whose upper bound gives
bit-exact finite-double serialize/parse recovery on
conforming libraries. The active rounding-mode dependency is documented.
Halfway, subnormal, exponent-edge, negative-zero, 2^53-boundary, randomized
10,000-bit-pattern, and parser-front-end parity tests cover the contract.

### PJSON-SEC-001 — Make nesting limits stack-safe — Implemented
Confirmed defect A.4 was real: a large configured `maxDepth` still allowed
recursive DOM/SAX parsing to overflow. Configured depth is now clamped to a
proven-safe hard ceiling (`kParseDepthHardLimit`, 1024) that callers cannot
raise, applied uniformly in the DOM parser and both SAX parsers. A 100,000-deep
document with `maxDepth = INT_MAX` returns a structured resource-limit error
across all front ends. Tests: `tests_depth_frontends.cpp`; clean under ASan.

### PJSON-PARSE-001 — Keep parser front ends equivalent — Implemented (verified)
Added differential tests asserting the string, byte-span, DOM-stream,
buffered-SAX, and streaming-SAX front ends agree on acceptance, value/structure,
and rejection (including the new numeric-range and depth cases):
`tests_depth_frontends.cpp`. Sharing a single lexer core (PJSON-MAINT-001)
remains deferred; behavioral equivalence is now guarded by tests.

### PJSON-PARSE-002 — Apply duplicate-key policy early — Implemented
The DOM object parser now decodes the name, checks for a duplicate, and (under
`RejectDuplicateKeys`) fails at the duplicate key's own offset *before* parsing
or allocating its value subtree. Keep-first still grammar-checks the discarded
value. Comparison uses decoded, length-aware names. Tests:
`tests_error_model.cpp` (`duplicate_key_reported_early_at_key_offset`,
`duplicate_keep_first_still_validates_value`,
`duplicate_key_uses_decoded_length_aware_names`).

## 5. P1 core DOM and API

### PJSON-API-001 — Non-allocating traversal — Implemented
Added `forEachMember`/`forEachElement` (const and mutable) callback visitors
that iterate borrowed children directly, exposing a length-aware `StringView`
key and value reference with no per-key allocation or second lookup. Visitors
are function pointers with an opaque `void* ctx` (keeping the public header
declaration-only and ABI-stable); early stop is supported by returning `false`.
`keys()` remains as a convenience copy. Tests: `tests_dom_api.cpp`.

### PJSON-API-002 — Construction and mutation primitives — Implemented
Added `null()`/`object()`/`array()` factories, `operator=(std::nullptr_t)`,
`pushBack(const pjson&)` and `pushBack(pjson&&)`, `insertOrAssign` (copy and
move), and `reserve()`. Scalar/unsigned/vector assignment and append were
extended for `uint64_t`. Multi-step mutations retain the existing
build-then-swap strong-guarantee pattern. Tests: `tests_dom_api.cpp`.

### PJSON-API-003 — Separate safe reads from vivifying writes — Implemented
Added checked, non-vivifying `at(key)` and `at(index)` (throwing
`std::out_of_range`) and `contains()` alongside the existing non-vivifying
`find`/`hasKey`/`hasIndex`/`tryGet`. Positive `at(size_t)` uses `size_t`;
negative lookup stays on the separate signed `find(int)`/`tryGet(int, …)` API.
Mutable indexing now also has a `size_t` overload; valid negative `int` indexes
count from the end and an index before the beginning throws without mutation.
Tests: `tests_dom_api.cpp`, `tests_build.cpp`, and `tests_mutation.cpp`.

### PJSON-API-004 — Type conversion and equality — Implemented
`tryGet` conversions are exact: signed↔unsigned reads succeed only when
representable, integers widen to double, and no narrowing/precision-losing read
reports success. Cross-representation equality (`1 == 1u == 1.0`) is exact above
2^53 via the rewritten `_compareNumbers`. Object equality is order-independent.
The consolidated prose table enumerating every conversion is in the README
numeric/equality sections.

### PJSON-API-005 — Structured error model — Implemented
`ParseError` gained a stable `Code` enum (syntax, invalid encoding, duplicate
key, number range, depth/input/node limits, allocation failure, stream error,
callback error, invalid argument) set alongside the existing message and
byte/line/column. Serialization now also exposes non-throwing `SerializeError`
overloads with stable categories while retaining the existing convenience
exception/stream-state APIs. Tests: `tests_error_model.cpp`,
`tests_serialize_limits.cpp`.

### PJSON-API-006 — Ownership and allocator completeness — Implemented for the documented scope
The baseline already documents that the custom `Allocator` covers persistent
nodes and string/array/object wrapper objects, while standard-container backing
buffers and transient scratch use the standard allocator, and it is described as
exactly that (not a "complete DOM allocator"). Cross-allocator copy/move/swap
behavior, provenance-preserving deletion, and injected-failure invariants are
covered by `tests_allocator.cpp`. Routing every container's internal buffer
through the allocator is a larger design change left as a documented limitation.

### PJSON-API-007 — Document thread safety — Implemented (documentation)
pjson makes no positive concurrency guarantee beyond the C++ standard default:
distinct values may be used concurrently; a single value must not be mutated
concurrently with any other access; the default allocator's initialization is
thread-safe. This is now stated explicitly in the README thread-safety note. No
`ThreadSanitizer` job is added because no positive shared-object guarantee is
claimed.

## 6. P1 serialization

### PJSON-SER-001 — Valid and stable output — Implemented / already satisfied
Output is one valid RFC 8259 value with correct escaping and programmatic-UTF-8
validation; `toString()` and `write()` are byte-for-byte equivalent for the
same options; no framing bytes are appended. The output-size limit is now
verified overflow-safe at limit-1/limit/limit+1 for both APIs. Non-finite and
invalid-UTF-8 behavior is defined by policy. Tests:
`tests_serialize_limits.cpp`.

### PJSON-SER-002 — Deterministic output when requested — Already satisfied (verified)
Sorted (ascending/descending) bytewise key order is available and
deterministic; order does not affect structural equality. Verified by
`deterministic_key_order`. Canonical JSON is explicitly *not* claimed.

## 7. P1 resource and security

### PJSON-SEC-002 — Uniform, overflow-safe budgets — Already satisfied / extended
Parser, serializer, patch, and schema budgets exist with a documented "zero =
hard ceiling / unlimited" convention and checked arithmetic. This pass added the
depth hard-ceiling clamp (SEC-001) and kept the number-policy failures
distinguishable from malformed input via `ParseError::Code`.

### PJSON-SEC-003 — Transactional mutation — Already satisfied (verified)
Patch/Merge Patch remain atomic (build-scratch-then-swap), now using the safe
`_swapStorage` publication path. Move-into-descendant and move-root are
rejected. Covered by `tests_pointer_patch.cpp`.

### PJSON-SEC-004 — Regexes and external resources hostile — Already satisfied
Schema regex work is size-bounded and screened for catastrophic backtracking by
default (`trustedRegex()` to opt out). No API fetches a URL; remote `$ref` is
resolved only through an explicit application callback. pjson itself performs
no I/O, and document/byte/reference/work/depth budgets bound resolution.

## 8. Optional JSON Schema module

### PJSON-SCHEMA-000 — Strict fail-closed subset — Implemented
Added `pJsonSchemaValidator::Options::strict()` / `strictSubset`. In strict
mode, a standard validation/applicator keyword pjson does not enforce (e.g.
`contentSchema` or `$recursiveRef`) fails validation instead of being
ignored, while unknown non-standard extension keywords remain allowed as
annotations. Default remains permissive for compatibility. Tests:
`tests_schema_2020.cpp`.

### Schema module extracted to an external validator — Implemented
JSON Schema validation was moved out of `pjson` entirely into the standalone
`ByteDance::pJsonSchemaValidator` class (`<pjson_schema.h>` / `pjson_schema.cpp`).
It is a **pure consumer of pjson's public API** and touches no library
internals, so the core DOM no longer carries schema/regex state and the module
can later be packaged as a separately linked target. The former nested
`pjson::SchemaError` / `pjson::SchemaOptions` are now
`pJsonSchemaValidator::Error` / `pJsonSchemaValidator::Options`, and the
member `pjson::validate()` overloads are removed. Callers construct a validator
from a schema once and reuse it. A new public `pjson::tryCompareNumber()`
promotes the exact cross-kind numeric ordering the validator needs from a
former private helper. This also delivers the compiled/immutable validator
object requested by PJSON-SCHEMA-002.

### PJSON-SCHEMA-001 — Explicit dialect contract — Implemented for the subset
`pJsonSchemaValidator` now names its contract with
`documentedSubsetDialectUri()` and `documentedSubsetVocabularyUri()`.
`Options::defaultDialectUri` selects the dialect when `$schema` is absent; a
root `$schema` overrides it. Any unsupported declared/default dialect fails
schema compilation with a `SchemaCompilation` diagnostic. `$vocabulary` accepts
the pjson subset vocabulary, ignores unknown optional vocabularies, and rejects
unknown required vocabularies or malformed shapes. Callers inspect
`isSchemaValid()`, `schemaErrors()`, and `dialect()`. The official 2020-12 URI is
intentionally unsupported until pjson implements that complete dialect.

### PJSON-SCHEMA-002..006 — Substantially implemented / remaining dialect gaps
This pass materially expanded the validator toward 2020-12 by adding
`if`/`then`/`else`, `prefixItems`, `contains`/`minContains`/`maxContains`, and
`dependentSchemas` (fixing the A.5 conditional-schema gap), plus the strict
gate above, by extracting a reusable compiled validator object (SCHEMA-002),
and by adding the manifest-driven conformance gate (SCHEMA-006). SCHEMA-003/004
now add `$id` resource bases, anchors, dynamic references, explicit no-I/O
external resolution with document/byte/work/depth budgets, and annotation
propagation for both `unevaluated*` keywords. The official gate runs 1,287
Draft 2020-12 cases across 378 groups. Remaining gaps are standard meta-schema
loading/vocabulary-driven keyword selection and ECMA-262 Unicode property
escapes. Documentation therefore continues to describe this as a **documented
subset**, not general 2020-12 conformance.

PJSON-SCHEMA-002 strict keyword-shape compilation is implemented for the full
documented keyword set; permissive mode retains its compatibility behavior.
Standard meta-schema loading remains part of the broader 2020-12 work.

PJSON-SCHEMA-005 is implemented: errors distinguish schema compilation from
instance validation and provide stable fine-grained codes, separate instance
and schema locations, keyword names, and optional nested causes for failing
`anyOf` and zero-match `oneOf` branches. `Options::stopAfterFirstError` selects
first-error reporting; bounded multi-error collection remains the default, and
nested causes share the configured diagnostic bound.

Schema implementation utilities are now grouped into private value/numeric,
format, and URI translation units. The public surface remains the single
`pjson_schema.h` header and the implementation remains in the existing library
target; no redundant schema target was added.

## 9. Existing extensions

### PJSON-EXT-001/002/003 — Pointer / Patch / Merge Patch — Already satisfied
RFC 6901/6902/7396 behavior, atomicity, and structured errors were already
implemented and tested; embedded-NUL and aliasing fixes above strengthen them.
Re-verified by `tests_pointer_patch.cpp`.

## 10. P2 performance

### PJSON-PERF-001/002/003 — Partially satisfied; enforcement deliberately deferred
The benchmark now separately covers small/medium/large mixed documents, wide
objects, large arrays, string-heavy, escape-heavy, integer-heavy, floating-heavy,
and caller-supplied inputs. `--json`/`--bench-json` emits a versioned report with
source, compiler, flags, target, allocator disclosure, methodology, workload, and
raw-result metadata. CI retains baseline and cross-library reports for 30 days.

Hosted GitHub runners are not controlled performance machines, so these jobs do
not enforce universal timing thresholds. A stable runner and agreed per-case
baseline are prerequisites for a credible gate. Move timing, allocation counts,
peak RSS, binary/object size, and build-time measurements also remain separate
instrumentation projects rather than being mislabeled as operation latency. The
new unsigned path and traversal API avoid extra allocations/copies; further
PJSON-PERF-002 work should follow profiles rather than speculative redesign.

## 11. P2 build, packaging, portability

### PJSON-BUILD-001..005 — Already satisfied (verified)
The baseline is a well-behaved CMake subproject (namespaced `pjson::pjson`,
developer targets off when embedded), supports static/shared install and
build-tree consumers, ships relocatable CMake + pkg-config + Conan/vcpkg
recipes, publishes a CI platform matrix (GCC/Clang/AppleClang/MSVC), and keeps
optional features modular. Version fields were bumped to 2.0.0 across the header,
CMake, Conan, and vcpkg manifests (a configure-time mismatch is a hard error).

## 12. Verification

### PJSON-TEST-001..005 — Partially implemented / already satisfied
JSONTestSuite and the JSON-Schema-Test-Suite are pinned and wired; sanitizer,
differential, and fuzz jobs exist. This pass added the two mandatory regressions
(embedded-NUL access; ancestor/descendant move under sanitizers), dedicated
serialization, Pointer, and Merge Patch fuzz targets with 64 KiB input support,
and new differential front-end tests. Every compiled case remains individually
registered with CTest through post-link discovery from the executable's actual
test registry rather than source-text scraping. A manifest-driven
`draft2020-12` conformance gate
(`schema_official_draft2020_optional`) now runs alongside the existing draft-07
gate: supported-keyword files run whole, and each remaining unsupported group
(official meta-schema behavior and Unicode `\p{}` regex)
is skipped with a concrete reason so coverage cannot silently shrink. Measured
baseline: 1,287 Draft 2020-12 cases pass across 378 groups; four groups (10
cases) and one two-case meta-schema file are skipped. Full unconditional 2020-12
conformance remains unclaimed.

## 13. Documentation and governance

### PJSON-DOC-001..004 — Implemented
README, `CHANGELOG.md`, and `Todo.md` are updated for the new numeric model,
non-finite policy, error codes, traversal/factory/checked APIs, and schema
additions, and the 2.0.0 compatibility impact is called out (ABI break +
behavioral changes) per DOC-004. `SECURITY.md`/`GOVERNANCE.md` cover DOC-003.
`docs/behavioral-contract-2.0.md` is the single versioned contract for value and
numeric representation, strictness/budgets, error and exception boundaries,
mutation/invalidation, copy/move/allocator/aliasing behavior, serialization,
thread safety, and each optional standard's exact conformance scope.

## 14. Maintainability

### PJSON-MAINT-001/002 — Partially implemented
DOM and SAX now share numeric-token classification and conversion, including
integer kind and lossy overflow/underflow policy. Their remaining token scanning,
Unicode, and container control flow stays separate because streaming cursors and
DOM ownership have materially different needs; further unification remains
tracked. Schema validation is external to `pjson`, and stateless value/numeric,
format, and URI helpers now use focused private translation units behind the one
public `pjson_schema.h` surface.

## 15. P3 optional enhancements — Deferred
Insertion-order object storage, big-integer/decimal types, `string_view`
overloads, JSON Lines helpers, canonical JSON, and a pull-parser cursor remain
optional and out of scope; several are listed in `Todo.md`.

## 16–17. Delivery sequence and definition of done

Steps 1–6 of the requirement's own delivery order (the core correctness gate)
are complete: embedded-NUL keys, aliasing safety, exact unsigned integers, the
non-finite policy, stack-safe/equivalent front ends, and early duplicate
detection with structured diagnostics — each with a permanent regression test
and clean under ASan/UBSan. Step 7 (traversal, generic insertion, factories,
checked indexing) and the structured-error portion of step 6 are done. The full
JSON Schema 2020-12 module (step 10) is advanced but intentionally still labeled
a documented subset, and steps 9/11 (performance baselines, registry publishing)
plus the deferred items above remain open and tracked in `Todo.md`.
