# pjson Production-Readiness Requirements

Status: Proposed  
Baseline reviewed: pjson 1.0.0, commit 843930fbf2ec0ca6e2edc9fdc60aad6e27ed9cb6  
Scope: the standalone pjson library and its optional standards modules

## 1. Purpose

This document defines the correctness, safety, API, standards-conformance,
performance, testing, packaging, and maintenance requirements for pjson to be a
dependable general-purpose C++ JSON library. It is intentionally independent of
any particular downstream project, application, or protocol.

The requirements are observable contracts. Implementations may change as long
as the contracts and acceptance criteria remain satisfied.

The key words MUST, MUST NOT, SHOULD, SHOULD NOT, and MAY are to be interpreted
as described by RFC 2119 and RFC 8174.

## 2. Product goals

pjson should provide:

- strict and predictable RFC 8259 parsing and serialization;
- lossless handling of every value represented by its documented data model;
- safe processing of untrusted input under explicit resource budgets;
- a compact but complete DOM API for construction, inspection, traversal, and
  mutation;
- consistent behavior across DOM, SAX, string, byte-span, and stream APIs;
- optional standards modules whose conformance level is explicit and testable;
- portable build and package integration; and
- evidence-based performance and reliability claims.

The following are not required goals:

- being header-only;
- preserving source key order unless an explicit storage policy requests it;
- silently accepting malformed or implementation-defined JSON;
- implicit network access for external references; or
- being the fastest library on every workload.

## Normative references

The implementation and its conformance claims should be evaluated against the
published standards rather than another library's behavior:

