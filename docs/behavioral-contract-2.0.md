<!-- SPDX-FileCopyrightText: 2026 ByteDance Ltd. and/or its affiliates -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# pjson 2.0 behavioral contract

Status: normative public behavior for pjson 2.0.x  
Applies to: `pjson.h`, `pjson_schema.h`, and the `pjson::pjson` library target

This page consolidates the guarantees that applications may rely on. The public
headers remain authoritative for overload signatures and enum members. Examples,
benchmarks, private layout, exact diagnostic prose, and undocumented implementation
details are not compatibility promises. pjson follows Semantic Versioning for source
and documented behavior, but does not promise a stable C++ ABI; rebuild the library
and dependents together after an upgrade.

## 1. Value and ownership model

`ByteDance::pjson` is an owning, mutable JSON value with deep-copy semantics. A
value owns its complete subtree. Roots are ordinary C++ values: parsing returns a
`pjson` by value, no smart pointer is exposed, and callers never delete child nodes.

The representations are:

| JSON value | `jsonType` | Stored C++ value |
|---|---|---|
| null | `jsonNull` | no payload; also the default and moved-from state |
| string | `jsonString` | length-aware bytes in `std::string`; valid UTF-8 is required for JSON output |
| signed integer | `jsonNumberInt` | `int64_t` |
| unsigned integer | `jsonNumberUInt` | `uint64_t` |
| fractional/exponent number | `jsonNumberDouble` | `double` |
| boolean | `jsonBoolean` | `bool` |
| array | `jsonArray` | ordered children |
| object | `jsonObject` | bytewise-sorted, unique `std::string` keys |

Object keys and strings may contain embedded NUL bytes. `std::string` and
`StringView` APIs preserve their full lengths; `const char*` APIs are conventionally
NUL-terminated and reject null pointers where documented. Objects do not retain
insertion order.

`size()` is the member/element count for containers and zero for scalars, so
`empty()` is true for every scalar. `clear()` keeps an array or object container but
empties it; it resets a scalar to null. `reset()` always produces null.

## 2. Construction, lookup, and mutation

`operator[]` is a builder, not a read-only lookup:

- key access converts a non-object to an object and inserts a missing null child;
- a non-negative index converts a non-array to an array and fills gaps with null;
- one indexed access may create at most 1,000,000 children; larger growth throws
  `std::length_error` before mutation;
- a valid negative `int` index addresses an existing element from the end; a
  negative index before the beginning throws `std::out_of_range` without mutation.

Use `find`, `findPointer`, `hasKey`, `hasIndex`, `contains`, `tryGet`, or `at` for
reads. `find` and `tryGet` do not create values and report absence/type mismatch by
null or `false`; `at` is non-vivifying and throws `std::out_of_range` on a missing
key/index or wrong container type. `tryGet` leaves its output unchanged on failure.
No scalar-to-string or boolean coercions occur. Integer reads permit only
range-safe signed/unsigned conversion; a `double` read accepts all stored numbers.

`pushBack` and `insertOrAssign` copy lvalues. They transfer an rvalue without a deep
copy when allocator domains match and deep-copy it otherwise. `reserve` promotes a
non-array to an empty array. Array erasure shifts later elements left.

### Borrowing and invalidation

Pointers/references returned by `find`, `findPointer`, `at`, and `operator[]`, plus
`StringView` and traversal callback arguments, borrow storage from the owning tree.
They become invalid when that child or an ancestor is destroyed, replaced, reset,
erased, moved, swapped, cleared, or successfully patched. Array growth/erasure and
object insertion/erasure may invalidate container traversal state. Never mutate a
container's membership or size from its `forEachMember`/`forEachElement` callback;
mutating the current child without resizing the parent is allowed. Callback key/value
views are valid only during the callback.

`keys()` returns owning copies. Direct traversal uses pre-declared function pointers
plus an opaque context and does not copy keys or perform a second lookup. A null
visitor or wrong container type is a successful no-op; returning `false` stops early.

## 3. Copy, move, swap, and aliasing

- Copy construction and assignment are deep. Copy assignment preserves the
  destination allocator.
- Move construction transfers storage in O(1) and leaves the source null. Move
  assignment is O(1) when allocators match; a cross-allocator move deep-copies, may
  allocate, and clears the source only after success.
- `swap` is O(1) only when `canSwap` is true. Cross-allocator swap is a safe no-op.
- Self-copy and self-move are safe. Assignment from an ancestor, descendant, or
  sibling is snapshot-safe. Swapping an ancestor with its descendant is rejected as
  a safe no-op so an ownership cycle cannot be formed.

