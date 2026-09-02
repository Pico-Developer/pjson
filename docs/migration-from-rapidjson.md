# Migrating from RapidJSON {#migration-rapidjson}

This guide maps RapidJSON DOM, Reader/Writer, Pointer, and Schema idioms to the
final `ByteDance::pjson` API. pjson favors a compact owning DOM, RFC 8259
parsing, explicit result ownership, status-based errors, and a deliberately
limited schema vocabulary.

```cpp
#include "pjson.h"
using ByteDance::pjson;
```

pjson requires C++11 or newer and is a compiled library. Link `pjson::pjson` or
compile `pjsonlib/src/pjson.cpp` with the application.
`pjsonlib/include/pjson.h` and `pjsonlib/src/pjson.cpp` are the canonical API
and behavior sources; this guide describes how to adapt RapidJSON code to them.

## Migration map

| RapidJSON | pjson | Important difference |
|---|---|---|
| `Document` / `Value` | `pjson` | One owning mutable value type. |
| `SetObject()` / `SetArray()` | `resetTo(jsonObject)` / `resetTo(jsonArray)` | Explicit empty-container reset. |
| `SetInt64()` / `SetDouble()` | assignment from `int64_t` / `double` | Only signed 64-bit integer and double numeric APIs. |
| `IsInt64()` / `IsDouble()` | `isInt()` / `isDouble()` | Storage-kind checks. |
| `GetInt64()` / `GetDouble()` | `tryGet(out)` | Exact-type, status-returning extraction. |
| `GetString()` + `GetStringLength()` | `tryGet(std::string&)` or `tryGet(StringView&)` | `StringView` is borrowed. |
| `HasMember()` / `FindMember()` | `hasKey()` / `find(key)` | Returns a borrowed child pointer or `nullptr`. |
| `operator[]` for lookup | `find` / `tryGet` | pjson subscripting is builder-only and may mutate. |
| member iteration | `keys()` + `find(key)` | No public raw object container. |
| array iteration | `size()` + `find(index)` | No public raw array container. |
| `Document::Parse(...)` | `pjson::parse(...)` | Every DOM overload returns a `pjson` value; pass a `ParseError` for status. |
| `Reader` + handler | `parseSax(...)` / `parseSaxStream(...)` | SAX callbacks return `bool` to continue. |
| `Writer` / `PrettyWriter` | `write(out[, options])` | Configure `SerializeOptions`; inspect stream state. |
| `StringBuffer` + Writer | `toString([options])` | Returns the serialized string. |
| `Pointer::Get` | `findPointer(...)` | Non-vivifying RFC 6901 lookup. |
| Pointer mutation | normal building or `applyPatch(...[, options])` | RFC 6902 patching is atomic and bounded. |
| Merge Patch helper code | `applyMergePatch(...[, options])` | Atomic RFC 7396 with the same limits. |
| `SchemaDocument` + `SchemaValidator` | `pJsonSchemaValidator v(schema); v.validate(value, ...)` | Compile a schema once into the standalone validator; only the documented subset is enforced. |

## Values, ownership, and allocators

### Building values

`operator[]` is the pjson builder API. String access promotes the receiver to
an object and creates a missing null child. Integer access promotes it to an
array and grows it with null children. Use explicit final scalar types:
One indexed access that would create more than 1,000,000 children throws
`std::length_error` before mutation.

```cpp
pjson document;
document["name"] = "Ada";
document["age"] = int64_t(36);
document["ratio"] = double(0.5);
document["roles"][0] = "admin";
```

Supported scalar mutation types are C strings, `std::string`, `bool`,
`int64_t`, and `double`. Exact vector overloads exist for `std::string`, `bool`,
`int64_t`, and `double`; there are no convenience `int`, `float`, or vectors of
`int`, `float`, or C strings. Convert other values explicitly and build other
arrays with indexed assignment:

```cpp
array[static_cast<int>(array.size())] = child;
```

### Parsed roots are returned by value

