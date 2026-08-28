# Chapter 09 — Testing

pjson ships with a large automated test suite. This chapter shows how to run it
and how it is organized — useful whether you are evaluating pjson or changing
it.

## Running the tests

The easiest way, via the build script:

```sh
./build.sh --test
```

Or directly with CMake and CTest:

```sh
cmake -S . -B build \
  -DPJSON_BUILD_TESTS=ON \
  -DPJSON_BUILD_EXAMPLES=OFF \
  -DPJSON_BUILD_BENCHMARKS=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

CTest registers every `TEST()` separately, so progress and failures are
reported case by case rather than as one aggregate `1/1` executable.
You can run one case by name with `ctest --test-dir build -R pjson.test_name`.

There is still only one test binary. Run it directly to execute every case:

```sh
./out/debug/bin/pjsontest    # if built via ./build.sh
./build/pjsontest/pjsontest  # if built via plain cmake
```

A passing run ends with a summary like:

```
[PASS] version_string  (5 checks)
...
<total> tests, 0 failed, 0 failing checks
```

The runner exits non-zero if any check fails, so it plugs into CI directly.
It also supports `--list-tests` and `--run-test NAME`; CTest uses those options
to enumerate and isolate cases.

The test run offers to fetch both pinned conformance corpora—nst/JSONTestSuite
and JSON-Schema-Test-Suite—when they are missing. Add `--auto` to accept both
downloads without prompting:

```sh
./build.sh --test        # prompt before downloading, then run all tests
./build.sh --test --auto # download automatically, then run all tests
./build.sh --all --auto  # run the complete non-interactive contributor gate
```

The corpora are stored in the gitignored `.test-corpora/` directory and reused
on later runs. You can also fetch the JSON grammar corpus explicitly with
`./scripts/fetch-json-test-suite.sh`; `PJSON_JSONTESTSUITE_DIR` is only needed to
override the standard location. Declining a prompt leaves the optional corpus
test as a clean skip; once a download is accepted, a fetch failure aborts the
full sweep. The fetch helper checks out a pinned corpus commit for reproducible
results. Plain `./build.sh --test --auto` also fetches either corpus when it is
missing. Without `--auto`, both `--test` and `--all` ask before downloading.

The schema suite uses a separately pinned subset manifest drawn from the
JSON-Schema-Test-Suite `draft7` directory. Fetch and run that manifest with:

```sh
./scripts/fetch-json-schema-test-suite.sh
PJSON_JSON_SCHEMA_TEST_SUITE_DIR="$PWD/.test-corpora/JSON-Schema-Test-Suite" \
  ctest --test-dir out/build-debug --output-on-failure \
  -R '^pjson\.schema_official_draft7_optional$'
```

Without that checkout, the official-schema case reports a clean skip; the
repository's inline schema tests still run.

## How the suite is organized

Tests are grouped by topic into files under `pjsontest/src/`, all linked into
one executable:

| File | Focus |
|------|-------|
| `tests_core.cpp` | types, exact typed access, copy/move |
| `tests_build.cpp` | supported operators, setters, and containers |
| `tests_parse.cpp` | valid parsing and the number grammar |
| `tests_strings.cpp` | escaping and Unicode |
| `tests_roundtrip.cpp` | serialize/parse stability, formatting |
| `tests_features.cpp` | version, depth guard, RFC 8259 parsing, errors, equality, streams |
| `tests_schema.cpp`, `tests_schema_complex.cpp`, `tests_schema_vocabulary.cpp` | schema validation and vocabulary |
| `tests_schema_official.cpp` | optional pinned JSON-Schema-Test-Suite subset manifest |
| `tests_malformed.cpp` | exhaustive invalid/hostile input (never throws) |
| `tests_mutation.cpp` | complex add/edit/delete/rebuild scenarios |
| `tests_api_edge.cpp` | normal + edge case for every public method |
| `tests_fuzz.cpp` | deterministic (seeded) fuzzing |
| `tests_pathological.cpp` | extreme numbers, wide payloads, and exact budget boundaries |
| `tests_conformance.cpp` | inline RFC 8259 cases + optional nst/JSONTestSuite corpus |
| `tests_storage.cpp` | inline scalar storage, copy/move/swap, type transitions |
| `tests_allocator.cpp` | custom allocator ownership, failure, move, and swap behavior |
| `tests_streaming.cpp` | SAX events, chunk boundaries, cancellation, direct stream output |
| `tests_serialize_access.cpp` | serialization options and non-vivifying access |
| `tests_pointer_patch.cpp` | JSON Pointer, JSON Patch, and Merge Patch |

## The test harness

Tests use a tiny header-only harness (`pjsontest/src/test_harness.h`). Writing a
test is just:

```cpp
#include "pjson.h"
#include "test_harness.h"
#include "test_util.h"     // pjson_test::parse() helper

