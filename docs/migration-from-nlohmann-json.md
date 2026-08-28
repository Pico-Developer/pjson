# Migrating from nlohmann/json {#migration-nlohmann-json}

This guide maps common `nlohmann::json` idioms to the final
`ByteDance::pjson` API. Both libraries provide a mutable JSON DOM, but pjson
uses explicit ownership, exact typed reads, builder-only subscripting, and
status-based parse and patch errors. A mechanical type rename is therefore not
a safe migration strategy.

The examples assume:

```cpp
// Before
#include <nlohmann/json.hpp>
using json = nlohmann::json;

// After
#include "pjson.h"
using ByteDance::pjson;
```

pjson requires C++11 or newer. It is a compiled library: link `pjson::pjson`
or compile `pjsonlib/src/pjson.cpp` with the application in addition to
including `pjson.h`. Those two files are the canonical API and behavior
sources; generated documentation and examples are explanatory.

## API mapping at a glance

| nlohmann/json | pjson | Important difference |
|---|---|---|
| `json j;` | `pjson j;` | Both start as JSON `null`. |
| `json::object()` / `json::array()` | `j.resetTo(pjson::jsonObject)` / `j.resetTo(pjson::jsonArray)` | pjson has no object/array factory. |
| `json::parse(text)` | `pjson::parse(text)` | Returns `pjson::unique_ptr`; failure is an empty pointer. |
| `json::parse(text, nullptr, false)` | `pjson::parse(text, error)` | Inspect the pointer and optional `ParseError`; there is no discarded value. |
| `input >> j` or `json::parse(input)` | `pjson::parseStream(input)` | Builds a DOM and buffers the complete input. |
| `j.dump()` | `j.toString()` | Compact output. |
| `j.dump(indent, ch, ensure_ascii)` | `j.toString(options)` | Configure a `SerializeOptions` value explicitly. |
| `out << j` | `j.write(out[, options])` | Returns `void`; inspect stream state. |
| `j.is_null()`, `is_string()`, ... | `j.isNull()`, `isString()`, ... | pjson distinguishes signed `int64_t` and `double`; there is no unsigned kind. |
| `j.get<T>()` | `j.tryGet(out)` | Exact-type extraction writes an out-parameter and returns `false` on mismatch. |
| `j.get_ref<const std::string&>()` | `j.tryGet(pjson::StringView&)` | The view is borrowed and mutation-sensitive. |
| `j.contains(key)` | `j.hasKey(key)` | Non-mutating; false on a non-object. |
| `j.find(key)` | `j.find(key)` | pjson returns a borrowed pointer or `nullptr`, not an iterator. |
| `j.value(key, fallback)` | `tryGet`, then choose the fallback | The fallback remains application logic. |
| `j[key] = value` | `j[key] = value` | pjson `operator[]` is a builder and may replace the receiver's type. |
| `j.push_back(value)` | `j[static_cast<int>(j.size())] = value` | Indexed builder access grows an array. |
| `j.erase(key/index)` | `j.erase(key/index)` | Returns `bool`; an array index is `size_t`. |
| range iteration | `size()` + `find(index)`, or `keys()` + `find(key)` | No public raw-container access. |
| `json::sax_parse(...)` | `pjson::parseSax(...)` / `parseSaxStream(...)` | `parseSaxStream()` is the incremental stream path. |
| `j = j.patch(patch)` | `j.applyPatch(patch[, error][, options])` | Mutates atomically; `PatchOptions` bounds amplification. |
| `j.merge_patch(patch)` | `j.applyMergePatch(patch[, error][, options])` | Atomic RFC 7396 with the same limits. |
| external JSON Schema library | `value.validate(schema[, errors][, options])` | Implements only the documented subset. |

## Parsing and ownership

### Every DOM parse returns `pjson::unique_ptr`