- [RFC 8259 — The JavaScript Object Notation Data Interchange Format](https://www.rfc-editor.org/rfc/rfc8259)
- [ECMA-404 — The JSON Data Interchange Syntax](https://ecma-international.org/publications-and-standards/standards/ecma-404/)
- [RFC 6901 — JavaScript Object Notation Pointer](https://www.rfc-editor.org/rfc/rfc6901)
- [RFC 6902 — JavaScript Object Notation Patch](https://www.rfc-editor.org/rfc/rfc6902)
- [RFC 7396 — JSON Merge Patch](https://www.rfc-editor.org/rfc/rfc7396)
- [JSON Schema Draft 2020-12](https://json-schema.org/draft/2020-12/)
- [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html)

Where a standard permits implementation-defined behavior, pjson MUST document
its chosen behavior and test it consistently across all relevant APIs.

## 3. Priority definitions

| Priority | Meaning | Release rule |
| --- | --- | --- |
| P0 | Correctness, memory-safety, or silent-data-loss defect | Resolve before recommending the affected API for production use |
| P1 | Core capability needed for broad adoption | Resolve before declaring the corresponding feature complete |
| P2 | Performance, usability, portability, or ecosystem improvement | Track with measurable outcomes after P0/P1 |
| P3 | Optional enhancement | Implement when supported by demonstrated demand |

An optional component, such as JSON Schema, has its own release gate. The core
DOM may be production-ready without that component, but the project MUST NOT
claim conformance that the optional component has not achieved.

## 4. P0 correctness and safety requirements

### PJSON-COR-001: Preserve object keys byte-for-byte

JSON object names are strings and may contain embedded U+0000. Every API that
accepts a length-aware string MUST preserve the complete byte sequence and MUST
NOT route it through a NUL-terminated representation.

Required behavior:

- std::string lookup, insertion, assignment, tryGet, hasKey, and erase paths
  MUST use both the pointer and length.
- A string-view API MUST also be length-aware. For C++11, a library-defined
  view or a (const char*, size_t) overload is acceptable.
- const char* overloads MAY retain conventional NUL-terminated behavior, but
  this distinction MUST be documented.
- Parsed names "a" and "a\u0000b" MUST remain distinct.
- JSON Pointer escaping, lookup, Patch, Merge Patch, equality, copying, and
  serialization MUST preserve such names.

Acceptance tests MUST cover empty names and U+0000 at the beginning, middle,
and end of a name through const and mutable APIs. At minimum, parsing an object
containing both "a" and "a\u0000b" must allow the two values to be found,
updated, and erased independently.

### PJSON-COR-002: Make aliasing mutations memory-safe

Every public copy, move, assignment, swap, and mutation operation MUST have
defined behavior when its source aliases the destination, including when one
operand is an ancestor or descendant of the other. It MUST NOT cause
use-after-free, double-free, ownership cycles, leaks, or partially committed
state.

The implementation MAY complete an operation from a temporary snapshot or
reject an unsupported relationship before mutation. If rejection is chosen,
the API MUST expose it deterministically; an undocumented undefined-behavior
precondition is not acceptable. A noexcept API must use a non-throwing error
result, a documented safe no-op, or another explicit mechanism.

Acceptance tests MUST cover:

- self-copy and self-move;
- assigning a root from one of its descendants;
- assigning a descendant from its root;
- assignments between siblings;
- swapping a root and descendant;
- same-allocator and cross-allocator cases; and
- allocation failure during paths that take a defensive copy.

All cases MUST run under AddressSanitizer, UndefinedBehaviorSanitizer, and leak
checking. This pattern, in particular, must never access freed storage:

~~~cpp
pjson root;
root["child"]["value"] = std::int64_t{7};
pjson& child = root["child"];
root = std::move(child);
~~~

### PJSON-NUM-001: Never silently corrupt an accepted number

The library MUST define an explicit numeric model and preserve every value it
claims to represent. A syntactically valid integer token MUST NOT be silently
rounded into a different value merely because it is outside int64_t.

At minimum, the DOM and SAX APIs MUST support the complete int64_t and uint64_t
ranges exactly. The public API MUST provide:

- a distinct unsigned integer representation, such as jsonNumberUInt;
- assignment and construction from uint64_t;
- isUInt() and tryGet(uint64_t&);
- an unsigned SAX event, such as onUInt(uint64_t);
- array and vector insertion support for unsigned integers;
- exact signed/unsigned/double comparison semantics; and
- decimal serialization without conversion through double.

For backward compatibility, integer tokens from zero through INT64_MAX MAY
remain stored as signed integers. Tokens from INT64_MAX + 1 through UINT64_MAX
MUST be stored as unsigned integers. An explicit uint64_t assignment SHOULD
retain unsigned type identity even when its value is small.

Integer tokens outside the supported exact range MUST either:

1. be rejected with a structured out-of-range error; or
2. be preserved through an explicitly documented exact decimal or big-integer
   representation.

Lossy conversion to double MUST require an explicit opt-in policy.

Acceptance tests MUST include:

- INT64_MIN, INT64_MIN - 1, -1, 0, 2^53 - 1, 2^53, 2^53 + 1,
  INT64_MAX, INT64_MAX + 1, UINT64_MAX, and UINT64_MAX + 1;
- construction, parsing, SAX events, extraction, comparison, copying, moving,
  Patch test, schema numeric comparison, and serialization; and
- identical numeric classification across all parser front ends and arbitrary
  input chunk boundaries.

### PJSON-NUM-002: Handle non-finite floating-point values explicitly

JSON has no NaN or infinity values. A stored NaN or infinity MUST NOT silently
serialize as JSON null, because that changes both type and value while reporting
success.

The default policy MUST do one of the following:

- reject non-finite assignment; or
- retain the value in the DOM but make every serialization API fail with a
  structured error.

An explicit opt-in conversion policy MAY map non-finite values to null or
strings, but it MUST never be the implicit default. Compact, pretty, buffered,
and streaming output MUST follow the same policy.

Double formatting MUST be locale-independent and use
std::numeric_limits<double>::max_digits10 or a proven shortest-round-trip
algorithm rather than a hard-coded assumption about binary64 precision.

Tests MUST cover positive and negative infinity, quiet and signaling NaNs where
the platform provides them, negative zero, the smallest subnormal, the largest
finite value, and root and nested positions.

### PJSON-NUM-003: Define finite floating-point conversion precisely

Parsing a decimal JSON number into binary floating point is inherently a
conversion. The parser MUST document its supported floating-point domain and
MUST use a locale-independent, correctly rounded conversion where the platform
permits it.

Required behavior:

- finite values within the supported range MUST parse deterministically;
- overflow MUST fail with a structured numeric-range error;
- underflow that would silently change a nonzero token to zero MUST either fail
  by default or require an explicit lossy-conversion policy;
- the sign of negative zero MUST have a documented parse, equality, extraction,
  and serialization policy;
- serialization followed by parsing MUST recover the same finite double bits,
  except where a clearly documented normalization policy applies; and
- parsing and formatting MUST not depend on the process locale or rounding
  mode without explicitly documenting that dependency.

Tests MUST cover halfway cases, subnormals, exponent extremes, negative zero,
all rounding boundaries around 2^53, and randomized binary64 round trips on
every supported standard-library implementation.

### PJSON-SEC-001: Make nesting limits stack-safe

User-configurable resource limits MUST NOT allow callers to disable memory
safety. If a parser or tree algorithm is recursive, accepting an arbitrarily
large depth limit can exhaust the native stack.

The parser MUST either:

- use an iterative state machine whose nesting storage is heap-bounded; or
- clamp configured depth to a documented hard maximum proven safe on every
  supported platform.

The same rule applies to schema validation, equality, copying, destruction,
serialization, Pointer, Patch, and Merge Patch. Existing iterative algorithms
must remain iterative.

Acceptance tests MUST pass a very large requested depth, including INT_MAX,
then process deeply nested arrays and objects without stack overflow. DOM, SAX,
buffered-stream, and chunked-stream entry points MUST return a resource-limit
error under sanitizers rather than terminate the process.

### PJSON-PARSE-001: Keep all parser front ends behaviorally equivalent

The string, byte-span, DOM stream, buffered SAX, and incremental SAX APIs MUST
use the same JSON grammar and semantic policies. For equivalent input and
options, they MUST agree on acceptance, decoded values or events,
duplicate-key behavior, number classification, resource accounting, and the
first relevant error location.

Required edge cases include:

- empty input and every valid top-level scalar type;
- trailing JSON values, trailing non-whitespace bytes, and embedded NUL bytes;
- malformed and truncated UTF-8 at every byte boundary;
- escaped Unicode and surrogate pairs split across stream chunks;
- malformed numbers and very long number tokens;
- arrays and objects split at every possible one-to-four-byte boundary; and
- cancellation or exceptions from SAX callbacks.

DOM and SAX implementations SHOULD share a lexer/parser core to reduce future
behavioral drift.

### PJSON-PARSE-002: Apply duplicate-key policy early and consistently

Duplicate detection MUST compare decoded, length-aware names. Under the reject
policy, the parser SHOULD report a duplicate immediately after the second name
is decoded, before allocating or traversing its value subtree.

Under keep-first, the duplicate value MUST still be checked for valid JSON and
charged against input and work budgets, but the implementation SHOULD avoid
building an unused DOM subtree. Under keep-last, replacement MUST provide a
clear exception-safety guarantee. DOM and SAX behavior MUST be documented where
an event stream cannot retract an earlier value.

Tests MUST cover identical, escaped-equivalent, embedded-NUL, and nested names;
malformed duplicate values; large duplicate subtrees; and all three policies.

## 5. P1 core DOM and API requirements

### PJSON-API-001: Provide non-allocating traversal

The DOM MUST provide direct, non-owning traversal for arrays and objects without
copying every object name or performing a second lookup per member. Acceptable
designs include iterator and range types or callback-based visitors.

The API MUST provide:

- const array traversal;
- mutable array-value traversal;
- const object traversal exposing a length-aware key view and value reference;
- mutable object-value traversal without allowing in-place key corruption; and
- documented iterator and reference invalidation rules for insert, erase,
  clear, move, swap, and type-changing mutation.

keys() MAY remain as a convenience copy API. Tests and benchmarks MUST verify
that the direct traversal path performs no per-key allocations.

### PJSON-API-002: Complete construction and mutation primitives

The library SHOULD offer explicit, unambiguous ways to create and mutate each
JSON kind:

- null(), object(), and array() factories or equivalent tagged constructors;
- assignment from std::nullptr_t;
- scalar constructors and assignments for strings, booleans, signed integers,
  unsigned integers, and doubles;
- pushBack(const pjson&), pushBack(pjson&&), and an emplacement equivalent;
- object insert-or-assign operations accepting copied and moved pjson values;
- optional initializer-list factories with unambiguous object and array syntax;
  and
- reserve() for arrays and any object representation where reservation is
  meaningful.

Default construction MAY continue to mean JSON null. Callers must not need to
rely on default construction having an implicit object or array type.

All multi-step mutations MUST document and test their exception guarantee. A
failed allocation SHOULD leave the destination unchanged; where that is not
possible, the exact valid postcondition MUST be documented.

### PJSON-API-003: Separate safe reads from vivifying writes

Mutating operator[] MAY create missing nodes, but read-only access MUST NOT
mutate the document. The public API SHOULD include:

- find(key or index), returning a pointer or nullable view;
- contains(key) or hasKey(key), and hasIndex(index);
- checked at(key or index), with a documented exception or result type;
- strict tryGet functions that leave outputs unchanged on failure; and
- convenience getOr functions whose conversions are explicit and checked.

Positive array indexing SHOULD use size_t. If negative indexing remains, it
SHOULD use a separately named signed-index API. Out-of-range negative indexes
MUST NOT silently clamp to element zero.

### PJSON-API-004: Define type conversion and equality precisely

The documentation MUST define:

- which conversions are exact, widening, narrowing, or forbidden;
- whether 1, unsigned 1, and 1.0 compare equal;
- exact behavior above 2^53;
- negative-zero behavior;
- whether numeric type identity survives parse and serialization;
- object equality independent of storage or serialization order; and
- equality behavior for values using different allocators.

No narrowing or precision-losing tryGet operation may report success. Checked
conversion APIs MAY be supplied for callers that explicitly request narrowing.

### PJSON-API-005: Provide a structured error model

Human-readable messages are useful but insufficient as the only machine-facing
error contract. Parsing and serialization SHOULD expose stable error categories
in addition to text. At minimum, distinguish:

- syntax error;
- invalid UTF-8 or escape;
- duplicate key;
- numeric overflow, underflow, or unsupported exact number;
- depth, input-byte, node, work, and output-byte limits;
- allocation failure;
- stream read or write failure;
- callback cancellation or exception; and
- invalid API argument.

Parse diagnostics MUST retain byte offset, one-based line, and documented
column semantics. A non-throwing serialization overload SHOULD return a result
or populate a SerializeError; callers should not need to infer the cause from
ostream failbit.

### PJSON-API-006: Make ownership and allocator behavior complete

If the library advertises allocator-aware storage, the contract MUST state
exactly which allocations use the supplied allocator. Prefer routing all
persistent DOM allocations through it, including node objects, strings, object
names, and array and object backing storage. Otherwise, describe the feature as
a node allocator rather than a complete DOM allocator.

Required guarantees:

- an allocator outlives every value bound to it;
- destruction always uses the originating allocator;
- cross-allocator copy, move, and swap behavior is explicit;
- parsed ownership cannot be detached and deleted incorrectly;
- failure injection at every persistent allocation site leaves a valid tree;
  and
- iterative destruction remains safe for very deep documents.

### PJSON-API-007: Document thread safety

The project MUST state whether:

- separate values can be used concurrently;
- one immutable value can be read concurrently;
- mutation requires exclusive synchronization;
- custom allocators must provide their own synchronization; and
- global or default allocator and version functions are initialization-safe.

Any positive thread-safety guarantee MUST have a ThreadSanitizer test.

## 6. P1 serialization requirements

### PJSON-SER-001: Guarantee valid and stable JSON output

Every successful serializer MUST emit exactly one valid RFC 8259 JSON value. It
MUST correctly escape values and names, validate programmatically supplied
UTF-8, and never append framing bytes such as a newline unless explicitly
requested.

The contract MUST specify:

- compact versus pretty output;
- key-order policy;
- Unicode and solidus escaping policies;
- floating-point formatting;
- behavior for invalid UTF-8 and non-finite values;
- maximum output size; and
- whether a stream failure can leave partial output.

toString() and streaming write() MUST be semantically equivalent for the same
options. Output-size checks MUST be overflow-safe and tested at limit minus one,
the exact limit, and limit plus one.

### PJSON-SER-002: Preserve deterministic output when requested

The library MUST provide a deterministic object-key order. Sorted bytewise
order is sufficient and matches the current representation. If insertion-order
storage is added, callers MUST still be able to request sorted output.

Canonical JSON is a separate feature and MUST NOT be claimed unless all rules
of a named canonicalization specification are implemented and tested.

## 7. P1 resource and security requirements

### PJSON-SEC-002: Use uniform, overflow-safe resource budgets

Every operation that can scale with untrusted input SHOULD accept or inherit an
explicit budget. Applicable limits include:

- input bytes;
- nesting depth;
- materialized nodes;
- decoded string and name bytes;
- number-token length;
- total parser work;
- serialized output bytes;
- Patch operations, cloned nodes and bytes, and pointer traversal;
- schema depth, reference resolutions, regex work, validation work, and error
  count; and
- stream token buffering.

All size arithmetic MUST be checked before addition or multiplication. A zero
limit MUST have one consistent documented meaning; it must not mean unlimited
for one budget and use the hard ceiling for another without an explicit type or
name distinguishing those policies.

Resource-limit failures MUST be distinguishable from malformed input and
allocation failure. Defaults MUST be finite and suitable for untrusted input.
Applications MAY explicitly opt into larger limits, subject to stack-safe hard
ceilings.

### PJSON-SEC-003: Preserve transactional mutation guarantees

Patch and Merge Patch MUST remain atomic: syntax, lookup, failed test, budget,
and allocation failures leave the original target unchanged. Other compound
mutations SHOULD offer the strong exception guarantee.

Pointer and Patch implementations MUST handle deeply nested and adversarial
paths without integer overflow or unbounded recursion. Move operations MUST NOT
create ownership cycles.

### PJSON-SEC-004: Treat regexes and external resources as hostile

Any regex-processing feature MUST bound both pattern and subject work or use an
engine with a reliable complexity guarantee. Disabling protections MUST require
an explicit trusted-input option.

No API may fetch a URL merely because input contains one. Optional external
resource resolution MUST be callback-driven and disabled by default, with
caller-controlled scheme and host allowlists, byte limits, timeouts, redirect
policy, recursion limits, and caching.

## 8. Optional JSON Schema module requirements

JSON Schema is not required for a useful JSON DOM. However, if pjson advertises
general JSON Schema support rather than a named subset, the following are
requirements. Keeping this functionality in an optional pjson-schema target is
encouraged so the core library remains small.

### PJSON-SCHEMA-000: Make subset validation fail closed when requested

Even without full dialect support, the schema component MUST provide a strict
subset mode suitable for validation boundaries. In that mode it MUST reject:

- unsupported standard validation or applicator keywords;
- malformed values for supported keywords;
- unresolved or unsupported references; and
- a declared dialect or required vocabulary it cannot implement.

It MAY allow unknown extension keywords as annotations under an explicit
policy. Permissive behavior that ignores unsupported constraints MAY remain
available for backward compatibility, but it MUST be clearly named, documented,
and opt-in for new code. A caller must be able to determine whether every
validation-relevant part of a schema was understood before trusting the result.

### PJSON-SCHEMA-001: Implement an explicit dialect contract

The schema API MUST accept a default dialect option and honor $schema when
present. It MUST support JSON Schema Draft 2020-12 completely before claiming
2020-12 conformance. Additional dialects, such as Draft 7, MAY be supported.
Unsupported dialects and required vocabularies MUST produce a clear error.

Unknown extension keywords must be handled according to the selected dialect;
they must not be confused with unsupported required vocabularies.

### PJSON-SCHEMA-002: Compile and validate schemas separately

Provide a compiled, immutable schema object. Compilation MUST:

- validate the schema against the appropriate meta-schema when strict schema
  checking is enabled;
- reject malformed shapes for known keywords in strict mode;
- resolve identifiers, anchors, dynamic anchors, and references;
- detect invalid reference graphs and enforce reference and work budgets; and
- avoid repeating compilation for every instance validation.

Compiled schemas SHOULD be safe for concurrent validation when callers use
separate diagnostic sinks.

### PJSON-SCHEMA-003: Cover the Draft 2020-12 vocabulary

The implementation MUST cover the applicable 2020-12 Core, Applicator,
Validation, Unevaluated, and Metadata vocabularies, including at least:

- $schema, $id, $vocabulary, $defs, $anchor, $dynamicAnchor, $ref,
  $dynamicRef, and $comment;
- allOf, anyOf, oneOf, not, if, then, and else;
- prefixItems, items, contains, minContains, and maxContains;
- properties, patternProperties, additionalProperties, propertyNames, and
  dependentSchemas;
- unevaluatedItems and unevaluatedProperties;
- type, enum, const, multipleOf, numeric bounds, string lengths and patterns,
  array size and uniqueness, object size, required, and dependentRequired; and
- annotations such as title, description, default, deprecated, readOnly,
  writeOnly, and examples.

Format annotation and assertion behavior MUST be selectable and documented.
Supported formats MUST be listed individually. Content vocabulary and
nonstandard formats MAY be optional, but unsupported behavior must be explicit.
Regular-expression behavior MUST follow the dialect's required ECMA-262 model
closely enough to pass its official tests; using a platform regex engine is not
by itself evidence of compatibility.

### PJSON-SCHEMA-004: Make reference resolution secure and embeddable

Local fragment and URI resolution MUST follow the selected JSON Schema dialect.
Remote references MUST never trigger implicit network access. Applications MAY
provide a resolver callback that returns schema bytes or DOM values. Resolution
MUST enforce cycle detection, depth, document-count, total-byte, and work
limits.

Failure to resolve a required reference MUST fail compilation or validation; it
MUST NOT silently make the schema permissive.

### PJSON-SCHEMA-005: Provide actionable diagnostics

Each schema compilation or validation error SHOULD include:

- a stable error code;
- instance location as a JSON Pointer;
- schema or keyword location as a URI or JSON Pointer;
- keyword name;
- human-readable message; and
- nested causes for combinators when requested.

Callers MUST be able to choose first-error or bounded multi-error collection.
Diagnostic collection itself must respect an error-count and memory budget.

### PJSON-SCHEMA-006: Prove conformance

The full applicable JSON-Schema-Test-Suite Draft 2020-12 corpus MUST run in CI.
Skipped groups and deliberate deviations MUST be machine-readable, reviewed,
and published. A missing corpus must fail release CI rather than produce a
successful skip.

Tests MUST also cover malformed schemas, vocabulary negotiation, reference
cycles, external resolver failures, regex limits, validation budgets, Unicode
length, exact mixed numeric comparisons, and boolean schemas.

Until these requirements are met, documentation and package metadata MUST say
documented JSON Schema subset, name the supported keyword set, and prominently
state that unknown or unsupported constraints may be ignored.

## 9. Existing extension requirements

### PJSON-EXT-001: JSON Pointer

JSON Pointer behavior MUST conform to RFC 6901 for string and URI-fragment forms
if both are exposed. Tests MUST include empty tokens, ~0, ~1, embedded NUL,
non-ASCII names, invalid escapes, large indices, leading-zero indices, and the
array dash token where applicable. Lookups MUST be non-vivifying.

### PJSON-EXT-002: JSON Patch

JSON Patch behavior MUST conform to RFC 6902. All operations must be atomic as
a document, and test MUST use the documented structural and numeric equality
rules. Tests MUST cover root replacement and removal, same-array moves,
descendant moves, invalid paths, duplicate members in the patch document,
budget failure, and allocation failure.

### PJSON-EXT-003: JSON Merge Patch

JSON Merge Patch behavior MUST conform to RFC 7396, including root replacement,
null member deletion, and wholesale array replacement. Deep patches must be
stack-safe and atomic under allocation or budget failure.

## 10. P2 performance requirements

### PJSON-PERF-001: Maintain representative benchmarks

Benchmarks MUST separately measure parse, compact serialization, traversal,
copy, move, and allocation behavior for:

- small request and response documents;
- medium nested documents;
- large documents;
- wide objects;
- large arrays;
- string-heavy and escape-heavy data;
- integer-heavy and floating-point-heavy data; and
- optional caller-supplied real-world corpora.

Comparison runs SHOULD include current releases of several established DOM
libraries, including at least one feature-rich implementation and one
performance-oriented implementation. Results MUST record commit, compiler,
flags, architecture, operating system, allocator, input sizes, and methodology.

No performance claim should rely on a single machine, best-case sample, or one
workload. Median latency, throughput, peak resident memory, allocation count,
compiled object size, final binary size, and clean and incremental compilation
time SHOULD be reported separately.

### PJSON-PERF-002: Avoid avoidable work in common DOM operations

The common paths SHOULD support:

- one-lookup object access;
- traversal without copied key lists;
- moved child insertion without deep copying;
- array capacity reservation and amortized append;
- direct serialization to a caller-provided sink;
- schema compilation reuse; and
- parsing from byte spans without an intermediate NUL-terminated copy.

Performance changes MUST preserve all safety budgets and MUST be checked by
correctness tests and sanitizers.

### PJSON-PERF-003: Track regressions without overclaiming

CI SHOULD retain historical benchmark artifacts or compare against the last
stable release on controlled runners. Initially, regressions larger than an
agreed threshold should produce a report rather than a flaky pass or fail.
Once runner stability is demonstrated, release gates MAY enforce per-workload
budgets.

## 11. P2 build, packaging, and portability requirements

### PJSON-BUILD-001: Be a well-behaved CMake subproject

The project MUST export a namespaced target such as pjson::pjson and MUST NOT
modify parent-wide compiler flags, warning levels, language standards,
BUILD_TESTING, or unrelated cache variables when included with
add_subdirectory() or FetchContent. Developer-only tests, examples, benchmarks,
documentation, fuzzers, and install rules MUST default off when the project is
embedded.

The minimum CMake version SHOULD be no higher than required by the library
implementation. CMake 3.15 compatibility is a useful portability target. If a
newer minimum remains necessary, the exact feature requiring it MUST be
documented and direct-source integration must remain supported.

### PJSON-BUILD-002: Support static and shared consumption correctly

Static and shared builds MUST work through build-tree and installed-package
usage. Public symbol visibility and export macros SHOULD be explicit rather
than depending solely on automatic Windows symbol export. Position-independent
code, runtime-library selection, and debug and release configuration handling
MUST behave correctly on supported platforms.

Installed CMake and pkg-config metadata MUST be relocatable, contain no source
or build paths, and expose only actual consumer dependencies.

### PJSON-BUILD-003: Publish immutable package inputs

Release tags MUST be immutable and resolve to reviewed commits. Release source
archives and artifacts MUST include checksums. Package recipes SHOULD use an
immutable tag or commit plus a cryptographic hash rather than building a mutable
checkout.

Official or documented recipes SHOULD cover Conan and vcpkg once their registry
submission and maintenance status are clear. Static and shared package
consumers MUST be built and executed in CI.

### PJSON-BUILD-004: Define the supported platform matrix

The project MUST publish its supported combinations of operating system,
architecture, compiler, standard library, C++ language level, and build type.
CI MUST exercise every combination claimed as supported or clearly distinguish
fully tested platforms from best-effort platforms. At minimum, the expected
general-purpose matrix is:

- GCC and Clang on Linux;
- AppleClang on macOS;
- MSVC on Windows;
- x86-64 and arm64 where hosted runners are available; and
- Debug and optimized Release builds.

If MinGW, 32-bit targets, Android, unusual double formats, or big-endian targets
are claimed, they require corresponding CI or periodic verification.

### PJSON-BUILD-005: Keep optional features modular

The RFC 8259 parser, serializer, and DOM SHOULD remain usable without JSON
Schema, regular-expression, networking, or benchmark dependencies. Optional
standards modules SHOULD have separate targets and headers with explicit
dependency and version contracts. Disabling an optional module MUST remove its
code and transitive dependencies from consumer builds.

## 12. P1 and P2 verification requirements

### PJSON-TEST-001: Keep conformance corpora mandatory for releases

Release CI MUST fetch commit-pinned and integrity-verified conformance corpora
and fail if they are absent, empty, at the wrong revision, or produce an
unexpected case count. A local developer build MAY skip unavailable optional
corpora, but the skip must be conspicuous.

Required suites include:

- JSONTestSuite for parser acceptance and rejection;
- JSON-Schema-Test-Suite for every claimed schema dialect and vocabulary; and
- maintained RFC 6901, RFC 6902, and RFC 7396 cases for extension APIs.

Accepted implementation-defined parser cases SHOULD be checked for structural
equality and stable reserialization, not only successful reparsing.

### PJSON-TEST-002: Run differential and property tests

The project SHOULD maintain tests for:

- DOM versus SAX acceptance and value or event equivalence;
- string versus byte-span versus stream parsing;
- compact output reparsing to structural equality;
- stream output matching buffered output;
- copy, move, and swap invariants;
- Patch and Merge Patch atomicity;
- allocator provenance and injected allocation failure; and
- comparisons against one or more mature JSON implementations on the common,
  standards-defined subset.

Differences from comparison libraries MUST be placed in a small reviewed
allowlist with a reason and an expiry or review condition.

### PJSON-TEST-003: Strengthen fuzzing

Maintain separate fuzzers for DOM parsing, SAX and stream parsing,
serialization, Pointer and Patch, Merge Patch, and schema compilation and
validation. Fuzz invariants SHOULD include:

- no crash, leak, undefined behavior, or unbounded work within configured
  limits;
- DOM and SAX acceptance parity;
- parse of serialize producing structural equality for representable values;
- equivalent buffered and chunked-stream behavior;
- failed transactional operations leaving inputs unchanged; and
- diagnostics remaining within configured budgets.

Smoke fuzzing MUST include inputs larger than 4 KiB as well as targeted seeds
for Unicode boundaries, embedded NUL, duplicate names, long numbers, deep and
wide containers, output limits, and aliasing mutations. Prefer active
continuous hosted fuzzing; otherwise run scheduled sustained fuzz jobs and
retain and minimize all findings.

### PJSON-TEST-004: Require sanitizers and static analysis

Every release candidate MUST pass the complete unit and conformance suite with:

- AddressSanitizer;
- UndefinedBehaviorSanitizer;
- leak detection on a supported platform; and
- compiler warnings treated as errors for project sources.

ThreadSanitizer SHOULD cover any documented concurrent-use guarantees. Memory
Sanitizer SHOULD only be claimed when the whole relevant dependency graph is
instrumented. If a sanitizer option is unsupported by the selected toolchain,
configuration MUST fail clearly rather than silently ignoring the request.

Static analysis and CodeQL SHOULD remain enabled. Findings must be triaged, and
release criteria must require zero unresolved high-severity correctness or
security findings.

### PJSON-TEST-005: Add regression tests for every defect

Every correctness or security fix MUST first gain a minimal reproducer and then
retain it as a permanent test. The two initial mandatory regressions are:

1. length-preserving access to an embedded-NUL object name; and
2. ancestor and descendant move assignment under sanitizers.

The test suite must register every compiled case with the test runner. Release
CI SHOULD compare discovered and registered counts to prevent silent omission.

## 13. P2 documentation and API-governance requirements

### PJSON-DOC-001: Publish one precise behavioral contract

Versioned documentation MUST define:

- every JSON value representation and numeric boundary;
- parsing strictness, duplicate handling, and resource-limit defaults;
- error and exception behavior for each entry point;
- construction, auto-vivification, and null semantics;
- iterator, pointer, reference, and string-view invalidation;
- copy, move, swap, allocator, and aliasing behavior;
- serialization ordering, escaping, and numeric formatting;
- thread-safety guarantees; and
- exact conformance scope for every optional standard.

Examples MUST use safe, non-vivifying APIs for reads and must not depend on
undocumented behavior.

### PJSON-DOC-002: Maintain compatibility and migration guidance

Semantic Versioning MUST cover documented source and behavioral contracts. If
ABI stability is not promised, documentation must state that consumers should
rebuild the library and dependents together.

For each release, publish:

- added, changed, deprecated, removed, fixed, and security-relevant behavior;
- migration notes for behavior changes;
- supported compiler and platform matrix;
- conformance-suite revisions and results; and
- benchmark methodology and comparison caveats.

Changes to enum values, object ordering, number classification, duplicate-key
defaults, exception behavior, or serialization are compatibility changes and
must be versioned deliberately. Existing enum numeric values SHOULD NOT be
renumbered when an unsigned kind is added.

### PJSON-DOC-003: Keep security and maintenance expectations explicit

Maintain a private vulnerability-reporting path, supported-version table,
response targets, and coordinated-disclosure policy. Repository governance must
identify active maintainers and the process for reviewing significant API,
security, or compatibility changes.

### PJSON-DOC-004: Classify compatibility impact before implementation

Each requirement must be assigned a release-compatibility impact before its
implementation is merged:

| Change class | Typical impact |
| --- | --- |
| Fix incorrect lookup or memory-unsafe behavior | Patch release, with regression tests |
| Add new overloads, factories, traversal, or structured errors | Minor release when source-compatible |
| Add a numeric variant that changes class layout | ABI break; require dependent binaries to rebuild and document it prominently |
| Change duplicate-key defaults, non-finite handling, numeric classification, or serialized spelling | Behavioral compatibility change; provide migration notes and use the SemVer level required by the published contract |
| Add a separate optional schema module | Minor release when it does not alter core behavior |
| Change object storage or iteration invalidation rules | Potential source, behavioral, and ABI break; require an explicit migration plan |

The project MUST maintain tests for supported old behavior during deprecation
windows. A compatibility mode must have a removal version or review milestone
rather than becoming an undocumented permanent branch.

## 14. P2 maintainability requirements

### PJSON-MAINT-001: Share parser machinery

DOM and SAX parsing currently have separate grammar implementations. They
SHOULD share tokenization, Unicode decoding, number classification, duplicate
handling, resource accounting, and error-location logic through a common core
parameterized by a DOM builder or event sink.

The refactor MUST preserve public diagnostics and pass differential tests after
each stage. It must not turn the streaming SAX path into a whole-document
buffering implementation.

### PJSON-MAINT-002: Isolate standards extensions and complex subsystems

Schema validation, Pointer, Patch, Merge Patch, parsing, serialization, and DOM
storage SHOULD have clear internal module boundaries rather than accumulating
in a single implementation unit. Shared safety budgets and allocator rules must
remain centralized enough to prevent divergent enforcement.

The split SHOULD reduce review and incremental-build cost without exposing
private implementation types or weakening the single public contract.

## 15. P3 optional enhancements

The following are valuable but are not prerequisites for a robust core DOM:

- insertion-order-preserving object storage as a selectable policy;
- an exact arbitrary-precision integer or decimal type;
- user-defined type-conversion traits;
- JSON Lines or JSON Text Sequence helpers built above the single-document
  parser;
- canonical JSON for a specifically named standard;
- zero-copy or immutable document views;
- C++17 std::string_view overloads in addition to the C++11 API; and
- a pull-parser or cursor API between SAX and a fully materialized DOM.

Each optional feature must retain the same input validation, resource budgets,
diagnostics, and sanitizer and fuzz requirements as the core APIs.

## 16. Delivery sequence

The recommended implementation order is:

1. Fix embedded-NUL key handling and add its regression matrix.
2. Fix ancestor and descendant move and swap safety and add sanitizer tests.
3. Add exact unsigned integer support and define the unrepresentable-number
   policy.
4. Replace silent non-finite-to-null serialization with an explicit policy.
5. Make every parser front end stack-safe and behaviorally equivalent.
6. Align early duplicate detection and structured diagnostics.
7. Add direct traversal, generic child insertion, factories, checked indexing,
   and serialization-result APIs.
8. Complete allocator coverage and document thread safety.
9. Establish performance baselines and optimize only with correctness gates in
   place.
10. Implement full JSON Schema 2020-12 as a separately gated module, or retain
    the accurately documented subset designation.
11. Harden package and release provenance and publish supported registry
    packages.

Steps 1 through 6 constitute the core correctness gate. Step 10 is independently
required before claiming JSON Schema 2020-12 compatibility. The strict,
fail-closed subset mode in PJSON-SCHEMA-000 should be delivered before expanding
the subset or beginning the full-dialect implementation.

## 17. Definition of done

The production-readiness effort is complete when all of the following are true:

- every P0 requirement has a permanent regression test and passes sanitizers;
- supported integer values round-trip exactly across DOM and SAX APIs;
- valid object names are never truncated by a length-aware API;
- no public mutation operation can trigger undefined behavior through a
  supported aliasing pattern;
- excessive depth or work returns a structured limit error rather than
  crashing;
- all parser front ends agree on the pinned JSON conformance corpus;
- successful serialization never silently changes a stored value's JSON type;
- non-allocating object and array traversal is available;
- error codes, limits, invalidation, ownership, and thread safety are
  documented;
- static and shared build-tree and installed-package consumers pass on every
  claimed platform;
- all unit, property, differential, conformance, sanitizer, static-analysis,
  package, and fuzz gates pass;
- benchmark results and methodology are published without unsupported claims;
  and
- every advertised optional standard passes its declared conformance suite,
  with deviations published explicitly.

## Appendix A: confirmed 1.0.0 baseline defects

This appendix records evidence from the reviewed baseline. It does not prescribe
implementation details.

### A.1 Embedded-NUL name truncation

The std::string overloads for member access delegate through c_str(). A document
can store both "a" and "a\u0000b", but lookup and erasure using a
three-byte std::string containing a, NUL, b operate on "a".

Affected baseline areas in pjsonlib/src/pjson.cpp include the std::string
operator and find overloads around lines 2795-2819, hasKey around lines
4253-4260, and erase around lines 4313-4323.

### A.2 Descendant move-assignment use-after-free

When a parent is move-assigned from a referenced descendant using the same
allocator, assignment resets the parent before reading the descendant. An
AddressSanitizer run reports heap-use-after-free.

The affected baseline implementation is pjsonlib/src/pjson.cpp around lines
1402-1417.

### A.3 Unsigned integer precision loss

Parsing the decimal representation of UINT64_MAX stores a double and serializes
it as 1.8446744073709552e+19 rather than preserving the integer exactly.

The relevant baseline implementation is pjsonlib/src/pjson.cpp around lines
4028-4107.

### A.4 Configurable stack exhaustion

The default nesting limit is finite, but callers can request an arbitrarily high
limit while DOM and SAX parsing still recurse. A 100,000-level nested document
with a matching configured limit causes stack overflow under AddressSanitizer.

### A.5 Silent schema weakening

The current schema subset intentionally ignores unsupported keywords. For
example, a conditional schema using if and then can accept an instance that a
Draft 2020-12 validator rejects. This behavior is acceptable only while the
feature is clearly advertised as a subset; it is incompatible with a claim of
general Draft 2020-12 validation.

## Appendix B: requirements checklist

Status legend: [x] done, [~] partial (see `docs/featurerequest-response.md`),
[ ] deferred/tracked in `Todo.md`.

- [x] PJSON-COR-001 — Preserve object keys byte-for-byte
- [x] PJSON-COR-002 — Make aliasing mutations memory-safe
- [x] PJSON-NUM-001 — Never silently corrupt an accepted number
- [x] PJSON-NUM-002 — Handle non-finite floating-point values explicitly
- [x] PJSON-NUM-003 — Define finite floating-point conversion precisely
- [x] PJSON-SEC-001 — Make nesting limits stack-safe
- [x] PJSON-PARSE-001 — Keep all parser front ends behaviorally equivalent
- [x] PJSON-PARSE-002 — Apply duplicate-key policy early and consistently
- [x] PJSON-API-001 — Provide non-allocating traversal
- [x] PJSON-API-002 — Complete construction and mutation primitives
- [x] PJSON-API-003 — Separate safe reads from vivifying writes
- [x] PJSON-API-004 — Define type conversion and equality precisely
- [x] PJSON-API-005 — Provide a structured error model
- [x] PJSON-API-006 — Make ownership and allocator behavior complete for the
  documented allocator scope
- [x] PJSON-API-007 — Document thread safety
- [x] PJSON-SER-001 — Guarantee valid and stable JSON output
- [x] PJSON-SER-002 — Preserve deterministic output when requested
- [x] PJSON-SEC-002 — Use uniform, overflow-safe resource budgets
- [x] PJSON-SEC-003 — Preserve transactional mutation guarantees
- [x] PJSON-SEC-004 — Treat regexes and external resources as hostile
- [x] PJSON-SCHEMA-000 — Make subset validation fail closed when requested
- [x] PJSON-SCHEMA-001 — Implement an explicit dialect contract
- [~] PJSON-SCHEMA-002 — Compile and validate schemas separately
- [~] PJSON-SCHEMA-003 — Cover the Draft 2020-12 vocabulary
- [x] PJSON-SCHEMA-004 — Make reference resolution secure and embeddable
- [x] PJSON-SCHEMA-005 — Provide actionable diagnostics
- [~] PJSON-SCHEMA-006 — Prove conformance
- [x] PJSON-EXT-001 — JSON Pointer conformance
- [x] PJSON-EXT-002 — JSON Patch conformance
- [x] PJSON-EXT-003 — JSON Merge Patch conformance
- [ ] PJSON-PERF-001 — Maintain representative benchmarks
- [~] PJSON-PERF-002 — Avoid avoidable work in common DOM operations
- [ ] PJSON-PERF-003 — Track regressions without overclaiming
- [x] PJSON-BUILD-001 — Be a well-behaved CMake subproject
- [x] PJSON-BUILD-002 — Support static and shared consumption correctly
- [x] PJSON-BUILD-003 — Publish immutable package inputs
- [x] PJSON-BUILD-004 — Define the supported platform matrix
- [x] PJSON-BUILD-005 — Keep optional features modular
- [x] PJSON-TEST-001 — Keep conformance corpora mandatory for releases
- [x] PJSON-TEST-002 — Run differential and property tests
- [x] PJSON-TEST-003 — Strengthen fuzzing
- [x] PJSON-TEST-004 — Require sanitizers and static analysis
- [x] PJSON-TEST-005 — Add regression tests for every defect
- [~] PJSON-DOC-001 — Publish one precise behavioral contract
- [x] PJSON-DOC-002 — Maintain compatibility and migration guidance
- [x] PJSON-DOC-003 — Keep security and maintenance expectations explicit
- [x] PJSON-DOC-004 — Classify compatibility impact before implementation
- [~] PJSON-MAINT-001 — Share parser machinery
- [~] PJSON-MAINT-002 — Isolate standards extensions and complex subsystems
