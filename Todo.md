# pjson — Production-Readiness Backlog

This file tracks only open work. Completed items are intentionally removed; use
the git history for their implementation details. FEAT-3 is intentionally
deferred: pjson will keep its current `std::map` object representation for now.

Current baseline: strict RFC 8259 parsing, bounded parser and schema resources,
JSON Pointer/Patch/Merge Patch, a documented default schema subset plus opt-in
required Draft 2020-12 vocabularies,
configurable serialization, allocator-aware DOM storage, non-vivifying typed
access, SAX streaming, individually registered tests, pinned conformance
corpora, libFuzzer/OSS-Fuzz targets, benchmarks, packaging, API reference, and
cross-platform CI.

## Resume notes (2026-09-03)

Current implementation commits on branch `featurerequest`:

- `2cfd60c` — full post-churn ABI, error handling, packaging, and documentation audit;
- `334cd70` — stable opaque ABI baseline and pjson 3.0 version transition;
- `3904d3f` — deduplicated private storage aliases;
- `9d2a070` — extracted `pJsonParser` and split core implementation;
- `abed3ba` — external, public-API-only `pJsonSchemaValidator`;
- `f0d6b5e` — manifest-driven Draft 2020-12 conformance gate;
- `abcd331` — explicit subset dialect and `$vocabulary` contract;
- `940c56b` — first `$id`/anchor/dynamic-reference and `unevaluated*` pass.
- `84b3eea` — audited compiled-schema ownership, budgets, and concurrency;
- `61e6995` — corrected negative mutable-index bounds;
- `6f1e6c9` — structured non-throwing serialization diagnostics;
- `921f09c` — actionable schema diagnostics and bounded nested causes;
- `772353e` — strict validation of supported keyword shapes;
- `5c8a68f` — seven-target fuzz coverage and inputs above 4 KiB;
- `2787433` — finite floating-point conversion hardening;
- `478bc97` — private schema utility module split;
- `ea16a8c` — shared DOM/SAX numeric conversion;
- `6203aff` — CTest discovery from the compiled registry.

The worktree was clean immediately after `2cfd60c`; nothing has been pushed.
Version 3.0.0 is the new ABI baseline (`PJSON_ABI_VERSION == 3`, shared-library
`SOVERSION == 3`). The stable public object shapes are:

- `pjson`: two pointers (`Allocator*` plus opaque `pjsonImpl*`);
- `pJsonParser`: one opaque implementation pointer;
- `pJsonSchemaValidator`: one opaque implementation pointer.

`pjsonImpl` owns the type tag, direct anonymous payload union, private
`ArrayStorage`/`ObjectStorage` aliases, and intrusive `_disposeNext` teardown link.
There is no named `Storage`, `_uValue`, `_pValueRaw`, `_allocatorOwnedNode`, or
parent pointer. Null values share an allocation-free process-lifetime sentinel;
each non-null value allocates its implementation with
`Allocator::ImplementationAllocation`. Deep destruction remains iterative and
allocation-free.

Important invariants now enforced:

- `pJsonParser` is a separate public helper in `<pjson_parser.h>`. Dependency
  direction is strictly parser to DOM core: `pjson.h`, `pjson.cpp`, and
  `pjson_internal.h` must not include or name parser types. Parser options,
  errors, SAX callbacks, and implementation state remain parser-owned.
- The implementation is decomposed into focused DOM, parser, serializer, JSON
  Pointer, JSON Patch/Merge Patch, and schema translation units while retaining
  the single installed `pjson::pjson` target.
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

The last complete contributor gate built Release and ASan/UBSan Debug, then
passed all 538 CTest checks in sanitized Debug (537 compiled C++ cases plus the
benchmark-tool regression suite). The current
Draft 2020-12 manifest explicitly accounts for all 80 files in the pinned
corpus. It executes 1,773 official cases across 437 groups with no selected-group
skips and explicitly defers 15 whole optional files. Those cover unsupported
big-number/cross-draft behavior and unimplemented format families.
Also verified: clang-format, clang-tidy, 20,000 schema-fuzzer runs, seven-target
libFuzzer smoke coverage with inputs above 4 KiB, Doxygen API
validation, relocatable static/shared CMake and pkg-config consumers, REUSE
licensing (211/211 files), GCC, and a direct ThreadSanitizer concurrency probe.
The 2026-09-03 full-churn audit also hardened move assignment and generic
insertion against ancestor/descendant aliasing, made `canSwap()` accurately
reject overlapping nodes without violating its `noexcept` contract, fixed
duplicate custom-meta-schema resource accounting, normalized relative URI dot
segments, and made benchmark comparison reject missing/duplicate/invalid rows.
A follow-up ABI audit added non-narrowing `findIndex(size_t)`, corrected SAX
allocation/stream error categories, made null construction explicitly `noexcept`,
and verified shared-import visibility through CMake, pkg-config, and Conan.
It also made parse-error publication best-effort during allocation failure, added
an rvalue-array-append overflow guard, removed stale inline-storage documentation,
and prevented formatting checks from traversing generated Conan build trees.