All DOM parse and stream-parse overloads return `pjson::unique_ptr`, including
those using the default allocator. An empty pointer means failure. The custom
deleter destroys the complete tree through the allocator recorded by its root.
Do not call `delete` on the pointer or convert it to a differently-deletered
smart pointer.

```cpp
pjson::ParseError error;
pjson::unique_ptr document = pjson::parse(text, error);
if (!document) {
    report(error.message, error.offset, error.line, error.column);
    return;
}
consume(*document);
```

The `(const char*, size_t)` overload parses exactly the supplied byte span,
including embedded NUL bytes. `parseStream()` buffers one complete document.
Pass `pjson&` or `const pjson&` when code only borrows the parsed document, and
move the `pjson::unique_ptr` to transfer root ownership.

Allocator-aware overloads take a borrowed `pjson::Allocator&`. That allocator
must outlive the returned root and every descendant. A directly constructed
root remains caller-owned; a parser-created root is owned by
`pjson::unique_ptr`. SAX parsing builds no persistent DOM and has no allocator
overload.

### `ParseError` is reset on every reporting call

`ParseError::offset` is a zero-based byte offset. `line` and `column` are
one-based, and `column` counts bytes. Every parse overload that accepts a
`ParseError&` resets all fields before doing work. Success leaves:

```text
ok == true, offset == 0, line == 1, column == 1, message.empty()
```

Failure sets `ok == false` and describes the first error. It is safe to reuse
one error object across calls; never infer failure from an old message. Test
the returned pointer first, or `ok` for SAX parsing.

### Parsing always enforces RFC 8259

There is no permissive parsing mode. DOM and SAX parsing reject unknown escapes,
unpaired UTF-16 surrogates, upper- or mixed-case keywords, raw control
characters in strings, malformed UTF-8, invalid number grammar, comments,
trailing commas, `NaN`, `Infinity`, and trailing non-whitespace content.

`ParseOptions` contains resource budgets and duplicate-key policy only:

```cpp
pjson::ParseOptions options;
options.maxDepth = 512;
options.maxNodes = 1000000;
options.maxInputBytes = size_t(64) * 1024 * 1024;
options.duplicateKeys = pjson::ParseOptions::RejectDuplicateKeys;
```

`maxNodes == 0` and `maxInputBytes == 0` mean unlimited. A non-positive
`maxDepth` selects an effective one-level limit, not unlimited parsing. The
duplicate policies are:

| Policy | DOM behavior | SAX behavior |
|---|---|---|
| `RejectDuplicateKeys` | Fail at the second key. | Fail at the second key. |
| `KeepFirstDuplicate` | Keep the first value. | Suppress later duplicate value-subtree events. |
| `KeepLastDuplicate` | Replace the earlier value. | Emit each occurrence because prior events cannot be retracted. |

To preserve nlohmann/json's usual keep-last behavior without weakening RFC
8259 validation:

```cpp
pjson::ParseOptions options;
options.duplicateKeys = pjson::ParseOptions::KeepLastDuplicate;
auto document = pjson::parse(text, options);
```

## Reading without mutation

### Use `find`, `tryGet`, `size`, and `keys`

`operator[]` is only a mutable builder API. A string subscript changes a
non-object into an object and creates a missing null member. An integer
subscript changes a non-array into an array and grows it with null elements.
Do not translate checked or observational nlohmann access into pjson
subscripting.
One indexed access that would create more than 1,000,000 children throws
`std::length_error` before mutation.

Use `find(key)` and `find(index)` for borrowed node access. Both return
`nullptr` for the wrong container type or a missing child and never mutate the
document. Negative indexes count from the end.

```cpp
const pjson& root = *document;

std::string name;
if (root.tryGet("name", name))
    use(name);

if (const pjson* items = root.find("items")) {
    for (size_t i = 0; i < items->size(); ++i) {
        if (const pjson* item = items->find(static_cast<int>(i)))
            consume(*item);
    }
}

for (const std::string& key : root.keys()) {
    if (const pjson* value = root.find(key))
        consumeMember(key, *value);
}
```