Copy assignment, cross-allocator move assignment, container promotion/growth, and
document-level Patch operations build replacement state before publication in their
documented failure paths. Allocation failure therefore does not publish a partially
constructed replacement.

## 4. Parsing contract

All DOM, byte-span, buffered-stream, SAX-buffer, and incremental SAX entry points
accept exactly one RFC 8259 JSON value followed only by JSON whitespace. They reject
malformed UTF-8, raw string controls, invalid escapes/surrogates, non-lowercase
literals, malformed numbers, trailing data, and (by default) duplicate object names.
A byte-span is length-aware and may contain NUL bytes; a null source pointer is an
`InvalidArgument` failure even when its size is zero.

Default `ParseOptions` are:

| Option | Default | Zero/non-positive meaning |
|---|---:|---|
| `maxDepth` | 512 | values <= 0 mean one level; all values clamp to hard maximum 1024 |
| `maxNodes` | 1,000,000 | unlimited |
| `maxInputBytes` | 64 MiB | unlimited |
| `duplicateKeys` | `RejectDuplicateKeys` | choose keep-first/keep-last explicitly |
| `numberPolicy` | `RejectUnrepresentableNumbers` | opt into lossy conversion explicitly |

Every DOM overload returns a `pjson` value. A failed parse returns null; because valid
JSON `null` produces the same value, use a `ParseError` overload whenever success must
be distinguished. Reporting overloads reset the error first and provide a stable
`Code`, zero-based byte offset, one-based line, one-based byte column, and unstable
human-readable message. In-memory syntax, budget, numeric, and DOM-allocation
failures are reported rather than exposed as JSON-specific exceptions. Buffered
stream input may still propagate exceptions from an exception-enabled stream or its
temporary standard-allocated buffer.

SAX callbacks occur in source order and borrow string/key values only for the call.
Returning `false` or throwing from a callback stops parsing and becomes
`CallbackError`; callback exceptions do not cross the public SAX boundary. SAX work
already delivered is not rolled back. Under keep-first duplicate policy, later value
subtrees are suppressed; under keep-last, both occurrences are observable because a
stream cannot retract earlier callbacks.

## 5. Numeric contract

Integer tokens in `[INT64_MIN, INT64_MAX]` use `jsonNumberInt`; non-negative integer
tokens through `UINT64_MAX` that exceed `INT64_MAX` use `jsonNumberUInt`. Explicit
`uint64_t` assignment retains unsigned identity even for a small value. Integer
tokens outside `[INT64_MIN, UINT64_MAX]` are rejected by default.

Fractional/exponent tokens use finite `double`. Overflow and a nonzero token that
rounds to zero are rejected by default. `AllowLossyNumbers` permits out-of-range
integers and nonzero-to-zero underflow to use the nearest finite representable
`double`. Decimal conversion is classic-locale and follows the active floating-point
rounding mode; applications that change that mode must restore round-to-nearest for
cross-environment reproducibility. A floating negative-zero token (such as `-0.0`)
is retained as a double, compares equal to zero, and round-trips with its sign; the
integer token `-0` is the ordinary signed integer zero.

Numeric equality and `tryCompareNumber` compare signed integers, unsigned integers,
and doubles without first rounding integers through `double`; `1`, explicit `1u`, and
`1.0` compare equal. NaN is unequal and unordered. Arrays compare in order and
objects by key/value, independent of any construction history.

Finite double output is locale-independent and chooses the shortest tested precision
between `digits10` and `max_digits10` that reparses to the same value. Integral-looking
doubles retain a decimal marker so their storage kind survives a round trip. Exact
lexical spelling is not otherwise guaranteed.

## 6. Serialization contract

Defaults are compact output, raw valid UTF-8, ascending bytewise object-key order,
non-finite rejection, and a 64 MiB output limit. Pretty output defaults to two spaces.
Descending key order and non-ASCII escaping are explicit options; an indentation
character other than space/tab is normalized to space. A zero output limit means
unlimited.

Stored invalid UTF-8 is never emitted. NaN and infinity fail by default; explicit
policies may emit `null` or the strings `"NaN"`, `"Infinity"`, and
`"-Infinity"`.

| API | Logical/allocation failure | Physical stream failure | Publication |
|---|---|---|---|
| `toString(options)` | throws `std::invalid_argument`, `std::length_error`, or allocation exception | n/a | no result |
| `toString(out, error, options)` | returns `false` with stable `SerializeError::Code` | n/a | `out` remains unchanged |
| `write(stream, options)` | sets `failbit`; enabled stream exceptions may propagate | sets stream failure state | logical failures emit no bytes; I/O may leave a prefix |
| `write(stream, error, options)` | returns `false`, sets error and stream failure state | returns `false` as `StreamFailure` | logical failures emit no bytes; I/O may leave a prefix |

