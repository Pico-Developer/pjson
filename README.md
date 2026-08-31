# pjson — Praveen's JSON

**pjson** (short for **P**raveen's **JSON**) is an ultra-simple JSON library for C++.

- Its intent is to make creating and accessing JSON data in C++ as simple as possible.
- A single class, `ByteDance::pjson`, represents any JSON value (null, bool,
  number, string, array, or object) and provides an ergonomic
  `obj["key"][i] = value` style API.
- Licensed under Apache-2.0;
- Current source version: **1.0.0** (`pjson::getVersion()` / the
  `PJSON_VERSION` macro).

---

## Table of contents

- [Building / Installation](#building--installation)
- [Quick start](#quick-start)
- [Creating & building JSON](#creating--building-json)
- [Serializing (`toString`)](#serializing-tostring)
- [Parsing (`parse`)](#parsing)
- [Streaming large documents](#streaming-large-documents)
- [Reading values](#reading-values)
- [Reading arrays](#reading-arrays)
- [Reading objects / maps](#reading-objects--maps)
- [Safe vs. vivifying access](#safe-vs-vivifying-access)
- [Editing an existing document](#editing-an-existing-document)
- [Inspecting, comparing & modifying](#inspecting-comparing--modifying)
- [JSON Pointer, Patch & Merge Patch](#json-pointer-patch--merge-patch)
- [Numbers](#numbers)
- [Schema validation](#schema-validation)
- [Error handling & allocator ownership](#error-handling--allocator-ownership)
- [API reference (cheat sheet)](#api-reference-cheat-sheet)
- [Testing & fuzzing](#testing--fuzzing)
- [Benchmarking](#benchmarking)
- [Documentation & project resources](#documentation--project-resources)
- [Limitations](#limitations)

---

## Building / Installation

### Option 0 : `build.sh` (quickest)

From the repo root, run the bundled script. It configures CMake and builds the
library, tests, and examples in **both Release and Debug**, collecting the
artifacts into an `out/` folder. Works on Linux, macOS, and Windows (Git Bash /
MSYS2):
```sh
./build.sh            # full verification sweep (same as --all)
./build.sh --all      # same as above, explicitly
./build.sh --test     # just build both configs, then run the unit tests
./build.sh --bench    # run dependency-free Release benchmarks
./build.sh --bench-compare --auto  # compare with three pinned libraries
./build.sh --fuzz --auto           # bounded libFuzzer corpus smoke
./build.sh --docs --auto           # generate and validate API reference
./build.sh --package --auto        # relocatable static/shared install + pkg-config checks
./build.sh --license --auto        # validate SPDX/REUSE licensing metadata
./build.sh --clean    # remove out/ first, then build
./build.sh --release-only   # or --debug-only, to build just one config
```
Resulting layout:

```text
out/
  include/pjson.h              public header
  release/lib/libpjson.a       Release library
  release/bin/pjsontest        Release test runner
  release/bin/pjsonbench       Release benchmark runner
  release/bin/examples/        Release example programs
  debug/...                    the same, built as Debug
```
Missing tools (`cmake`, `clang-format`, `clang-tidy`, Doxygen) are detected and, with
your confirmation, installed via the system package manager; pass `--auto` to
install without prompting. Run `./clean.sh` to remove all generated files,
including both pinned test corpora and benchmark dependencies under
`.test-corpora/` and `.benchmark-deps/`.

### Option 1 : Direct source integration

Add the canonical library sources directly to your project and put
`pjsonlib/include` on its include path. There are no third-party dependencies:

- Public header: [`pjsonlib/include/pjson.h`](pjsonlib/include/pjson.h)
- Implementation: [`pjsonlib/src/pjson.cpp`](pjsonlib/src/pjson.cpp)

### Option 2 : Build with CMake directly

```sh
cmake -S . -B build
cmake --build build
```

### Option 3 : Use from CMake

`add_subdirectory(pjson)` in your project and link the exported target:
```cmake
add_subdirectory(pjson)
target_link_libraries(myapp PRIVATE pjson::pjson)
```
Or install a relocatable package:

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/desired/prefix \
  -DPJSON_BUILD_TESTS=OFF \
  -DPJSON_BUILD_EXAMPLES=OFF \
  -DPJSON_BUILD_BENCHMARKS=OFF
cmake --build build --config Release
cmake --install build --config Release
```

Consumers use the same target after pointing CMake at that prefix:

```cmake
find_package(pjson 1.0 CONFIG REQUIRED)
target_link_libraries(myapp PRIVATE pjson::pjson)
```

The install also provides relocatable `pkg-config` metadata (`pkg-config
--cflags --libs pjson`). Repository-local [Conan 2](conanfile.py) and
[vcpkg overlay](packaging/vcpkg/ports/pjson) recipes are available, but their
presence here does not imply publication in a public package registry:

```sh
conan create . --build=missing
vcpkg install pjson --overlay-ports=packaging/vcpkg/ports
```

---

## Quick start

For a standalone first program and its exact compile command, see the
[hello-world tutorial](docs/01-getting-started.md) and its canonical
[source file](examples/src/01_hello_world.cpp).

```cpp
#include "pjson.h"
#include <cstdint>
#include <iostream>
using namespace ByteDance;

int main() {
    // Build a document
    pjson person;
    person["name"]   = "Ada";
    person["age"]    = int64_t(36);
    person["active"] = true;
    person["scores"][0] = int64_t(90);
    person["scores"][1] = int64_t(82);
    person["scores"][2] = int64_t(77);
    person["address"]["city"] = "London";

    const pjson::SerializeOptions pretty =
        pjson::SerializeOptions::prettyPrinted();
    std::cout << person.toString(pretty) << "\n";

    // Every DOM parse returns an owning pjson::unique_ptr (empty on error).
    pjson::unique_ptr parsed = pjson::parse(person.toString());
    if (parsed) {
        std::string name;
        int64_t age = 0;
        if (parsed->tryGet("name", name) && parsed->tryGet("age", age)) {
            std::cout << "name = " << name << "\n";
            std::cout << "age  = " << age << "\n";
        }
    }
}
```

---

## Creating & building JSON

Assigning to `operator[]` builds the tree as you go — intermediate maps and
arrays are created automatically.

```cpp
pjson j;

// Object of key/value
j["myKey1"] = "Value1";          // const char*  -> string
j["myKey2"] = std::string("v2"); // std::string  -> string

// Nested object
j["myKey3"]["myInteger"] = int64_t(1); // signed 64-bit integer
j["myKey3"]["myFloat"]   = double(1.0); // double

// Build an array element by element
for (int64_t i = 0; i != 7; ++i)
    j["myKey4"][static_cast<int>(i)] = i;

// Direct array indexing (extends the array as needed)
j["myKey4"][7] = int64_t(7);
j["myKey4"][8] = "Eight";        // arrays may hold mixed types

// Deep nesting: map -> array -> map -> value
j["myKey4"][9]["ninth"] = double(9.0);

// Take a reference to a sub-node and keep building through it
pjson& doubles = j["myKey3"]["myFloatArray"];
doubles[0] = double(0.0);
doubles[1] = double(1.1);
doubles[3] = double(4.4);        // index 2 is auto-filled with null
```

One indexed access may create at most 1,000,000 children. An access that would
cross that growth limit throws `std::length_error` before mutating the value.

Append to an array with `+=` (it promotes the node to an array if needed):

```cpp
pjson list;
list += int64_t(1);                    // [1]
list += "two";                         // [1,"two"]
list += int64_t(3);                    // [1,"two",3]
list += int64_t(4);                    // [1,"two",3,4]
```

The `=` and `+=` operators accept strings, `bool`, `int64_t`, and `double`, as
well as vectors of `std::string`, `bool`, `int64_t`, or `double`. Convenience
overloads for `int`, `float`, and vectors of those types or C strings are not
part of the API.

The document built above serializes to:
```json
{
  "myKey1": "Value1",
  "myKey2": "v2",
  "myKey3": {
    "myFloat": 1.0,
    "myFloatArray": [
      0.0,
      1.1,
      null,
      4.4
    ],
    "myInteger": 1
  },
  "myKey4": [
    0,
    1,
    2,
    3,
    4,
    5,
    6,
    7,
    "Eight",
    {
      "ninth": 9.0
    }
  ]
}
```
The skipped array position `[2]` is auto-filled with `null` by
`doubles[3] = 4.4`. Object keys come out in sorted order.

---

## Serializing (`toString`)

```cpp
std::string compact = person.toString();       // single line, no extra spaces
pjson::SerializeOptions prettyOptions =
    pjson::SerializeOptions::prettyPrinted();
std::string pretty = person.toString(prettyOptions);
```

Use `SerializeOptions` when formatting must be explicit or reproducible:

```cpp
pjson::SerializeOptions output = pjson::SerializeOptions::prettyPrinted();
output.indentWidth = 4;
output.indentCharacter = ' '; // only space or tab; other values fall back to space
output.escapeNonAscii = true;
output.keyOrder = pjson::SerializeOptions::DescendingKeys;
output.maxOutputBytes = size_t(64) * 1024 * 1024;

std::string text = person.toString(output);
person.write(std::cout, output);
```

Defaults are compact output, two-space indentation when `pretty` is enabled,
raw UTF-8, ascending bytewise key order, and a 64 MiB output limit. Set
`maxOutputBytes = 0` only when explicitly requesting unlimited output. Use
`SerializeOptions::prettyPrinted()` for two-space pretty output. Because objects
use `std::map`, insertion order is not retained; serialization can select
ascending or descending order.

For the document built in [Quick start](#quick-start):

**Compact** — note that object keys are emitted in sorted order:
```json
{"active":true,"address":{"city":"London"},"age":36,"name":"Ada","scores":[90,82,77]}
```

**Pretty** (`toString(SerializeOptions::prettyPrinted())`):
```json
{
  "active": true,
  "address": {
    "city": "London"
  },
  "age": 36,
  "name": "Ada",
  "scores": [
    90,
    82,
    77
  ]
}
```

Strings are automatically escaped (`"`, `\`, control characters, etc.). Valid
UTF-8 input therefore produces valid JSON. If a programmatically stored string
contains invalid UTF-8, `toString()` throws `std::invalid_argument` and `write()`
sets `failbit`; `escapeNonAscii` does not make invalid byte sequences valid. If
the configured output limit or indentation arithmetic would overflow,
`toString()` throws `std::length_error` before returning output and `write()`
sets `failbit`. These logical preflight failures emit no bytes; only a physical
stream/I/O failure can leave a partial prefix.

---

## Parsing

### `parse()` — the recommended API
Every DOM-parsing overload returns an owning `pjson::unique_ptr` (empty on JSON
or DOM-allocation failure), so there is no manual `delete`.

```cpp
pjson::unique_ptr p =
    pjson::parse(R"({ "a": 1, "b": [true, null, "x"] })");
if (p) {
    int64_t a = 0;
    if (p->tryGet("a", a))
        std::cout << a << "\n";   // 1
}   // freed automatically
```

A `(const char*, size_t)` overload handles buffers that are not
NUL-terminated or that contain embedded NUL bytes:
```cpp
auto p = pjson::parse(buffer, length);
```

**Parse options** — `parse()` accepts an optional `ParseOptions`:
```cpp
pjson::ParseOptions opt;
opt.maxDepth = 64;     // reject nesting deeper than this (default 512)
opt.maxNodes = 100000; // cap materialized values (default 1,000,000)
opt.maxInputBytes = 8 * 1024 * 1024; // cap input (default 64 MiB)
opt.duplicateKeys = pjson::ParseOptions::RejectDuplicateKeys; // default
auto p = pjson::parse(text, opt);
```

**Error reporting** — pass a `ParseError` to learn *why*/*where* parsing failed
(no exceptions):
```cpp
pjson::ParseError err;
auto p = pjson::parse("[1, 2, ]", err);
if (!p) {
    std::cerr << "parse failed at " << err.line << ':' << err.column
              << " (byte " << err.offset << "): " << err.message << "\n";
}
```
The parser resets the supplied `ParseError` at the start of every call. Success
leaves `ok == true`, offset `0`, line `1`, column `1`, and an empty message;
failure sets `ok == false` and records the first failure.

### Strict parsing and duplicate keys
The parser always enforces RFC 8259. It rejects:
- unescaped control characters inside strings,
- invalid UTF-8 byte sequences,
- unknown escapes (e.g. `\q`),
- unpaired `\u` surrogates,
- non-lowercase keywords (`NULL`, `True`),
- out-of-range numbers.

Duplicate object keys are rejected by default. Set `duplicateKeys` to
`KeepFirstDuplicate` or `KeepLastDuplicate` only when interoperability requires
it. Resource budgets and duplicate-key handling are the only parse options;
neither relaxes the JSON grammar or UTF-8 validation.

### Reading from a stream
```cpp
std::ifstream file("data.json");
pjson::unique_ptr doc = pjson::parseStream(file);
if (doc) { /* ... */ }
```

`parseStream()` builds a normal DOM. For very large documents, derive from
`pjson::SaxHandler` and use `parseSaxStream()` to receive values incrementally
without buffering the full file or allocating a DOM:

```cpp
struct Counter : pjson::SaxHandler {
    size_t numbers = 0;
    bool onInt(int64_t) override { ++numbers; return true; }
    bool onDouble(double) override { ++numbers; return true; }
};

Counter counter;
pjson::ParseError err;
std::ifstream input("huge.json", std::ios::binary);
if (!pjson::parseSaxStream(input, counter, err)) {
    std::cerr << err.line << ':' << err.column << ": " << err.message << '\n';
}
```

Every SAX callback returns `bool`; return `false` to stop early. Callback
exceptions are caught and returned as a parse failure. `write(std::ostream&)`
also emits directly and incrementally rather than allocating a full serialized
copy first.

## Streaming large documents

For the complete SAX callback list, cancellation/error semantics, resource
limits, and direct streaming output, see
[Chapter 11 — Streaming large JSON documents](docs/11-streaming.md).

---

## Reading values

Use `tryGet()` for typed reads. It returns `false` and leaves the destination
unchanged when a node is absent or has the wrong type. The only numeric widening
it permits is a stored `int64_t` read into a `double`.

Given this document:
```json
{
  "active": true,
  "age": 36,
  "name": "Ada",
  "ratio": 0.5
}
```

```cpp
auto p = pjson::parse(
    R"({ "name": "Ada", "age": 36, "ratio": 0.5, "active": true })");
const pjson& j = *p;

std::string name;
int64_t age = 0;
double ratio = 0.0;
bool active = false;
if (j.tryGet("name", name) && j.tryGet("age", age) &&
    j.tryGet("ratio", ratio) && j.tryGet("active", active)) {
    // name == "Ada", age == 36, ratio == 0.5, active == true
}

const pjson* nameNode = j.find("name");
pjson::jsonType t = nameNode ? nameNode->getType() : pjson::jsonNull;
```

The type tags are: `jsonNull`, `jsonString`, `jsonNumberInt`,
`jsonNumberDouble`, `jsonBoolean`, `jsonArray`, `jsonObject`.

---

## Reading arrays

Given this document (shown formatted so you can see exactly what is being read):
```json
{
  "friends": [
    {
      "name": "Bob"
    },
    {
      "name": "Cid"
    }
  ],
  "scores": [
    90,
    82,
    77
  ],
  "tags": [
    "a",
    "b",
    "c"
  ]
}
```

```cpp
auto p = pjson::parse(
    R"({ "scores": [90, 82, 77], "tags": ["a", "b", "c"],
         "friends": [ {"name":"Bob"}, {"name":"Cid"} ] })");
pjson& j = *p;
```

Read arrays through `size()` and `find(index)`. These operations do not resize or
otherwise modify the array:

```cpp
if (const pjson* scores = j.find("scores")) {
    std::cout << "count = " << scores->size() << "\n"; // 3
    for (size_t i = 0; i < scores->size(); ++i) {
        int64_t value = 0;
        const pjson* element = scores->find(static_cast<int>(i));
        if (element && element->tryGet(value))
            std::cout << value << " "; // 90 82 77
    }
}
```

**Array of objects** — combine iteration with per-element access:
```cpp
if (const pjson* friends = j.find("friends")) {
    for (size_t i = 0; i < friends->size(); ++i) {
        const pjson* entry = friends->find(static_cast<int>(i));
        std::string name;
        if (entry && entry->tryGet("name", name))
            std::cout << name << " "; // Bob Cid
    }
}
```

**Filter by element type** — arrays can be heterogeneous, so check `getType()`
when you only want some elements. Given:
```json
{
  "mixed": [
    1,
    "two",
    3,
    true,
    4
  ]
}
```
```cpp
// Sum only the integer elements -> 1 + 3 + 4 = 8
auto mixed = pjson::parse(R"({ "mixed": [1, "two", 3, true, 4] })");
if (const pjson* node = mixed->find("mixed")) {
    for (size_t i = 0; i < node->size(); ++i) {
        int64_t value = 0;
        const pjson* element = node->find(static_cast<int>(i));
        if (element && element->tryGet(value))
            std::cout << value << " ";
    }
}
```

---

## Reading objects / maps

Use `keys()` to obtain object keys in sorted order, then `find(key)` to read each
member without creating it.

Given this document:
```json
{
  "address": {
    "city": "London",
    "zip": "N1"
  },
  "name": "Ada"
}
```
```cpp
auto p = pjson::parse(
    R"({ "name": "Ada", "address": { "city": "London", "zip": "N1" } })");
pjson& j = *p;

// Iterate top-level keys in sorted order -> "address", then "name"
for (const std::string& key : j.keys()) {
    const pjson* value = j.find(key);
    if (value) {
        pjson::SerializeOptions compact;
        compact.maxOutputBytes = size_t(64) * 1024 * 1024;
        std::cout << key << " : " << value->toString(compact) << "\n";
    }
}
```

To look up a single key without creating it, use `find()` (returns a pointer or
`nullptr`) or `hasKey()`:
```cpp
if (const pjson* addr = j.find("address")) {
    if (const pjson* city = addr->find("city")) {
        std::string value;
        if (city->tryGet(value))
            std::cout << value << "\n"; // "London"
    }
}
```

---

## Safe vs. vivifying access

This is the one sharp edge worth understanding.

`operator[]` is a builder API: accessing a key or index that does not exist
**creates** it, and access can change a node's type. That makes building concise,
but `operator[]` is not a safe read. A single index access that would create
more than 1,000,000 children throws `std::length_error` before mutation:

```cpp
pjson building;
building["scores"][0] = int64_t(90);
building["scores"][1] = int64_t(82);
building["scores"][2] = int64_t(77);

building["scores"][10];    // OOPS: grows the array to 11 elements (nulls)
building["missing"];       // OOPS: creates an empty "missing" key
```

For **non-mutating reads**, use these instead. Given:
```json
{
  "age": 36,
  "name": "Ada",
  "scores": [
    90,
    82,
    77
  ]
}
```

```cpp
auto doc = pjson::parse(
    R"({ "age": 36, "name": "Ada", "scores": [90, 82, 77] })");
const pjson& j = *doc;

// hasKey / find never create anything
if (j.hasKey("scores")) { /* ... */ }

if (const pjson* p = j.find("scores")) {
    // p is a read-only pointer, or nullptr if absent
    if (p->hasIndex(-1)) {
        const pjson* last = p->find(-1); // negative indexes count from the end
        int64_t value = 0;
        if (last && last->tryGet(value)) { /* value == 77 */ }
    }
}

int64_t age = 0;
if (j.tryGet("age", age)) { /* age == 36 */ }
```

For strict scalar reads, `tryGet` works on a node or directly by key/index. It
returns `false` and leaves the output unchanged for a missing child or type
mismatch; only integer-to-`double` widening is accepted:

```cpp
int64_t score = -1;
if (const pjson* scores = j.find("scores")) {
    if (scores->tryGet(0, score)) { /* score == 90 */ }
}

pjson::StringView view;
if (j.tryGet("name", view)) {
    consumeBytes(view.data(), view.size()); // supports embedded NUL bytes
}
```

`StringView` avoids a string copy but borrows the node's bytes. Mutation,
erasure, reset, move, swap, ancestor replacement, or destruction can invalidate
it; make a `std::string` copy when the value must outlive that state.

---

## Editing an existing document

Because `operator[]` returns a mutable reference, editing a parsed tree is the
same as building one. Starting from:
```json
{
  "status": "active",
  "user": {
    "scores": [
      10,
      20,
      30
    ]
  }
}
```
```cpp
auto p = pjson::parse(
    R"({ "user": { "scores": [10, 20, 30] }, "status": "active" })");

// Change values in place
(*p)["user"]["scores"][0] = int64_t(99); // change a value
(*p)["user"]["scores"][1] = "twenty";   // change an element's type

// Replace a whole node (the "status" string becomes an array here)
(*p)["status"][0] = int64_t(1);
(*p)["status"][1] = int64_t(2);

std::cout << p->toString(pjson::SerializeOptions::prettyPrinted());
```

produces:

```json
{
  "status": [
    1,
    2
  ],
  "user": {
    "scores": [
      99,
      "twenty",
      30
    ]
  }
}
```

---

## Inspecting, comparing & modifying

**Type predicates and container queries** answer common questions directly:
```cpp
auto p = pjson::parse(R"({ "scores": [90, 82, 77] })");
pjson& j = *p;

j.isObject();              // true
const pjson* scores = j.find("scores");
scores && scores->isArray();  // true
scores ? scores->size() : 0;  // 3
scores && !scores->empty();   // true
j.getType();               // pjson::jsonObject
```
Predicates: `isNull`, `isString`, `isNumber`, `isInt`, `isDouble`, `isBool`,
`isArray`, `isObject`. `size()` returns the element count for arrays/objects
(0 for scalars); `empty()` is `size() == 0`.

**Read with an application default** — initialize the destination, then replace
it only if `tryGet` succeeds:
```cpp
int64_t count = 0;
j.tryGet("count", count); // count remains 0 if missing or mistyped

std::string name = "anon";
j.tryGet("name", name);   // name remains "anon" on failure
```

**Iterate object keys** (sorted):
```cpp
for (const std::string& key : j.keys()) {
    const pjson* value = j.find(key);
    if (value) {
        pjson::SerializeOptions compact;
        compact.maxOutputBytes = size_t(64) * 1024 * 1024;
        std::cout << key << " = " << value->toString(compact) << "\n";
    }
}
```

**Modify** — `clear()` empties a container (keeping its type), `erase()` removes
a key or index:
```cpp
j.erase("scores");         // remove a map key       -> true if present
j["list"].erase(2);        // remove array element #2 -> true if in range
j.clear();                 // empty the object (stays an object)
```

**Compare** — deep, structural equality. Numbers compare across integer/double
(`1 == 1.0`), objects compare regardless of key order, arrays compare in order:
```cpp
auto a = pjson::parse(R"({"x":1,"y":[2,3]})");
auto b = pjson::parse(R"({"y":[2,3],"x":1.0})");
bool same = (*a == *b);    // true
```

---

## JSON Pointer, Patch & Merge Patch

`findPointer()` performs non-vivifying [RFC 6901](https://www.rfc-editor.org/rfc/rfc6901)
lookup. The empty pointer selects the current value; non-empty pointers begin
with `/`, and `-` is not a lookup index. Escape dynamic object-key tokens with
`escapePointerToken()`:

```cpp
pjson::PointerError pointerError;
if (pjson* city = document.findPointer("/users/0/address/city", pointerError)) {
    *city = "London";
}

std::string key = "a/b~c";
const pjson* value = document.findPointer("/" + pjson::escapePointerToken(key));
```

`applyPatch()` supports the RFC 6902 `add`, `remove`, `replace`, `move`,
`copy`, and `test` operations. `applyMergePatch()` implements RFC 7396. Both
modify the receiver only after the complete operation succeeds, so failure is
atomic, and the `PatchError` overload reports the failing operation/path. A
successful RFC 6902 `remove` at the empty root path leaves the target as JSON
`null`:

```cpp
auto patch = pjson::parse(R"([
    {"op":"replace","path":"/status","value":"ready"},
    {"op":"add","path":"/tags/-","value":"new"}
])");

pjson::PatchError patchError;
pjson::PatchOptions patchLimits;
if (!document.applyPatch(*patch, patchError, patchLimits)) {
    std::cerr << patchError.opIndex << ": " << patchError.message << '\n';
}

auto merge = pjson::parse(R"({"obsolete":null,"enabled":true})");
document.applyMergePatch(*merge, patchError, patchLimits);
```

`PatchOptions` defaults to 10,000 operations, 1,000,000 cloned nodes,
64 MiB of cloned node/string/key bytes, and 1,000,000 work units. Its fields are
`maxOperations`, `maxClonedNodes`, `maxClonedBytes`, and `maxWork`. Zero retains
the corresponding hard ceiling rather than disabling it. Exceeding a budget
returns `false`, reports `PatchError::ResourceLimit` when diagnostics are
requested, and leaves the target unchanged. `maxOperations` applies to RFC
6902 operation entries and to members processed by Merge Patch.
Moving the document root beneath itself is rejected as
`PatchError::MoveRootNotAllowed`; removing the root remains valid and produces
JSON null.

---

## Numbers

- Integers are stored and assigned as **64-bit** (`int64_t`); read them with
  `tryGet(int64_t&)`.
- Non-integers are stored and assigned as **`double`**; read them with
  `tryGet(double&)`. A stored integer may widen to `double`; other conversions
  are rejected.
- There is no unsigned storage kind; range-check other numeric C++ types before
  explicitly converting them to `int64_t` or `double`.
- Double serialization is locale-independent and uses 15–17 significant digits
  as needed for stable parse/serialize round-tripping. Integral-looking doubles
  retain a decimal marker (for example, `1.0`) so reparsing preserves the double
  storage kind; the spelling is not promised to be the shortest possible.

---

## Schema validation

A document can be checked against a **schema that is itself a `pjson` object**,
so schemas load and round-trip through `parse()`/`toString()` like any other
JSON. The documented vocabulary is a deliberately limited subset of
[JSON Schema](https://json-schema.org), not a complete draft implementation.
`validate()` is `noexcept` and normally collects every
applicable failure (a resource-budget failure stops traversal), each
reported as a `SchemaError { std::string path; std::string message; }` where
`path` is a JSON Pointer to the offending node.

```cpp
auto schema = pjson::parse(R"({
    "type": "object",
    "required": ["name", "age"],
    "properties": {
        "name": { "type": "string", "minLength": 1 },
        "age":  { "type": "integer", "minimum": 0 },
        "tags": { "type": "array", "items": { "type": "string" } }
    },
    "additionalProperties": false
})");

auto data = pjson::parse(R"({ "name": "Ada", "age": 36, "tags": ["x","y"] })");

// Simple pass/fail:
if (data->validate(*schema)) {
    /* conforms */
}

// Or collect all the reasons it failed:
std::vector<pjson::SchemaError> errors;
if (!data->validate(*schema, errors)) {
    for (const auto& e : errors) {
        std::cerr << (e.path.empty() ? "(root)" : e.path)
                  << ": " << e.message << "\n";
    }
}
```

Example failure output for `{ "age": "old" }` against the schema above:
```text
(root): missing required property "name"
/age: expected type integer, got string
```

Schemas can equally be built with the normal API instead of parsed from text:

```cpp
pjson schema;
schema["type"] = "object";
schema["required"][0] = "name";
schema["required"][1] = "age";
schema["properties"]["name"]["type"] = "string";
schema["properties"]["age"]["type"]  = "integer";
schema["properties"]["age"]["minimum"] = int64_t(0);
bool ok = data->validate(schema);
```

> **Warning:** Unknown or unsupported schema keywords are ignored and therefore
> impose no constraint. A typo can silently weaken validation. Treat the table
> below as an allowlist, audit schemas before use, and test both accepted and
> rejected instances for every intended rule.

**Supported keywords:**

| Applies to | Keywords |
|------------|----------|
| any / references | `type` (name or array of names), `enum`, `const`, local-fragment `$ref` |
| objects | `properties`, `patternProperties`, `propertyNames`, `required`, `dependentRequired`, `dependencies`, `additionalProperties` (boolean or schema), `minProperties`, `maxProperties` |
| arrays | single-schema or tuple-array `items`, `minItems`, `maxItems`, `uniqueItems` |
| numbers | `minimum`, `maximum`, `exclusiveMinimum`, `exclusiveMaximum`, `multipleOf` |
| strings | `minLength`, `maxLength`, `pattern` (ECMAScript regex), `format` |
| combinators | `allOf`, `anyOf`, `oneOf`, `not` |

Notes:
- `type: "integer"` matches whole numbers (including `2.0`); `type: "number"`
  matches either numeric storage kind (`int64_t` or `double`).
- `enum` / `const` use pjson's deep equality, so they work for arrays and
  objects too.
- A boolean schema is allowed: `true` accepts every value, `false` rejects all.
- `$ref` resolves local URI fragments only; remote references are rejected.
- Known formats are `date`, `time`, `date-time`, `ipv4`, `ipv6`, and `uuid`;
  unknown format names are ignored.
- `minLength` and `maxLength` count Unicode code points, not UTF-8 bytes.
- `pattern` uses ECMAScript syntax and search semantics. The default policy
  limits pattern and subject sizes and rejects unsafe expressions.
- `SchemaOptions` defaults `maxRegexPatternBytes` to 256,
  `maxRegexSubjectBytes` to 4096, `allowUnsafeRegex` to `false`,
  `maxValidationDepth` to 64, `maxRefResolutions` to 1024,
  `maxValidationWork` to 1,000,000, `maxErrors` to 100, and
  `validateFormats` to `true`. Zero removes only a regex byte limit; zero for a
  validation, reference, work, or error budget retains its documented hard
  ceiling. Validation depth has an absolute hard ceiling of 64, so larger
  configured values are clamped to 64. `trustedRegex()` removes only the regex
  limits/safety screen; reserve it for trusted schemas and data.

This is the documented pjson subset, not a complete JSON Schema draft. See
[the schema tutorial](docs/06-schema-validation.md) and the
[generated API reference](https://pico-developer.github.io/pjson/) for details.

---

## Error handling & allocator ownership

- Every `parse()` / `parseStream()` overload returns a `pjson::unique_ptr` (empty on
  JSON or DOM-allocation failure), so ownership is automatic and there is no
  manual `delete`. An exception-enabled input stream can still throw while
  `parseStream()` buffers input.
- A supplied `ParseError` is reset for each attempt. Success leaves its success
  state (`ok`, offset 0, line 1, column 1, empty message); failure records the
  first error with a byte `offset`, one-based `line` and byte `column`, and a
  human-readable `message`.
- The parser rejects trailing garbage, trailing/leading/doubled commas,
  unterminated strings/containers, malformed numbers (`1.`, `.5`, `1e`, `+1`),
  out-of-range numbers (`1e400`), and input nested deeper than
  `ParseOptions::maxDepth`.
- Strings are correctly escaped on output and unescaped on input, including
  `\uXXXX` (decoded to UTF-8) and surrogate pairs.
- Invalid UTF-8 in a programmatically stored string makes `toString()` throw
  `std::invalid_argument`; `write()` instead sets the destination stream's
  `failbit` before emitting bytes. The default
  `SerializeOptions::maxOutputBytes` is 64 MiB; exceeding it produces
  `std::length_error` from `toString()` or preflight `failbit` from `write()`.
- `tryGet` returns `false` without changing its output on a missing value or type
  mismatch. Operations that allocate,
  such as copying a string or moving across allocators, can still report
  allocation failure through the normal C++ mechanism unless their signature
  is explicitly `noexcept`.

The default constructors and parse overloads use pjson's default allocator; a
successful DOM parse returns `pjson::unique_ptr`. Applications
that need to route persistent DOM storage can derive from `pjson::Allocator`,
bind a root during construction, or pass it to an allocator-aware parse:

```cpp
class Arena : public pjson::Allocator {
public:
    void* allocate(size_t bytes, size_t alignment, AllocationKind kind) override;
    void deallocate(void* ptr, size_t bytes, size_t alignment,
                    AllocationKind kind) noexcept override;
};

Arena arena;
pjson value(arena);
pjson::ParseError error;
pjson::unique_ptr parsed = pjson::parse(text, error, arena);
```

`allocate` must return non-null storage satisfying `bytes` and `alignment` or
throw; `deallocate` receives matching metadata and must not throw. A directly
constructed root such as `value` is caller-owned, while its wrapper objects and
dynamic descendants use its bound allocator. A parsed root is a `NodeAllocation`
released through `pjson::unique_ptr`.

`Allocator` is borrowed and must outlive every bound root and descendant. It
covers persistent nodes and string/array/object wrapper objects; backing
allocations inside the standard containers and transient algorithm/parser
scratch space still use the standard allocator. The stateless `ValueDeleter` in
`pjson::unique_ptr` reads allocator provenance from the root; do not release a
parsed root and call `delete` on it.
`allocate` must return non-null storage satisfying the requested size and
alignment or throw; `deallocate` receives matching metadata and must not throw.

Ordinary copy construction inherits the source allocator;
`pjson(source, allocator)` explicitly deep-copies into another one. Copy and
move assignment preserve the destination allocator. Same-allocator moves are
constant-time, while cross-allocator moves deep-transfer and may allocate.
`swap()` is constant-time only when `canSwap()` is true; a cross-allocator swap
is a safe no-op. SAX parsing has no allocator overload because it creates no
persistent DOM.

---

## API reference (cheat sheet)

| Category | Members |
|----------|---------|
| Parse | `parse(str \| ptr,size, ...)`, `parseStream(std::istream&, ...)` → `pjson::unique_ptr` |
| Streaming parse | `parseSax(str \| ptr,size, handler, ...)`, `parseSaxStream(std::istream&, handler, ...)`, `SaxHandler` callbacks |
| Parse options | `ParseOptions{ maxDepth, maxNodes, maxInputBytes, duplicateKeys }`, `ParseError{ ok, offset, line, column, message }` |
| Serialize | `toString([SerializeOptions])`, `write(std::ostream&[, SerializeOptions])`; options include `maxOutputBytes` |
| Type | `getType()`, `isNull/isString/isNumber/isInt/isDouble/isBool/isArray/isObject()` |
| Typed read | node/key/index `tryGet(out&)` for scalars or `StringView`; result is untouched on failure |
| Inspect containers | `size()`, `empty()`, `keys()`, `hasKey(key)`, `hasIndex(index)`, `find(key\|index)` |
| JSON Pointer | `findPointer(pointer[, PointerError])`, `escapePointerToken(token)` |
| JSON Patch | `applyPatch(patch[, PatchError][, PatchOptions])`, `applyMergePatch(patch[, PatchError][, PatchOptions])` |
| Container ops | `size()`, `empty()`, `clear()`, `erase(key)`, `erase(index)` |
| Compare | `operator==`, `operator!=` (deep, structural) |
| Validate | `validate(schema[, errors][, SchemaOptions])` — documented JSON Schema subset |
| Build | `operator[](key\|index)` — **vivifying** |
| Assign | `operator=` for strings, `bool`, `int64_t`, `double`, `std::vector<std::string>`, `std::vector<bool>`, `std::vector<int64_t>`, and `std::vector<double>` |
| Append | `operator+=` for those same scalar and vector types; promotes the node to an array |
| Lifetime / allocator | allocator-aware constructors, `getAllocator()`, `canSwap()`, `copyFrom()`, `swap()` |
| Reset | `reset()` (→ null), `resetTo(jsonType)`, `resetIfNeeded(jsonType)` |

`pjson` copies deeply. Same-allocator moves transfer storage and null the source;
cross-allocator moves deep-transfer into the destination allocator.

---

## Testing & fuzzing

The unit test suite is the single `pjsontest` target under `pjsontest/`. It is
assertion based (via a tiny header-only harness in `test_harness.h`), prints a
`PASS`/`FAIL` line per test, and exits non-zero if any check fails. Topic files
under `pjsontest/src/` link into one executable; CTest discovers and registers
each case individually, so the count remains derived from the source. See the
[testing guide](docs/09-testing.md) for the current suite inventory and corpus
setup.

Easiest — build and run through the script:
```sh
./build.sh --test
```

**Contributors:** before sending a change, run the full sweep — clean build,
sanitizers, tests, formatting and static-analysis checks:
```sh
./build.sh            # or, equivalently, ./build.sh --all
```
The current sweep covers formatting, Release plus sanitized Debug builds, all
tests, comparison benchmarks, bounded fuzz-corpus replay when supported, the
Doxygen reference, relocatable static and shared install consumers, pkg-config
checks, and clang-tidy. It offers to fetch both pinned JSON and JSON Schema
conformance corpora; `--all --auto` installs or downloads missing prerequisites
without prompting. Run `./build.sh --help` for the exact current expansion and
`./build.sh --format` to apply source formatting.

Or with CMake/CTest directly:
```sh
cmake -S . -B build \
  -DPJSON_BUILD_TESTS=ON \
  -DPJSON_BUILD_EXAMPLES=OFF \
  -DPJSON_BUILD_BENCHMARKS=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

Or run the collected binary directly to see every case:
```sh
./out/debug/bin/pjsontest        # if built via ./build.sh
./build/pjsontest/pjsontest      # if built via plain cmake
```

Deterministic generated cases live in `tests_fuzz.cpp`; four standalone
coverage-guided targets exercise DOM parsing/round trips, stream and SAX
agreement, schema validation, and the atomicity of JSON Patch and Merge Patch.
With a full Clang/libFuzzer toolchain on Linux or macOS, replay the checked-in
seeds with the CI-sized budget:

```sh
./build.sh --fuzz --auto
```

Seeds and the shared dictionary live under [`fuzz/`](fuzz); generated corpus
entries and crash artifacts go under ignored `out/` directories. A focused
direct CMake build uses:

```sh
CXX=clang++ cmake -S . -B out/build-fuzz \
  -DPJSON_BUILD_TESTS=OFF \
  -DPJSON_BUILD_EXAMPLES=OFF \
  -DPJSON_BUILD_BENCHMARKS=OFF \
  -DPJSON_BUILD_FUZZERS=ON
cmake --build out/build-fuzz --parallel
```

Without `PJSON_FUZZING_ENGINE`, Clang must provide libFuzzer. An external engine
can instead be supplied through that cache string. Repository-local OSS-Fuzz
integration is provided in [`oss-fuzz/`](oss-fuzz), without implying active
upstream service enrollment.

---

## Benchmarking

The Release benchmark suite measures parsing, compact serialization, read-only
traversal, and deep copying on generated small, medium, and large JSON
documents. Run pjson by itself or compare the same cases with pinned versions
of nlohmann/json, RapidJSON, and simdjson:

```sh
./build.sh --bench --release-only
./build.sh --bench-compare --release-only
./build.sh --bench-compare --release-only --auto # download pinned dependencies without prompting
```

Add real documents with repeatable `--bench-input` arguments:

```sh
./build.sh --bench-compare --release-only \
  --bench-input /path/to/small.json \
  --bench-input /path/to/production-shaped.json
```

Comparison mode groups rows by workload and operation. For example, all four
`small / parse` rows appear together, followed by all four `small / serialize`
rows. Parse, serialize, and traverse cover every library. Copy covers pjson,
nlohmann/json, and RapidJSON; simdjson has no equivalent owned mutable-DOM
deep-copy operation.

### Reference comparison

The following snapshot was produced on this development machine with:

```sh
./build.sh --bench-compare --release-only --auto
```

| Machine detail | Value |
| --- | --- |
| Computer | MacBook Pro (`Mac15,6`) |
| Processor | Apple M3 Pro, 12 cores (6 performance + 6 efficiency) |
| Memory | 36 GB |
| Architecture | ARM64 |
| Operating system | macOS 26.3.1 (`25D771280a`) |
| Compiler | Apple Clang 21.0.0 (`clang-2100.0.123.102`) |
| CMake / build | CMake 4.2.3, Release configuration |
| Compared versions | nlohmann/json 3.11.3, RapidJSON 1.1.0, simdjson 3.12.2 |
| Inputs | Deterministic generated workloads; no additional corpus files |
| Sampling | One warm-up, then six timed samples; iterations calibrated per case |

Each result cell is **median microseconds per operation / MiB/s**. Lower
microseconds are better; higher MiB/s is better. Workload sizes are the original
compact JSON inputs.

| Workload | Operation | pjson | nlohmann/json | RapidJSON | simdjson |
| --- | --- | ---: | ---: | ---: | ---: |
| small (341 B) | parse | 4.06 us / 80.1 MiB/s | 4.18 us / 77.8 MiB/s | 1.68 us / 193.6 MiB/s | 0.80 us / 405.4 MiB/s |
| small (341 B) | serialize | 2.74 us / 118.7 MiB/s | 1.61 us / 201.5 MiB/s | 1.17 us / 277.6 MiB/s | 0.98 us / 331.5 MiB/s |
| small (341 B) | traverse | 0.96 us / 339.7 MiB/s | 0.35 us / 922.6 MiB/s | 0.27 us / 1185.6 MiB/s | 0.37 us / 877.4 MiB/s |
| small (341 B) | copy | 3.13 us / 104.0 MiB/s | 1.61 us / 201.4 MiB/s | 1.17 us / 277.9 MiB/s | N/A |
| medium (117,854 B) | parse | 1108.31 us / 101.4 MiB/s | 1025.26 us / 109.6 MiB/s | 292.90 us / 383.7 MiB/s | 175.75 us / 639.5 MiB/s |
| medium (117,854 B) | serialize | 896.97 us / 125.3 MiB/s | 464.90 us / 241.8 MiB/s | 350.65 us / 320.5 MiB/s | 259.48 us / 433.2 MiB/s |
| medium (117,854 B) | traverse | 271.95 us / 413.3 MiB/s | 94.70 us / 1186.9 MiB/s | 83.00 us / 1354.1 MiB/s | 102.73 us / 1094.0 MiB/s |
| medium (117,854 B) | copy | 987.08 us / 113.9 MiB/s | 406.88 us / 276.2 MiB/s | 159.80 us / 703.3 MiB/s | N/A |
| large (805,216 B) | parse | 14026.68 us / 54.7 MiB/s | 8019.35 us / 95.8 MiB/s | 2173.38 us / 353.3 MiB/s | 1429.88 us / 537.0 MiB/s |
| large (805,216 B) | serialize | 21643.63 us / 35.5 MiB/s | 3756.54 us / 204.4 MiB/s | 2529.02 us / 303.6 MiB/s | 2379.25 us / 322.8 MiB/s |
| large (805,216 B) | traverse | 2464.98 us / 311.5 MiB/s | 777.77 us / 987.3 MiB/s | 605.36 us / 1268.5 MiB/s | 740.37 us / 1037.2 MiB/s |
| large (805,216 B) | copy | 8602.30 us / 89.3 MiB/s | 3447.04 us / 222.8 MiB/s | 1225.48 us / 626.6 MiB/s | N/A |

These numbers are a reproducible local snapshot, not a universal ranking. CPU
scaling, background load, compiler versions, allocator behavior, and workload
shape can materially change results. Re-run the command above on the target
machine before making performance-sensitive decisions.

| Measurement | Interpretation | Better result |
| --- | --- | --- |
| `best us` | Fastest microseconds per operation among six samples. | Lower |
| `median us` | Median microseconds per operation; usually the best primary comparison. | Lower |
| `avg us` | Mean microseconds per operation. | Lower |
| `MiB/s` | Original input size divided by median time. | Higher |
| `bytes` | Original input JSON size. | Context only |
| `iters` | Operations in each calibrated timed sample. | Context only |

`MiB/s` is an input-size-normalized comparison, not actual serialized, visited,
or copied bytes. The final `sink=` value is only an anti-optimization checksum.
Benchmark results have no pass/fail threshold; compare Release runs made on the
same machine under similar load. See the [benchmark guide](bench/README.md) for
the exact timed work, dependency versions, methodology, and sample output.

---

## Documentation & project resources

- [Tutorials](docs/README.md) and [streaming guide](docs/11-streaming.md)
- [Browsable API reference](https://pico-developer.github.io/pjson/) and its
  [source landing page](docs/reference/mainpage.md)
- Migration guides for [nlohmann/json](docs/migration-from-nlohmann-json.md) and
  [RapidJSON](docs/migration-from-rapidjson.md)
- [Custom allocator tutorial](docs/12-custom-allocators.md)
- [Contributing](CONTRIBUTING.md), [security reporting](SECURITY.md), and
  [changelog](CHANGELOG.md)
- [Versioning](VERSIONING.md), [release process](RELEASING.md),
  [licensing/SPDX policy](LICENSING.md), and [authors](AUTHORS)

Build and validate the generated reference with Doxygen and Python 3:

```sh
cmake -S . -B out/build-docs \
  -DPJSON_BUILD_TESTS=OFF \
  -DPJSON_BUILD_EXAMPLES=OFF \
  -DPJSON_BUILD_BENCHMARKS=OFF \
  -DPJSON_BUILD_DOCS=ON
cmake --build out/build-docs --target pjson-docs-check
```

Open `out/build-docs/docs/reference/html/index.html`. Warnings and missing
public API families fail validation.

---

## Limitations

- pjson requires C++11 and owns a mutable DOM; it is not a zero-copy parser.
  `parseSaxStream()` avoids buffering the whole document, although its handler,
  current tokens, nesting state, and duplicate-key tracking still use memory.
- Object insertion order is not preserved; keys are stored in `std::map` and
  serialize in selectable ascending or descending bytewise order.
- Duplicate object keys are rejected by default; `ParseOptions` can explicitly
  keep the first or last value.
- Numbers outside `int64_t` range fall back to `double` (may lose precision);
  there is no separate unsigned-integer representation, and numbers outside
  finite `double` range are rejected. Programmatically stored non-finite
  floating values serialize as `null`.
- Parsing always enforces RFC 8259, including valid UTF-8 and the standard
  lowercase literals and escape syntax.
- Hostile-input limits default to 512 nesting levels, 1,000,000 materialized
  values, and 64 MiB of input; tune `maxDepth`, `maxNodes`, and `maxInputBytes`.
- Schema validation implements a documented subset, not a complete draft. It
  ignores unknown keywords, so unsupported rules and misspellings are not
  enforced. It does not compile/cache
  schemas, resolve remote `$ref` values, validate during SAX parsing, support
  `additionalItems`, or implement newer conditional/unevaluated vocabularies.
  Tuple-form `items` validates corresponding positions but leaves elements past
  the tuple unconstrained. String lengths count Unicode code points. Regex
  matching uses the policy-limited default unless trusted mode is requested.
- A custom `Allocator` routes persistent DOM nodes and wrapper objects, not
  transient scratch storage or the backing allocations inside standard-library
  containers.
