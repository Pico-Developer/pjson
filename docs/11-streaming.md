# Chapter 11 — Streaming large JSON documents

Building a DOM keeps every value in memory. For a multi-gigabyte JSON document,
use pjson's SAX interface instead: it reads an `std::istream` through a fixed
8 KiB input buffer and calls your code as values arrive. Memory then scales with
nesting depth, the largest individual string or number token, and any state your
handler retains, not the total document size. Under the default duplicate-key
rejection policy, the parser also remembers keys in each currently open object,
so a very wide object uses memory proportional to its unique key data.
Follow along with
[`examples/src/08_streaming.cpp`](../examples/src/08_streaming.cpp).

## Define an event handler

Derive from `pjson::SaxHandler` and override only the events you need. Every
callback returns `bool`; return `false` for controlled early termination.

```cpp
#include "pjson.h"

#include <cstdint>
#include <fstream>
#include <iostream>

using ByteDance::pjson;

struct NumberSummary : pjson::SaxHandler {
    uint64_t count = 0;
    double total = 0.0;

    bool onInt(int64_t value) override {
        ++count;
        total += static_cast<double>(value);
        return true;
    }

    bool onUInt(uint64_t value) override {
        ++count;
        total += static_cast<double>(value);
        return true;
    }

    bool onDouble(double value) override {
        ++count;
        total += value;
        return true;
    }
};
```

Available callbacks are `onNull`, `onBool`, `onInt`, `onUInt`, `onDouble`,
`onString`, `onStartArray`, `onEndArray`, `onStartObject`, `onKey`, and
`onEndObject`. They arrive in source order. `onUInt` receives integer tokens
above `INT64_MAX`; handlers that only care about smaller integers may ignore it.

## Parse the stream

```cpp
std::ifstream input("huge.json", std::ios::binary);
NumberSummary summary;
pjson::ParseError error;

if (!pjson::parseSaxStream(input, summary, error)) {
    std::cerr << "JSON error at " << error.line << ':' << error.column
              << " (byte " << error.offset << "): " << error.message << '\n';
    return 1;
}
```

The same parser accepts an in-memory `std::string` or `(const char*, size_t)`
through `parseSax()`. SAX parsing does not build a `pjson` tree; use normal
`parse()` when you need random access or mutation afterward. `parseStream()`
also builds a DOM: it reads in chunks but buffers the complete document before
constructing the tree. Only `parseSaxStream()` provides true incremental input.

## Limits and duplicate keys

All `ParseOptions` apply to streaming input. Their defaults are `maxDepth =
512`, `maxNodes = 1,000,000`, and `maxInputBytes = 64 MiB`. `maxNodes` counts
JSON values even without a DOM, providing a predictable work limit. Zero makes
`maxNodes` or `maxInputBytes` unlimited; a non-positive `maxDepth` is instead
treated as a limit of one. Raise or disable limits only for inputs you trust.

Parsing always follows RFC 8259. The default duplicate policy rejects repeated
keys. `KeepFirstDuplicate` suppresses
events for later duplicate value subtrees. `KeepLastDuplicate` emits both
occurrences because a stream cannot retract an event already delivered.

## Cancellation and exceptions

Returning `false` from a callback stops parsing and reports `SAX parse aborted`.
If a callback throws, pjson catches it and reports a handler exception in
`ParseError`; exceptions do not escape `parseSax*()`.

## Streaming output

`write()` walks the DOM iteratively and writes directly to the destination
stream without constructing a complete serialized string first:

```cpp
std::ofstream output("result.json", std::ios::binary);
pjson::SerializeOptions options = pjson::SerializeOptions::prettyPrinted();
options.indentWidth = 4;
options.indentCharacter = ' ';
options.escapeNonAscii = true;
options.keyOrder = pjson::SerializeOptions::AscendingKeys;
options.maxOutputBytes = size_t(64) * 1024 * 1024;
document.write(output, options);
if (!output) {
    // Inspect or discard the destination according to the application's policy.
}
```

This avoids a whole-document output buffer; traversal state scales with nesting
depth, while escaping a key or string can use temporary memory proportional to
that token. `SerializeOptions` controls indentation, non-ASCII escaping, and
ascending or descending key traversal for both output APIs. The default output
limit is 64 MiB; zero explicitly means unlimited. `write()` returns `void`, so
check the stream state. Invalid stored UTF-8, crossing the configured byte
limit, and indentation/size overflow are logical preflight failures: they set
`failbit` before any bytes are emitted. The corresponding `toString()` call
throws `std::invalid_argument` for invalid UTF-8 or `std::length_error` for
budget/indentation overflow. Only a physical sink or I/O failure during emission
may leave a partial prefix. Write to a temporary file and rename it when atomic
replacement is required.

Next: [Chapter 12 — Custom allocators](12-custom-allocators.md). For the full
verification workflow, see [Chapter 10 — Contributing](10-contributing.md).
