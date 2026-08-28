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

---

## Medium Priority

### [ ] MAINT-1 — Unify DOM and SAX parser grammar code

**Where:** DOM parsing and SAX parsing currently use separate recursive-descent
implementations in `pjson.cpp`, with differential conformance tests guarding
their behavior.

**Why:** duplicated token, number, Unicode, and container grammar logic raises
the chance that a future parser fix reaches only one API. The current paths are
well tested, so this is architectural debt rather than a release blocker.

**How:** extract a shared lexer/parser core parameterized by a DOM builder or SAX
event sink. Preserve the current error offsets, duplicate-key policies, resource
budgets, streaming cursor behavior, and DOM/SAX differential regression suite.

### [ ] MAINT-2 — Split schema validation into keyword-family helpers

**Where:** `_validateCtx` coordinates references, scalar keywords, containers,
regular expressions, and combinators in one large dispatcher.

**Why:** the shared depth, work, reference, and reported-error budgets make this
logic security-sensitive; smaller helpers would make future keyword changes
easier to review without changing the public validation contract.

**How:** extract focused reference, numeric, string, array, object, and
combinator helpers that all receive the same validation context and error sink.
Keep the official schema manifest and resource-budget tests green throughout.

### [ ] MAINT-3 — Discover CTest cases from the compiled test registry

**Where:** `pjsontest/CMakeLists.txt` currently extracts `TEST(name)` tokens
from source text, while the executable separately exposes `--list-tests`.

**Why:** comments, conditional compilation, or future macro wrappers could make
source-text discovery drift from the cases compiled into the runner. CI compares
both counts today, so this is guarded architectural debt rather than a release
blocker.

**How:** add a post-build discovery helper that invokes
`pjsontest --list-tests` and generates the CTest entries from that output. Keep
the CI nonzero/count check as a defense-in-depth assertion.

### [ ] FEAT-3 — Preserve object key insertion order

**Where:** pjson currently stores objects in `std::map`, so serialization sorts
keys alphabetically.

**Why:** round-tripping that reorders keys creates noisy configuration and golden
file diffs. Most modern JSON DOMs preserve insertion order even though JSON
object semantics do not require it.

**How:** use an insertion-ordered representation, such as a vector of key/value
pairs plus a lookup index. Preserve structural equality semantics and retain
protection from hash-collision denial of service if a hash index is introduced.