Every DOM `parse` and `parseStream` overload returns a `pjson` **by value**, for
both default and custom allocation. There is no smart pointer and no manual
`delete`; the value owns its subtree and frees it on destruction. The terse
overloads return JSON `null` on failure; pass a `ParseError` to distinguish
failure from a successfully parsed literal `null`.

```cpp
pjson::ParseError error;
pjson document = pjson::parse(jsonBytes, byteCount, error);
if (!error.ok) {
    std::cerr << error.line << ':' << error.column
              << ": " << error.message << '\n';
    return;
}
```

An allocator passed to a constructor or parse overload is borrowed and must
outlive the complete tree. The returned value is bound to that allocator.
Persistent nodes and wrapper objects use it; backing storage inside standard
containers and parser scratch space use their normal standard allocators.
Copying a `pjson` is deep. Assignment preserves the destination allocator; a
cross-allocator move may allocate. `swap()` is O(1) only when `canSwap()` is
true.

### Parse diagnostics have a reusable lifecycle

Reporting parse and SAX overloads reset `ParseError` on entry. Success leaves
`ok == true`, `code == None`, offset zero, line one, column one, and an empty
message. Failure sets `ok == false`, a stable `code`, and reports the first
problem. Offset is a zero-based byte position; line and byte-column are
one-based. A reused error never intentionally retains diagnostics from the
previous call.

## Parsing always enforces RFC 8259

pjson has no permissive parse mode and no RapidJSON-style syntax feature flags.
All DOM and SAX entry points reject malformed UTF-8, invalid escapes, lone
surrogates, raw string controls, non-lowercase literals, comments, trailing
commas, invalid numbers, `NaN`, `Infinity`, and trailing non-whitespace data.

`ParseOptions` controls only work budgets and duplicate names:

```cpp
pjson::ParseOptions options;
options.maxDepth = 512;
options.maxNodes = 1000000;
options.maxInputBytes = size_t(64) * 1024 * 1024;
options.duplicateKeys = pjson::ParseOptions::RejectDuplicateKeys;

pjson document = pjson::parse(json, error, options);
```

Zero means unlimited for node and input-byte budgets. A non-positive depth
means an effective one-level limit. Duplicate policy can be
`RejectDuplicateKeys`, `KeepFirstDuplicate`, or `KeepLastDuplicate`. This choice
does not weaken RFC 8259 syntax or UTF-8 validation. For SAX, keep-first
suppresses later duplicate value-subtree events; keep-last emits each
occurrence because emitted callbacks cannot be retracted.

`parseStream()` buffers the complete input before DOM parsing. Use
`parseSaxStream()` when input must be consumed incrementally.

## Safe reads and iteration

Do not translate RapidJSON lookup to pjson `operator[]`: it creates structure
and can replace the receiver's type. Use `find` for a borrowed node and
`tryGet` for a typed value:

```cpp
const pjson& root = document;

int64_t count = 0;
if (root.tryGet("count", count))
    use(count);

if (const pjson* settings = root.find("settings")) {
    bool enabled = false;
    if (settings->tryGet("enabled", enabled))
        configure(enabled);
}
```

`tryGet` supports `int64_t`, `uint64_t`, `double`, `bool`, `std::string`, and
`StringView`. It performs no coercion except integer-to-double widening and the
exact signed/unsigned reads described in the numeric section, and it leaves the
destination unchanged on failure. `StringView` is valid only while the owning
node remains alive and unchanged.

Iterate without exposing container internals:

```cpp
for (size_t i = 0; i < array.size(); ++i) {
    if (const pjson* value = array.find(static_cast<int>(i)))
        consume(*value);
}

for (const std::string& key : object.keys()) {
    if (const pjson* value = object.find(key))
        consumeMember(key, *value);
}
```

`find(index)` supports negative end-relative indexes, but normal forward loops
should convert their checked `size_t` position to `int`. `keys()` returns a
copy in deterministic map order. Child pointers are borrowed and can be
invalidated by mutation of the child or an ancestor.

