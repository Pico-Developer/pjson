# pjson — Production-Readiness Backlog

This file tracks only open work. Completed items are intentionally removed; use
the git history for their implementation details. FEAT-3 is intentionally
deferred: pjson will keep its current `std::map` object representation for now.

Current baseline: strict RFC 8259 parsing, bounded parser and schema resources,
JSON Pointer/Patch/Merge Patch, an expanded documented JSON Schema subset,
configurable serialization, allocator-aware DOM storage, non-vivifying typed
access, SAX streaming, individually registered tests, pinned conformance
corpora, libFuzzer/OSS-Fuzz targets, benchmarks, packaging, API reference, and
cross-platform CI.

## Resume notes (2026-09-01)

Current implementation commits on branch `featurerequest`:

- `abed3ba` — external, public-API-only `pJsonSchemaValidator`;
- `f0d6b5e` — manifest-driven Draft 2020-12 conformance gate;
- `abcd331` — explicit subset dialect and `$vocabulary` contract;
- `940c56b` — first `$id`/anchor/dynamic-reference and `unevaluated*` pass.

The pending worktree is the audited follow-up to `940c56b` and should be
committed as one polish/hardening change after the final checks. Important
invariants now enforced:

- `pJsonSchemaValidator` is a pure consumer of pjson's public API;
  `pjson_schema.cpp` must not include `pjson_internal.h` or access pjson storage.
- The public class uses a private `Impl*`; the root schema and all resolved
  documents are copied into default-allocator storage during construction.
- Resolver callbacks run only during construction. The callback and context are
  cleared from `options()` afterward; `validate()` performs no I/O or cache
  mutation and supports concurrent read-only use with separate error vectors.
- `Options::retrievalUri` supplies the base for a root without `$id`; relative
  external references without either base are compilation errors.
- `Options::modernSubset()` enables modern `$ref` sibling behavior and the
  Draft 2020-12 annotation-only default for `format`. Plain `Options` retains
  legacy Draft 7-compatible `$ref` replacement and format assertion behavior.
- Resource compilation indexes only schema-bearing keyword positions; objects
  inside `const`, `default`, `examples`, or extension annotations are data and
  must not register `$id` or anchors. Duplicate resource IDs/anchors, malformed
  anchors/references, unresolved references, resolver failures/exceptions, and
  document/byte/work/depth exhaustion fail schema compilation.
- `unevaluatedProperties`/`unevaluatedItems` use annotations only from successful
  branches. `anyOf` merges every successful branch, `oneOf` merges its sole
  successful branch, `not` discards outward annotations, and `if` annotations
  are retained only when `if` succeeds.

Authoritative verification commands:

```sh
PJSON_JSON_SCHEMA_TEST_SUITE_DIR="$PWD/.test-corpora/JSON-Schema-Test-Suite" \
  ctest --test-dir out/build-debug --output-on-failure
./build.sh --all --auto
```

The last complete Debug/ASan/Release runs passed 510/510 tests. The current
Draft 2020-12 manifest executes 1,287 official cases across 378 groups and skips
10 cases across four groups. The remaining groups require the official
meta-schema/custom vocabulary behavior or ECMA-262 Unicode property escapes.
Also verified: clang-format, clang-tidy, 20,000 schema-fuzzer runs, seven-target
libFuzzer smoke coverage with inputs above 4 KiB, Doxygen API
validation, relocatable static/shared CMake and pkg-config consumers, REUSE
licensing, GCC, and a direct ThreadSanitizer concurrency probe.

---

## From the production-readiness review (docs/featurerequest.md)

The core correctness gate (embedded-NUL keys, aliasing safety, exact unsigned
integers, non-finite policy, stack-safe/equivalent parser front ends, early
duplicate detection, structured error codes) shipped in 2.0.0. See
`docs/featurerequest-response.md` for the full per-requirement disposition. The
remaining, larger items are tracked here.

### [ ] SCHEMA-2020 — Finish remaining JSON Schema dialect gaps