The structured serialization overloads are `noexcept`; diagnostic message text is not
stable.

## 7. JSON Pointer, Patch, and Merge Patch

`findPointer` implements RFC 6901 lookup without mutation. The empty pointer selects
the current value; non-empty pointers start with `/`; `~0` and `~1` decode to `~` and
`/`; and `-` is not a lookup index. Reporting overloads provide stable
`PointerError::Code` values and token details. Returned nodes are borrowed.

`applyPatch` implements RFC 6902 and `applyMergePatch` implements RFC 7396. Both are
`noexcept`, transactional at document scope, and leave the target unchanged on any
failure. Defaults/hard ceilings are 10,000 operations, 1,000,000 cloned nodes, 64 MiB
of cloned node/string/key bytes, and 1,000,000 work units; zero retains the hard
ceiling rather than disabling it. Patch `test` uses pjson structural/numeric equality.
Moving a root beneath itself and moving into a descendant are rejected.

## 8. Allocators and destruction

Every value is permanently bound to either the process-lifetime default allocator or
a caller-supplied `Allocator`. A supplied allocator is borrowed and must outlive all
bound roots and descendants. It receives persistent node and string/array/object
wrapper allocations. Standard-container backing storage and temporary parsing,
serialization, Pointer, Patch, and schema work continue to use standard allocation.
`allocate` returns aligned non-null storage or throws; `deallocate` must not throw.

Destruction and subtree cleanup are iterative and do not allocate, including deeply
nested values. A custom allocator must remain usable during unwinding and must provide
its own synchronization if shared across threads.

## 9. Thread safety

Distinct `pjson` values may be used concurrently. A value or any part of its subtree
must not be mutated concurrently with another read or write of that tree. The default
allocator and `getVersion()` are initialization-safe.

`pJsonSchemaValidator` owns immutable copies after construction. One compiled
validator may validate concurrently when callers provide separate error vectors. Its
resolver runs only during construction and is not retained.

## 10. JSON Schema contract

`pJsonSchemaValidator` is an external helper and consumes only public `pjson` APIs.
Construction deep-copies the root and resolved schemas; construction may throw
`std::bad_alloc`. `isSchemaValid()` and `schemaErrors()` report invalid dialects,
vocabularies, keyword shapes, identifiers, anchors, references, resolver failures, and
resource exhaustion. `validate()` is read-only, `noexcept`, and never mutates either
input; its vector overload appends diagnostics rather than clearing the vector.

Default validation implements the named dialect returned by
`documentedSubsetDialectUri()`. `Options::draft2020()` opts into official Draft
2020-12, bundled standard meta-schema validation, and per-resource vocabulary
activation. It supports the
keyword allowlist documented in `pjson_schema.h`, including references/anchors,
conditionals, applicators, `unevaluated*`, object/array/string/numeric assertions, and
six formats. It never performs implicit network I/O. Unknown keywords are ignored in
permissive mode; `Options::strict()` rejects unsupported standard keywords and
malformed supported keywords. `Options::modernSubset()` enables modern `$ref` sibling
semantics and makes `format` annotation-only by default.

The private regex backend implements Unicode-aware
ECMAScript syntax, including property escapes and non-BMP code points. Safe regex mode
bounds patterns/subjects and rejects risky constructs; `trustedRegex()` removes only
that conservative syntax restriction and must be reserved for trusted schemas and
instances. The backend's finite work ceiling remains active.
Validation/reference/work/error/resource budgets remain active.

## 11. Standards and compatibility scope

| Facility | Contract | Scope caveat |
|---|---|---|
| JSON parse/output | RFC 8259 and ECMA-404 data model | duplicate-name policy is explicit; object order is library-defined |
| JSON Pointer | RFC 6901 | lookup API only; `-` is Patch syntax, not lookup |
| JSON Patch | RFC 6902 | bounded and document-atomic |
| JSON Merge Patch | RFC 7396 | bounded and document-atomic |
| JSON Schema | pjson documented subset dialect | not full Draft 2020-12 |

Stable public enum/code values and documented defaults are behavioral API. Exact error
messages, private storage, benchmark numbers, and source-file organization may change
without a major release. Changes to number classification, duplicate defaults, object
ordering, exception behavior, or serialization semantics require deliberate compatible
versioning under `VERSIONING.md`.