## Numeric migration

pjson stores numbers as signed `int64_t`, unsigned `uint64_t` (for values above
`INT64_MAX`), or `double`. Use `isInt()`/`isUInt()`/`isInteger()`/`isDouble()`
to inspect storage and `tryGet` for extraction. Reading an integer into `double`
is allowed but may lose precision beyond `2^53`; reading a double into an integer
is not an implicit `tryGet` conversion.

`SetUint64`, `GetUint64`, and `IsUint64` map directly onto
`operator=(uint64_t)`, `tryGet(uint64_t&)`, and `isUInt()`; the full `uint64_t`
range round-trips exactly. Values above `UINT64_MAX`, and non-finite floats, are
rejected by default (`ParseOptions::AllowLossyNumbers` and
`SerializeOptions::NonFinitePolicy` opt out). Use explicit `int64_t`,
`uint64_t`, and `double` at all API boundaries rather than relying on C++
overload selection.

## JSON Pointer and patching

`findPointer()` performs non-vivifying RFC 6901 lookup. The empty string
addresses the root. Other pointers start with `/`; array tokens are canonical
unsigned decimal indices, and `-` is not a lookup index. Use
`escapePointerToken()` for keys containing `~` or `/`.

For general pointer mutation, apply an RFC 6902 patch:

```cpp
pjson patch = pjson::parse(R"([
  {"op":"replace", "path":"/address/city", "value":"Paris"},
  {"op":"add", "path":"/tags/-", "value":"new"}
])",
                           error);

pjson::PatchError patchError;
pjson::PatchOptions patchOptions;
if (!error.ok || !document.applyPatch(patch, patchError, patchOptions)) {
    // The document is unchanged on failure.
}
```

`applyPatch()` supports `add`, `remove`, `replace`, `move`, `copy`, and `test`,
and commits atomically. Removing the empty path succeeds and resets the target
document to JSON null. `applyMergePatch()` implements atomic RFC 7396: object
patches merge recursively, null members remove object members, and non-object
patches replace the complete target. Both APIs are `noexcept` and offer a
reporting `PatchError` overload.

Both APIs also accept a trailing `PatchOptions`. Defaults allow 10,000
operations, 1,000,000 cloned nodes, 64 MiB of cloned node/string/key bytes, and
1,000,000 work units. Zero for `maxOperations`, `maxClonedNodes`,
`maxClonedBytes`, or `maxWork` retains the corresponding hard ceiling. A limit
failure reports `PatchError::ResourceLimit` and leaves the target unchanged.
`maxOperations` counts RFC 6902 array entries or Merge Patch members.
Moving the document root beneath itself reports
`PatchError::MoveRootNotAllowed`; root removal remains valid and produces null.

## SAX input and serialized output

Derive from `pjson::SaxHandler` and override the callbacks of interest. Integer
events use `int64_t`; floating events use `double`. Returning `false` cancels
the parse. Callback string and key references are borrowed only for the
callback duration.

Use `SerializeOptions` instead of Writer flags, PrettyWriter setters, or a
boolean pretty argument:

```cpp
pjson::SerializeOptions options = pjson::SerializeOptions::prettyPrinted();
options.indentWidth = 2;
options.indentCharacter = ' ';
options.escapeNonAscii = true;
options.keyOrder = pjson::SerializeOptions::AscendingKeys;
options.maxOutputBytes = size_t(64) * 1024 * 1024;

document.write(output, options);
if (!output)
    reportWriteFailure();

std::string encoded = document.toString(options);
```

The defaults are compact layout, two-space indentation, space indentation,
UTF-8 output, ascending keys, and a 64 MiB output limit. Zero explicitly makes
`maxOutputBytes` unlimited. Object insertion order is not retained. A stored
non-finite double fails serialization by default (`SerializeOptions::nonFinite`
selects `RejectNonFinite`, `NonFiniteToNull`, or `NonFiniteToString`). Finite
doubles use locale-independent, stable round-trip formatting with 15–17
significant digits; shortest spelling is not part of the contract.

