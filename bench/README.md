## Benchmark Suite

`bench/` contains the benchmark suite for `pjson`. It is intentionally
self-contained and C++11-only: no Google Benchmark or other external runtime
dependency is required.

Baseline `--bench` runs stay dependency-free and offline. Cross-library
comparison runs when requested with `--bench-compare` and as part of the full
`--all` sweep.

### What it measures

The `pjsonbench` executable reports per-operation timing for:

- `parse`
- `serialize` (compact `toString()`)
- `traverse` (recursive read-only walk over the parsed tree)
- `copy` (deep copy via the copy constructor)

When built with `--bench-compare`, it also reports comparable rows for:

- `nlohmann/json` v3.11.3
- `RapidJSON` v1.1.0
- `simdjson` v3.12.2

Comparison mode covers `parse`, compact `serialize`, and `traverse` for all
three third-party libraries. `copy` is reported only where deep-copy semantics
are reasonably comparable, which currently means `pjson`, `nlohmann/json`, and
RapidJSON. simdjson is intentionally parse-on-demand and does not expose the
same owned mutable DOM copy model, so no simdjson copy row is emitted.

Each operation runs against representative generated workloads:

- `small`: compact session-style object with nested user/event data
- `medium`: user/session dataset with arrays, nested objects, booleans, and numbers
- `large`: inventory-style dataset with hundreds of nested records and repeated arrays

The harness adapts iteration counts so each measurement runs long enough to
produce stable timings, then records six timed samples. Comparison output is
grouped by workload and operation, so the pjson, nlohmann/json, RapidJSON, and
simdjson rows for the same test appear next to each other. The `copy` group
omits simdjson because it has no comparable owned mutable-DOM deep-copy API.

### Build and run

From the repository root:

```bash
./build.sh --bench --release-only
```

`--bench` always runs the dependency-free Release benchmark executable. The
complete `--all` sweep includes comparison mode; it prompts before fetching the
pinned dependencies, while `--all --auto` downloads them automatically.

### Cross-library comparison

Run comparison mode directly with:

```bash
./build.sh --bench-compare --release-only
```

The first `--bench-compare` run fetches these pinned upstream releases into the
gitignored `.benchmark-deps/` directory:

- `nlohmann/json` `v3.11.3`
- `RapidJSON` `v1.1.0`
- `simdjson` `v3.12.2`

`build.sh` verifies the exact commit behind each tag before configuring the
comparison target and rejects locally modified tracked dependency files.

The pjson library and baseline benchmark remain C++11. The optional comparison
target is compiled as C++17 because simdjson's public API uses
`std::string_view`.

The fetch step follows the same prompt policy as the rest of `build.sh`:

- interactive by default
- automatic with `--auto`

If comparison mode is not requested, no benchmark dependencies are downloaded
or required for configure/build/run.

### Optional corpus inputs

You can extend the generated workloads with real JSON documents:

```bash
./build.sh --bench --release-only \
  --bench-input /path/to/sample1.json \
  --bench-input /path/to/sample2.json
```

The same `--bench-input` arguments work with `--bench-compare`.

The benchmark binary also accepts direct inputs:

```bash
./out/release/bin/pjsonbench --input /path/to/sample.json
```

Unreadable or invalid JSON inputs are skipped with a warning so the suite still
runs on the remaining workloads.

### Output

The report is plain text and intended for direct, case-by-case comparison. For
example, comparison mode groups all implementations of `small / parse`, then
all implementations of `small / serialize`, and so on:

```text
library       workload              operation          bytes       iters       best us     median us        avg us       MiB/s
------------------------------------------------------------------------------------------------------------------------------
pjson         small                 parse                341       32768          ...             ...             ...         ...
nlohmann      small                 parse                341       32768          ...             ...             ...         ...
rapidjson     small                 parse                341       65536          ...             ...             ...         ...
simdjson      small                 parse                341      131072          ...             ...             ...         ...

pjson         small                 serialize            341       65536          ...             ...             ...         ...
nlohmann      small                 serialize            341       65536          ...             ...             ...         ...
rapidjson     small                 serialize            341       65536          ...             ...             ...         ...
simdjson      small                 serialize            341      131072          ...             ...             ...         ...
```

#### How to interpret each measurement

| Measurement | Meaning | Better result |
| --- | --- | --- |
| `best us` | Fastest per-operation time among the six samples, in microseconds. | **Lower is better.** |
| `median us` | Conventional median per-operation time across the six samples. This is the best primary comparison because it is less sensitive to one unusually fast or slow sample. | **Lower is better.** |
| `avg us` | Arithmetic mean per-operation time across the six samples. | **Lower is better.** |
| `MiB/s` | Original input JSON size divided by `median us`, normalized to mebibytes per second. | **Higher is better.** |
| `bytes` | Byte length of the original input JSON. It is not serialized output size or DOM memory usage. | Context only; neither lower nor higher is better. |
| `iters` | Number of operations in each timed sample, selected automatically so the sample runs long enough. | Context only; neither lower nor higher is better. |

`MiB/s` always uses the original input size. For `serialize`, `traverse`, and
`copy`, it is therefore a consistent input-size-normalized rate, not a count of
the actual bytes emitted, visited, or copied.

The timed work includes result consumption that prevents the compiler from
removing the operation:

| Operation | Work included in the timed body |
| --- | --- |
| `parse` | Parse the input into a DOM, then recursively hash the result. |
| `serialize` | Compact-serialize a pre-parsed DOM, then hash the output. |
| `traverse` | Recursively hash a pre-parsed DOM. |
| `copy` | Deep-copy a pre-parsed DOM, then recursively hash the copy. |

The final `sink=` line is only an opaque anti-optimization checksum. It is not a
performance measurement and should be ignored when comparing libraries.

The suite does not impose pass/fail thresholds because benchmark numbers are
sensitive to machine load, CPU scaling, allocator behavior, and backend-specific
parser strategies. Record results from comparable Release builds on the same
machine when tracking regressions.
