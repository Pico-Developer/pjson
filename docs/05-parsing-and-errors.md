# Chapter 05 — Parsing, resource limits & errors

pjson parses **RFC 8259 JSON**. This chapter shows the resource limits
that keep hostile documents from exhausting memory or the call stack, the
independent duplicate-key policy, and structured diagnostics. Follow along with
[`examples/src/05_parsing_and_errors.cpp`](../examples/src/05_parsing_and_errors.cpp).

## Parse options

Every `parse()` call can take a `pjson::ParseOptions`:

```cpp
struct ParseOptions {
    int maxDepth;          // default 512
    size_t maxNodes;       // default 1,000,000; 0 means unlimited
    size_t maxInputBytes;  // default 64 MiB; 0 means unlimited
    DuplicateKeyPolicy duplicateKeys; // default RejectDuplicateKeys
};
```

```cpp
pjson::ParseOptions opt;
opt.maxNodes = 100000;
pjson::ParseError err;
pjson doc = pjson::parse(text, err, opt);
```

## JSON syntax and duplicate keys

Every parser accepts RFC 8259 JSON only. It rejects, among other malformed input:

| Rejected input | Example |
|----------------|---------|
| Case-insensitive keywords | `NULL`, `True`, `FALSE` |
| Unknown escapes | `"\q"` |
| Lone/unpaired `\u` surrogates | `"\uD800"` |
| Raw control characters in strings | a literal tab inside `"..."` |
| Invalid UTF-8 | malformed byte sequences |
| Ordinary grammar errors | trailing commas, missing quotes, bad numbers |

Example failures from the companion program:

```
uppercase keyword: FAILED at byte 0 (invalid JSON value)
raw tab: FAILED at byte 2 (unescaped control character in string)
```

The default also rejects duplicate object keys. Independently choose
`RejectDuplicateKeys`, `KeepFirstDuplicate`, or `KeepLastDuplicate` through
`ParseOptions::duplicateKeys`. This changes only duplicate handling; it never
relaxes RFC 8259 syntax.

## Resource limits

`maxDepth` caps how deeply values may nest. This is a safety valve: without it,
a maliciously deep document (thousands of nested `[`s) could exhaust the call
stack and crash your program. The default of 512 is generous for real data, and
any configured value is clamped to a stack-safe hard ceiling (1024) that cannot
be exceeded. `maxNodes` separately caps the number of materialized JSON values,
blocking wide flat inputs from amplifying into millions of heap allocations.
`maxInputBytes` rejects oversized buffers before parsing begins.

```cpp
pjson::ParseOptions shallow;
shallow.maxDepth = 3;
pjson::ParseError err;
pjson d = pjson::parse("[[[[1]]]]", err, shallow);   // fails: too deep
```

## Getting the error details

Pass a `pjson::ParseError` to learn what went wrong. Reporting APIs reset every
field on entry: success leaves `ok == true`, `code == None`, offset `0`, line
`1`, column `1`, and an empty message; failure describes the first problem.

```cpp
struct ParseError {
    bool ok;              // true if parsing succeeded
    Code code;            // stable machine-facing category (None on success)
    size_t offset;        // byte index where the problem was found
    size_t line;          // one-based source line
    size_t column;        // one-based byte column
    std::string message;  // human-readable description
};
```

`code` is a stable enum (`Syntax`, `InvalidEncoding`, `DuplicateKey`,
`NumberRange`, `DepthLimit`, `InputLimit`, `NodeLimit`, `AllocationFailure`,
`StreamError`, `CallbackError`, `InvalidArgument`) suitable for programmatic
branching; the `message` text may change between releases.

```cpp
pjson::ParseError err;
pjson doc = pjson::parse("[1, 2, ]", err);
if (!err.ok) {
    std::cerr << "parse failed at " << err.line << ':' << err.column
              << " (byte " << err.offset << "): " << err.message << "\n";
}
```

You can combine both: `parse(text, err, opt)`. Because a failed parse returns a
JSON `null` value, always test `err.ok` (not the value) when the input might
legitimately be `null`.

The same options and error coordinates apply to `parseSax()` and the incremental
`parseSaxStream()` API. SAX callback cancellation and callback exceptions are
converted into an ordinary parse failure rather than escaping. Streaming avoids
buffering the complete document, but current tokens, nesting state, duplicate-key
tracking, and handler-owned state still consume memory.

## Why not exceptions?

pjson does not throw JSON-specific parse exceptions. In-memory JSON and
DOM-allocation failures produce a null `pjson` value plus optional `ParseError`; SAX
handler failures similarly become `false`. An exception-enabled input stream can
still throw while `parseStream()` buffers bytes, and mutating APIs that allocate
may report `std::bad_alloc` unless declared `noexcept`.

## What you learned

- All parsing follows RFC 8259 syntax; duplicate handling is a separate
  policy.
- `maxDepth`, `maxNodes`, and `maxInputBytes` bound stack and memory use.
- `ParseError{ ok, offset, line, column, message }` tells you where and why
  parsing failed, without exceptions.

Next: [Chapter 06 — Schema validation](06-schema-validation.md), where you check
that parsed data has the *shape* you expect.