`tryGet` supports `int64_t`, `double`, `bool`, `std::string`, and
`pjson::StringView`, both on a node and through key/index child overloads. It
returns `false` on absence or type mismatch and leaves the output unchanged.
An integer may widen to `double`; no other coercion occurs. A `StringView`
borrows bytes and is invalidated when its node or an ancestor is modified or
destroyed.

### Numbers are signed `int64_t` or `double`

pjson has no unsigned numeric representation and no convenience `int` or
`float` API. Use `int64_t` and `double` explicitly in assignments, appends,
vectors, SAX callbacks, and `tryGet` calls:

```cpp
root["count"] = int64_t(42);
root["ratio"] = double(0.5);

int64_t count = 0;
double ratio = 0.0;
if (!root.tryGet("count", count) || !root.tryGet("ratio", ratio))
    reportTypeError();
```

Before narrowing an unsigned source, perform an application-level range check.
An integer read as `double` may lose precision beyond `2^53`.

### Building and editing

Use `operator[]` to build or deliberately mutate paths and scalar assignment
for `std::string`, C strings, `bool`, `int64_t`, and `double`. Exact vector
overloads exist for `std::string`, `bool`, `int64_t`, and `double`; there are no
convenience vectors of `int`, `float`, or C strings. Build other arrays with
indexed assignment.

```cpp
pjson value;
value["name"] = "Ada";
value["age"] = int64_t(36);
value["scores"][0] = int64_t(90);
value["scores"][1] = int64_t(82);
value.erase("obsolete");
```

## JSON Pointer and patch operations

`findPointer()` performs non-vivifying RFC 6901 lookup. The empty pointer
addresses the root. Non-empty pointers begin with `/`; array indices are
canonical unsigned decimal tokens, and `-` is reserved for JSON Patch add. Use
`escapePointerToken()` when constructing paths from object keys.

`applyPatch()` supports RFC 6902 `add`, `remove`, `replace`, `move`, `copy`, and
`test`. It applies the complete operation array to a scratch document and
commits only on success. The reporting overload fills `PatchError`. A `remove`
operation whose path is the empty string succeeds and resets the target to JSON
null. This is the pjson representation of removing the document root.

`applyMergePatch()` provides the same atomic status-based model for RFC 7396.
An object patch recursively merges, null members remove object members, and a
non-object patch replaces the complete target. Both patch APIs are `noexcept`.

Both APIs accept a trailing `PatchOptions`. Defaults allow 10,000 operations,
1,000,000 cloned nodes, 64 MiB of cloned node/string/key bytes, and 1,000,000
work units through `maxOperations`, `maxClonedNodes`, `maxClonedBytes`, and
`maxWork`. Zero retains the corresponding hard ceiling. A budget failure returns
`false`, reports `PatchError::ResourceLimit`, and preserves the target.
`maxOperations` counts RFC 6902 array entries or Merge Patch members.
Moving the document root beneath itself reports
`PatchError::MoveRootNotAllowed`; root removal remains valid and produces null.

## Serialization

Use `SerializeOptions` for every non-default serialization choice; the legacy
boolean pretty-print overloads are not part of the final API.

```cpp
pjson::SerializeOptions options = pjson::SerializeOptions::prettyPrinted();
options.indentWidth = 4;
options.indentCharacter = ' ';
options.escapeNonAscii = true;
options.keyOrder = pjson::SerializeOptions::AscendingKeys;
options.maxOutputBytes = size_t(64) * 1024 * 1024;

std::string text = document->toString(options);
document->write(output, options);
```

Default construction selects compact output, two-space indentation, a space
indent character, UTF-8 output, ascending keys, and a 64 MiB output limit. Set
`maxOutputBytes = 0` only when explicitly requesting unlimited output. Objects
are inherently map-ordered; insertion order is unavailable. Non-finite stored
doubles serialize as JSON `null`. Finite doubles use locale-independent, stable
round-trip formatting with 15–17 significant digits; shortest spelling is not
part of the contract.