Additional audit evidence:

- all three installed public headers compile independently as C++11 with warnings
  as errors and hidden consumer visibility;
- a stricter `-Wconversion -Wsign-conversion -Wshadow` build is clean for project
  code (one warning remains inside the unchanged vendored Ryu source);
- shared CMake, pkg-config, and Conan consumers receive `PJSON_SHARED` and pass
  while compiled with hidden visibility;
- static and shared Conan package/test-package runs pass;
- exported-symbol inspection shows no `pjsonImpl`, `pJsonParserImpl`, or
  `pJsonSchemaValidator::Impl` symbols; and
- `git diff --check`, Python benchmark-tool tests, and public-header
  self-containment checks pass.

### Resume here

No release-blocking defect is known. Start by checking `git status` and the two
latest commits above. For further work, choose one of the open items below rather
than reopening the completed ABI migration. The highest-value remaining choices are:

1. finish optional asserted JSON Schema formats under `SCHEMA-2020`;
2. continue only incremental, independently testable parser deduplication under
   `MAINT-1`;
3. split stateful schema validation only where a cohesive private interface can
   preserve budgets, diagnostics, annotations, and reference scope (`MAINT-2`); or
4. establish controlled-runner performance thresholds before enabling a benchmark
   regression gate (`PERF-BASELINE`).

Keep `std::map` for object storage unless insertion-order semantics are deliberately
designed as a future major-version feature. Do not replace it with
`std::unordered_map`.

---

## From the production-readiness review (docs/featurerequest.md)

The core correctness gate (embedded-NUL keys, aliasing safety, exact unsigned
integers, non-finite policy, stack-safe/equivalent parser front ends, early
duplicate detection, structured error codes) shipped in 2.0.0. See
`docs/featurerequest-response.md` for the full per-requirement disposition. The
remaining, larger items are tracked here.

### [~] SCHEMA-2020 — Optional Draft 2020-12 vocabularies and extensions

**What is done:** `if`/`then`/`else`, `prefixItems`,
`contains`/`minContains`/`maxContains`, `dependentSchemas`, a strict
fail-closed subset mode (`pJsonSchemaValidator::Options::strict()`), a
compiled/immutable validator object: schema validation now lives in the external
`ByteDance::pJsonSchemaValidator` class (`<pjson_schema.h>` / `pjson_schema*.cpp`)
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
official Draft 2020-12 gate now explicitly accounts for all 80 pinned files. It
runs 1,773 cases across 437 groups with no selected-group skips and defers 15
whole optional files with concrete reasons.

Strict mode now performs a complete pre-validation pass over the documented
keyword set and rejects malformed keyword shapes before instance validation.

**What remains:** optional asserted formats not currently implemented: duration,
email/IDN email, hostname/IDN hostname, IRI/IRI-reference, JSON Pointer/relative
JSON Pointer, URI/URI-reference, and URI-template. Optional bignum and cross-draft
suites are outside pjson's explicit numeric/dialect contracts. Do not claim every
optional Draft 2020-12 behavior. The required 2020-12 vocabularies and standard
meta-schema compilation are implemented by `Options::draft2020()`; the legacy
default remains pjson's documented subset dialect.

**Implemented direction:** vocabulary activation is stored per compiled schema
resource. Official 2020-12 meta-schemas are bundled and pinned; custom meta-schemas
are loaded only through the explicit resolver and share document/byte budgets. Regex
is implemented with privately vendored, pinned SRELL
2026.06 under BSD-2-Clause. It passes the mandatory Unicode-property groups and the
optional ECMAScript, non-BMP, and regex-format suites. pjson retains pattern/subject
byte budgets and conservative safe-mode syntax checks; SRELL's finite work ceiling
is mapped to a resource-limit error.

### [~] PERF-BASELINE — Controlled regression policy and auxiliary metrics

