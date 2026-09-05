<!-- SPDX-FileCopyrightText: 2026 ByteDance Ltd. and/or its affiliates -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Coverage-guided fuzzing

## Harnesses

The fuzz build provides seven Clang/libFuzzer targets:

- `pjson_fuzz_parse` exercises strict DOM parsing only, while varying
  duplicate-key policy and bounded parse budgets across the same bytes.
- `pjson_fuzz_stream` compares DOM, buffered stream, SAX-buffer, and chunked
  SAX-stream behavior under the same option variants.
- `pjson_fuzz_serialize` checks transactional structured serialization, stream
  equivalence, output budgets, UTF-8 rejection, and non-finite policies.
- `pjson_fuzz_schema` parses a schema and instance and verifies agreement
  between both validation overloads. Its input is `schema`, one newline byte,
  then `instance`; inputs without a newline are split at their midpoint.
- `pjson_fuzz_pointer` compares mutable, const, string, and C-string RFC 6901
  lookups and verifies that lookups never mutate the document.
- `pjson_fuzz_patch` drives RFC 6902 JSON Patch with varied resource limits.
- `pjson_fuzz_merge_patch` independently drives RFC 7396 Merge Patch. Both
  mutation targets check atomic failure and stable serialization after success.

Inputs under `corpus/{serialize,pointer,patch,merge_patch}` cover successful,
failing, Unicode, embedded-NUL, output-limit, and deep/wide paths.

## Bounded local smoke

Run the bounded smoke used in CI with:

```sh
./build.sh --fuzz --auto
```

On Linux and macOS, `build.sh --fuzz` probes for a usable Clang/libFuzzer
toolchain, configures `-DPJSON_BUILD_FUZZERS=ON`, builds all seven harnesses,
and replays each checked-in seed corpus with deterministic bounds:

- `-runs=1000`
- `-seed=1337`
- `-max_len=65536`
- `-timeout=5`

Checked-in seeds are read-only inputs under `corpus/`; generated corpus entries
and failure artifacts go under the ignored `out/fuzz-corpus/` and
`out/fuzz-artifacts/` trees. To preserve a useful failure or coverage
discovery, minimize it and copy the result into the matching checked-in corpus
with a descriptive name.

## Build integrations

Direct CMake builds use `-DPJSON_BUILD_FUZZERS=ON`.

Local libFuzzer builds rely on Clang plus a working `-fsanitize=fuzzer` runtime:

```sh
cmake -S . -B out/build-fuzz \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DPJSON_BUILD_TESTS=OFF \
  -DPJSON_BUILD_EXAMPLES=OFF \
  -DPJSON_BUILD_BENCHMARKS=OFF \
  -DPJSON_BUILD_FUZZERS=ON
cmake --build out/build-fuzz --parallel --target \
  pjson_fuzz_parse pjson_fuzz_stream pjson_fuzz_serialize pjson_fuzz_schema \
  pjson_fuzz_pointer pjson_fuzz_patch pjson_fuzz_merge_patch
```

External-engine builds pass linker input through the cache variable
`PJSON_FUZZING_ENGINE`. When that variable is non-empty, `fuzz/CMakeLists.txt`
does not require Clang's bundled libFuzzer runtime and instead splits the
provided shell-style engine string before linking each harness. This is the
path used by repository-local OSS-Fuzz integration:

```sh
cmake -S . -B out/build-fuzz \
  -DPJSON_BUILD_TESTS=OFF \
  -DPJSON_BUILD_EXAMPLES=OFF \
  -DPJSON_BUILD_BENCHMARKS=OFF \
  -DPJSON_BUILD_FUZZERS=ON \
  -DPJSON_FUZZING_ENGINE="${LIB_FUZZING_ENGINE}"
```

Repository-local OSS-Fuzz wiring is under `../oss-fuzz/`. Its build script
configures `PJSON_BUILD_FUZZERS=ON`, passes
`PJSON_FUZZING_ENGINE="${LIB_FUZZING_ENGINE}"`, builds all seven harnesses, and
packages per-target seed corpora from their matching `fuzz/corpus/` directories.