Every stored string value and object key must contain valid UTF-8. Invalid
stored UTF-8 is a serialization failure even when `escapeNonAscii` is false:
`toString()` throws `std::invalid_argument`. Output-budget or indentation/size
overflow throws `std::length_error`. `write()` detects all three logical
failures before emission and sets `failbit`; only a physical stream failure can
leave a partial prefix or propagate an enabled stream exception. Check stream
state after `write()`.

## DOM parsing versus SAX streaming

`parse()` and `parseStream()` build an owning DOM. `parseStream()` bounds input
with `maxInputBytes` but buffers the document. `parseSaxStream()` reads
incrementally and retains no DOM. SAX callbacks receive borrowed string/key
references valid only for the duration of the callback. Returning `false` from
a callback cancels parsing; the public call then returns `false` and populates
`ParseError` when supplied.

## JSON Schema validation is a subset

pjson validates a value directly against another `pjson` value. It does not
compile a schema or validate against a meta-schema. The collecting overload
appends `SchemaError` entries; clear a reused vector first. Error paths are RFC
6901 pointers, with the empty string denoting the root.

The documented pjson subset is the complete enforced vocabulary; it is not a
complete JSON Schema draft implementation:

| Area | Supported keywords and forms |
|---|---|
| General | `type` (string or array), `enum`, `const`, local-fragment `$ref` |
| Objects | `properties`, `patternProperties`, `propertyNames`, `required`, `dependentRequired`, `dependencies`, `additionalProperties`, `minProperties`, `maxProperties` |
| Arrays | single-schema or tuple-array `items`, `minItems`, `maxItems`, `uniqueItems` |
| Numbers | `minimum`, `maximum`, numeric `exclusiveMinimum`, numeric `exclusiveMaximum`, `multipleOf` |
| Strings | `minLength`, `maxLength`, `pattern`, and supported `format` values |
| Composition | `allOf`, `anyOf`, `oneOf`, `not` |
| Schema values | Boolean schemas |

Unknown or unsupported schema keywords are ignored and therefore impose no
constraint. This is a compatibility hazard: a typo or unsupported security
rule can make validation less restrictive without producing an error. Audit
schemas against the table above and retain an external validator when another
vocabulary is required. Remote references are unsupported; `$ref` resolves
only local URI-fragment JSON Pointers.

`minLength` and `maxLength` count Unicode code points, not UTF-8 bytes.
`pattern` uses ECMAScript regular-expression syntax with search semantics. The
default policy caps pattern and subject sizes and rejects expressions outside a
conservative safe subset. `SchemaOptions::trustedRegex()` removes only those
regex restrictions and should be used only when both schema and instance are
trusted. Validation depth, reference, work, and error-count budgets remain in
effect. Known format checks run by default; unknown format names are ignored.

## Suggested migration sequence

1. Replace parse results with `pjson::unique_ptr` and check every result before
   dereferencing it.
2. Replace exception-based parse handling with `ParseError`, remembering that
   reporting calls reset it on entry.
3. Remove permissive parser flags; pjson always enforces RFC 8259 syntax.
4. Choose a duplicate-key policy and explicit resource budgets.
5. Replace observational subscripting and checked-access calls with `find` or
   `tryGet`; reserve `operator[]` for building.
6. Replace raw-container iteration with `size()` plus `find(index)`, or
   `keys()` plus `find(key)`.
7. Normalize numeric code to `int64_t` and `double`, with explicit range checks
   at unsigned boundaries.
8. Replace dump flags and pretty booleans with `SerializeOptions`, and handle
   invalid-UTF-8 serialization failure.
9. Audit every schema keyword against pjson's documented subset and test both
   accepted and rejected instances.