The representative matrix and versioned machine-readable results are complete:
wide objects, large arrays, string/escape/integer/floating-heavy inputs, optional
corpora, source/build/environment/methodology metadata, and 30-day CI artifacts.
Hosted-runner numbers remain advisory. Before enforcing budgets, establish a
controlled runner, stable release baseline, and agreed per-case reporting
thresholds. Allocation counts, peak RSS, binary/object size, and build-time
measurements need separate platform/tooling protocols. Do not add them to the
latency table or treat a near-zero move operation as a useful microbenchmark.
The repository now provides `scripts/compare-benchmarks.py`, which rejects
environment mismatches by default and supports opt-in advisory or failing
thresholds, plus `scripts/benchmark-aux-metrics.py` for artifact sizes. Ryu
shortest conversion reduced same-machine floating-heavy serialization median by
about 88% while preserving all serialization and randomized bit-round-trip tests.

## Medium Priority

### [ ] MAINT-1 — Further unify DOM and SAX parser grammar code

**Where:** DOM parsing and SAX parsing currently use separate recursive-descent
implementations in `pjson_parser.cpp`, with differential conformance tests guarding
their behavior.

**Progress:** number grammar scanning plus token classification/conversion now use
shared internal routines across DOM and SAX.

**Why:** duplicated token scanning, Unicode, and container grammar logic raises
the chance that a future parser fix reaches only one API. The current paths are
well tested, so this is architectural debt rather than a release blocker.

**How:** incrementally extract the remaining shared lexer/parser operations
behind the existing buffer/stream cursors and DOM/event sinks. Preserve error
offsets, duplicate-key policies, resource budgets, streaming behavior, and the
DOM/SAX differential regression suite.

**Current disposition:** do not perform a wholesale rewrite. SAX has two cursor
types and callback/cancellation semantics while DOM has allocator-bound ownership
and transactional attachment. Forcing both through one state machine would replace
two tested paths at once. Continue extracting only independently testable lexical
operations when a defect or measured maintenance problem justifies the churn. The
number path is now fully shared behind buffer/stream adapters.

### [ ] MAINT-2 — Further split the stateful schema dispatcher

Stateless value/numeric, regex, format, URI, and dialect/vocabulary policy helpers
now live in focused private translation units. `validateCtx` still coordinates references, scalar
keywords, containers, combinators, annotations, and shared budgets. Extracting
those stateful families requires a shared private context interface and should
be done only with the official schema and resource-budget suites green after
each step.

**Current disposition:** the per-resource dialect/vocabulary context is now designed
and its stateless policy is extracted. Keep resolver ownership and the remaining
stateful dispatcher together: moving them would spread the same mutable budget,
diagnostic, annotation, reference-cycle, and dynamic-scope state across more files
without reducing coupling.

### [x] MAINT-3 — Keep implementation details out of the public DOM API

`pjson` is now a stable two-pointer handle: a borrowed allocator pointer plus an
opaque implementation pointer. A process-lifetime sentinel represents null without
allocation, preserving `noexcept` null and move construction as well as moved-from
allocator identity. Non-null private state uses the appended
`Allocator::ImplementationAllocation` category. Container types, scalar storage,
and the intrusive allocation-free destruction link live entirely in `pjsonImpl`.
`pJsonParser` is likewise a one-pointer PImpl. ABI generation 3 is pinned by
`PJSON_ABI_VERSION`, shared-library `SOVERSION`, layout assertions, explicit symbol
visibility, and the versioning contract.

### [ ] FEAT-3 — Preserve object key insertion order

**Where:** pjson currently stores objects in `std::map`, so serialization sorts
keys alphabetically.

**Why:** round-tripping that reorders keys creates noisy configuration and golden
file diffs. Most modern JSON DOMs preserve insertion order even though JSON
object semantics do not require it.

**How:** use an insertion-ordered representation, such as a vector of key/value
pairs plus a lookup index. Preserve structural equality semantics and retain
protection from hash-collision denial of service if a hash index is introduced.

**Current disposition:** do not replace `std::map` with `std::unordered_map`. That
would provide neither insertion order nor deterministic iteration and would add
collision-sensitive behavior for attacker-controlled keys. A correct implementation
needs an ordered sequence plus an index (or an audited ordered-map dependency), a new
`SerializeOptions` order choice while retaining ascending/descending output, explicit
key-view/traversal invalidation rules, and wide-object memory/lookup benchmarks. Treat
the storage/default-order decision as a deliberate major-version change unless the
new policy is fully opt-in.