using namespace ByteDance;

TEST(my_feature_does_x) {
    auto j = pjson_test::parse(R"({ "a": 1 })");
    CHECK(j != nullptr);
    int64_t value = 0;
    CHECK(j->tryGet("a", value));
    CHECK_EQ(value, int64_t(1));
}
```

- `TEST(name) { ... }` registers a test automatically — no list to maintain.
- `CHECK(expr)` fails the test if `expr` is false.
- `CHECK_EQ(a, b)` checks equality and prints both values on failure.
- `CHECK_PARSE_FAILS(text)` asserts that parsing `text` returns empty.

There is no `main()` to edit; the runner discovers every `TEST` at startup.

## Fuzzing

`tests_fuzz.cpp` feeds thousands of random and random-JSON-flavored byte strings
to the parser, and runs random build/edit/validate sequences. The seeds are
fixed, so a failure is **reproducible**. The fuzzed parsing and mutation entry
points must not crash or emit unexpected exceptions, and successful parses must
round-trip. Expected serialization and allocation failures keep their documented
contracts.

For mutation-guided coverage, Clang builds four standalone libFuzzer targets:
`pjson_fuzz_parse` exercises RFC 8259 DOM round trips,
`pjson_fuzz_stream` compares buffer, stream, and SAX paths, and
`pjson_fuzz_schema` checks schema validation invariants. `pjson_fuzz_patch`
exercises JSON Patch and Merge Patch, checking that failures leave the target
unchanged and successful transformations remain serializable. Run the bounded
seed corpus smoke used by CI with:

```sh
./build.sh --fuzz --auto
```

The no-argument/`--all` contributor sweep runs the same bounded smoke per target
when a usable Clang/libFuzzer toolchain is available and reports a skip on
unsupported platforms. An explicit `--fuzz` request fails with a clear
diagnostic when the required runtime is unavailable. This makes a full sweep
portable while ensuring a requested fuzz job cannot silently pass without
running.

The same sweep also validates the Doxygen API reference, SPDX/REUSE metadata,
and relocatable static/shared install and pkg-config consumers. Those can be
requested independently with `./build.sh --docs`, `./build.sh --license`, and
`./build.sh --package`.

The checked-in seeds live under `fuzz/corpus/`; generated inputs and crash
artifacts are written under the ignored `out/` tree, never back into those seed
directories. A focused direct build is:

```sh
CXX=clang++ cmake -S . -B out/build-fuzz \
  -DPJSON_BUILD_TESTS=OFF \
  -DPJSON_BUILD_EXAMPLES=OFF \
  -DPJSON_BUILD_BENCHMARKS=OFF \
  -DPJSON_BUILD_FUZZERS=ON
cmake --build out/build-fuzz --parallel
```

With no `PJSON_FUZZING_ENGINE`, this requires a full LLVM Clang distribution
with libFuzzer; Apple Command Line Tools alone may not include that runtime. An
external engine may instead be supplied through `PJSON_FUZZING_ENGINE`.
OSS-Fuzz packaging is kept in `oss-fuzz/`, and every target uses
`fuzz/json.dict`.

`PJSON_BUILD_FUZZERS` controls only whether the targets are built. It does not
run them; use `./build.sh --fuzz`, invoke the executables directly, or use the
OSS-Fuzz integration to execute inputs.

## Sanitizers

For memory-safety and undefined-behavior checking, build with sanitizers:

```sh
./build.sh --clean --asan --test
```

This compiles the library and the whole suite with AddressSanitizer and
UndefinedBehaviorSanitizer and fails on any finding — the strongest check
available and the one contributors should run.

## What you learned

- Run tests with `./build.sh --test` or `ctest`; the runner exits non-zero on
  failure.
- Tests are grouped by topic and use a tiny `TEST`/`CHECK` harness that
  auto-registers cases.
- Deterministic generated tests, coverage-guided libFuzzer targets, and a
  sanitizer build (`--asan`) provide complementary robustness coverage.

Next: [Chapter 10 — Contributing](10-contributing.md).