**What is done:** `if`/`then`/`else`, `prefixItems`,
`contains`/`minContains`/`maxContains`, `dependentSchemas`, a strict
fail-closed subset mode (`pJsonSchemaValidator::Options::strict()`), a
compiled/immutable validator object: schema validation now lives in the external
`ByteDance::pJsonSchemaValidator` class (`<pjson_schema.h>` / `pjson_schema.cpp`)
that consumes only pjson's public API and is constructed once per schema, and a
manifest-driven `draft2020-12` conformance gate
(`schema_official_draft2020_optional`, SCHEMA-006) that runs the pinned
JSON-Schema-Test-Suite: supported-keyword files run whole and every deferred
feature is skipped with a concrete reason. An explicit dialect contract
(SCHEMA-001) names pjson's subset
dialect and vocabulary, honors root `$schema`, rejects unsupported dialects and
required vocabularies, and accepts unknown optional vocabularies. SCHEMA-003 and
SCHEMA-004 now provide `$id`/URI resources, `$anchor`, `$dynamicAnchor`, `$ref`,
`$dynamicRef`, an explicit resolver with document/byte/work/depth budgets, and
annotation propagation for `unevaluatedItems`/`unevaluatedProperties`. The
official Draft 2020-12 gate now runs 1,287 cases across 378 groups; it skips four
groups (10 cases) and one two-case meta-schema file with explicit reasons.

Strict mode now performs a complete pre-validation pass over the documented
keyword set and rejects malformed keyword shapes before instance validation.

**What remains:** full standard-vocabulary/meta-schema loading and ECMA-262
Unicode property escapes. The
remaining skipped official groups document these gaps. Until they land, docs
must keep saying "documented subset" and must not claim general 2020-12
conformance.

### [ ] PERF-BASELINE — Representative benchmarks and regression tracking

PJSON-PERF-001/002/003: expand the benchmark matrix (wide objects, large
arrays, string/escape/int/float-heavy), record environment metadata, and add
regression reporting on controlled runners before enforcing budgets.

### [ ] DOC-CONTRACT — Single consolidated behavioral contract (PJSON-DOC-001)

One versioned reference covering value representations and numeric boundaries,
strictness/limits, error/exception behavior per entry point, invalidation
rules, allocator/aliasing/thread-safety, and per-standard conformance scope.

## Medium Priority

### [ ] MAINT-1 — Further unify DOM and SAX parser grammar code

**Where:** DOM parsing and SAX parsing currently use separate recursive-descent
implementations in `pjson.cpp`, with differential conformance tests guarding
their behavior.

**Progress:** numeric token classification and conversion now use one internal
routine shared by DOM and SAX.

**Why:** duplicated token scanning, Unicode, and container grammar logic raises
the chance that a future parser fix reaches only one API. The current paths are
well tested, so this is architectural debt rather than a release blocker.

**How:** incrementally extract the remaining shared lexer/parser operations
behind the existing buffer/stream cursors and DOM/event sinks. Preserve error
offsets, duplicate-key policies, resource budgets, streaming behavior, and the
DOM/SAX differential regression suite.

### [ ] MAINT-2 — Further split the stateful schema dispatcher

Stateless value/numeric/regex, format, and URI helpers now live in focused
private translation units. `validateCtx` still coordinates references, scalar
keywords, containers, combinators, annotations, and shared budgets. Extracting
those stateful families requires a shared private context interface and should
be done only with the official schema and resource-budget suites green after
each step.

### [ ] FEAT-3 — Preserve object key insertion order

**Where:** pjson currently stores objects in `std::map`, so serialization sorts
keys alphabetically.

**Why:** round-tripping that reorders keys creates noisy configuration and golden
file diffs. Most modern JSON DOMs preserve insertion order even though JSON
object semantics do not require it.

**How:** use an insertion-ordered representation, such as a vector of key/value
pairs plus a lookup index. Preserve structural equality semantics and retain
protection from hash-collision denial of service if a hash index is introduced.