Invalid UTF-8 in any stored string or object key is a serialization failure,
regardless of `escapeNonAscii`: `toString()` throws `std::invalid_argument`.
Output-budget or indentation/size overflow throws `std::length_error`. `write()`
detects all three logical failures before emission and sets `failbit`; only a
physical stream failure can leave a partial prefix or propagate an enabled
stream exception. This matters for programmatically built values even though
the parser itself accepts only valid UTF-8.

## Schema validation

RapidJSON 1.1 validates compiled draft-04 schemas. pjson instead compiles a
schema (itself a `pjson`) into a standalone `ByteDance::pJsonSchemaValidator`
(declared in `<pjson_schema.h>`), then validates already-built values against
it, with no SAX validation. The validator is a pure consumer of pjson's public
API. The error overload appends `pJsonSchemaValidator::Error` values; clear a
reused vector first. Error paths are RFC 6901 pointers, with `""` denoting the
root.

The validator implements pjson's explicitly named subset dialect, not the
official Draft 4 or 2020-12 dialect. An unsupported root `$schema` or required
`$vocabulary` makes `isSchemaValid()` false; inspect `schemaErrors()` before
trusting validation. Unknown optional vocabularies are accepted as annotations.

The documented pjson subset is the complete enforced vocabulary; it is not a
complete JSON Schema draft implementation:

| Area | Supported keywords/forms |
|---|---|
| Any value | `type`, `enum`, `const` |
| References | local-fragment `$ref` into the root schema |
| Objects | `properties`, `patternProperties`, `propertyNames`, `required`, `dependentRequired`, `dependencies`, `additionalProperties`, `minProperties`, `maxProperties` |
| Arrays | schema or tuple-array `items`, `minItems`, `maxItems`, `uniqueItems` |
| Numbers | `minimum`, `maximum`, numeric `exclusiveMinimum`, numeric `exclusiveMaximum`, `multipleOf` |
| Strings | `minLength`, `maxLength`, `pattern`, supported `format` names |
| Composition | `allOf`, `anyOf`, `oneOf`, `not` |
| Schema values | Boolean schemas |

Unknown or unsupported schema keywords are ignored and therefore are not
enforced. Treat this as a warning, not forward-compatible validation: typos and
unsupported security constraints can silently weaken a schema. Audit every
schema against this table and retain RapidJSON or another validator when the
application depends on any other vocabulary. Remote references are unsupported.

`minLength` and `maxLength` count Unicode code points rather than UTF-8 bytes.
`pattern` uses ECMAScript syntax and search semantics, but the default policy is
intentionally narrower: pattern and subject sizes are capped and expressions
outside a conservative safe subset fail validation.
`pJsonSchemaValidator::Options::trustedRegex()` removes only regex restrictions
and is appropriate only for trusted schemas and instances. Traversal, reference,
work, and collected-error budgets remain active. Known formats are checked by
default; unknown format names are ignored.

## Practical migration sequence

1. Change every DOM parse result to a `pjson` value plus a `ParseError`, and
   check `error.ok` before use.
2. Replace parse-error inspection and exceptions with `ParseError`; account for
   its reset-on-entry lifecycle.
3. Remove permissive syntax flags and choose explicit budgets and duplicate-key
   policy.
4. Replace lookup through `operator[]` with `find` or `tryGet`; keep subscripting
   only for construction and intentional mutation.
5. Replace member/array container iteration with `keys()`/`find(key)` and
   `size()`/`find(index)`.
6. Normalize numeric interfaces to `int64_t` and `double`, including SAX
   callbacks and vectors.
7. Replace Writer and pretty-boolean configuration with `SerializeOptions`, and
   handle invalid-UTF-8 output failure.
8. Verify every schema keyword is in pjson's documented subset and add
   accepted/rejected tests for every relied-upon constraint.
