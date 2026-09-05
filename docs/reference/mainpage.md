# pjson API Reference {#mainpage}

pjson is an owning, mutable JSON value for C++11. The generated reference is
the symbol-by-symbol companion to the [tutorials](https://github.com/Pico-Developer/pjson/tree/main/docs).
It describes the public header shipped to applications; implementation-only
types are intentionally excluded.

## Start here

- @ref ByteDance::pjson is the central DOM value.
- @ref ByteDance::pJsonParser is the separate configured parser. It consumes
  the public DOM implementation; the DOM does not depend on the parser.
- @ref ByteDance::pjson::Allocator supports allocator-bound persistent DOM
  storage; an allocator-configured parser returns the document by value, bound
  to the chosen allocator.
- @ref ByteDance::pJsonParser::Options configures duplicate keys and input
  budgets; every parser enforces RFC 8259 syntax.
- @ref ByteDance::pJsonParser::Error reports non-throwing parse failures.
- @ref ByteDance::pjson::PointerError and @ref ByteDance::pjson::PatchError
  describe RFC 6901, RFC 6902, and RFC 7396 failures.
- @ref ByteDance::pjson::PatchOptions bounds transactional patch amplification.
- @ref ByteDance::pjson::SerializeOptions controls formatting and escaping,
  and bounds output size.
- ByteDance::pjson::tryGet(), ByteDance::pjson::StringView, and
  ByteDance::pjson::findPointer() provide strict, non-vivifying reads.
- ByteDance::pjson::applyPatch() and ByteDance::pjson::applyMergePatch() apply
  atomic RFC 6902 and RFC 7396 updates.
- @ref ByteDance::pJsonParser::SaxHandler supports incremental, non-DOM parsing.
- @ref ByteDance::pJsonSchemaValidator validates a pjson value against a schema
  (itself a pjson value); its nested @ref ByteDance::pJsonSchemaValidator::Options
  and @ref ByteDance::pJsonSchemaValidator::Error configure and report schema
  validation. It is a standalone helper in `<pjson_schema.h>` that consumes only
  pjson's public API. Default options use its named subset dialect;
  `Options::draft2020()` selects the required Draft 2020-12 vocabularies,
  bundled meta-schemas, and per-resource vocabulary activation. Unsupported
  dialects and required vocabularies fail compilation.

Use the navigation tree to browse classes, nested option/error types, enums,
typedefs, and every public overload. Each entry is generated from the current
installed header, so the reference follows the API as it evolves.

## Guides

- @subpage custom-allocators
- @subpage migration-nlohmann-json
- @subpage migration-rapidjson

The two migration guides call out behavioral differences that a mechanical API
rename would miss: ownership, vivifying access, signed/unsigned numeric storage,
duplicate-key policy, allocator provenance and lifetime, streaming, schema
coverage, and pjson's status-based error model.

## Build this reference locally

With Doxygen and Python 3 installed:

```sh
cmake -S . -B out/build-docs \
  -DPJSON_BUILD_TESTS=OFF \
  -DPJSON_BUILD_EXAMPLES=OFF \
  -DPJSON_BUILD_BENCHMARKS=OFF \
  -DPJSON_BUILD_DOCS=ON
cmake --build out/build-docs --target pjson-docs-check
```

Open `out/build-docs/docs/reference/html/index.html`. The build treats Doxygen
warnings as errors and validates the generated XML so omitted public API
families fail locally and in CI.
