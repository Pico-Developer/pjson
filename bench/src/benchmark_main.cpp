#include "benchmark_build_config.h"
#include "pjson.h"
#include "pjson_parser.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

#ifdef PJSON_BENCH_COMPARE
#include <nlohmann/json.hpp>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <simdjson.h>
#include <simdjson/dom/serialization.h>
#endif

namespace {

    using ByteDance::pjson;
    using ByteDance::pJsonParser;

    // -------------------------------------------------------------------------
    // Benchmark data and anti-optimization state
    // -------------------------------------------------------------------------

    // Timed operations feed observable results into these sinks so an optimizing
    // compiler cannot discard otherwise unused parse, traversal, copy, or output work.
    volatile std::size_t g_sink_size = 0;
    volatile std::uint64_t g_sink_hash = 0;

    // Owns both the source text and its pre-parsed pjson DOM. Parse benchmarks use
    // jsonText, while the other operations intentionally reuse parsed.
    struct Workload {
        std::string name;
        std::string origin;
        std::string jsonText;
        pjson parsed;
    };

    // Per-operation timing summary. Times remain in nanoseconds internally and
    // are converted to microseconds only while rendering the report.
    struct RunStats {
        std::size_t iterations;
        double bestNs;
        double medianNs;
        double averageNs;
        double mibPerSecond;
    };

    // Defers printing until all libraries have run, allowing rows to be grouped
    // by workload and operation instead of by implementation.
    struct BenchmarkResult {
        std::size_t workloadIndex;
        std::string library;
        std::string operation;
        RunStats stats;
    };

    static const double kTargetSeconds = 0.075;
    static const std::size_t kMaxIterations = 1U << 22U;
    static const int kSamples = 6;

    // -------------------------------------------------------------------------
    // Stable result hashing and DOM traversal
    // -------------------------------------------------------------------------

    // Combines one value into a running, deterministic checksum. This is an
    // anti-optimization aid, not a cryptographic or collision-resistant hash.
    std::uint64_t mixHash(std::uint64_t seed, std::uint64_t value) {
        seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
        return seed;
    }

    // Hashes bytes with FNV-style mixing so serialized output and JSON strings
    // have an observable value without console I/O during timing.
    std::uint64_t hashBytes(const char* data, std::size_t size) {
        std::uint64_t hash = 1469598103934665603ULL;
        for (std::size_t i = 0; i < size; ++i) {
            hash ^= static_cast<unsigned char>(data[i]);
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    std::uint64_t hashString(const std::string& value) {
        return hashBytes(value.data(), value.size());
    }

    // Consumes a size-valued result outside the measured library's data model.
    void consumeSize(std::size_t value) {
        g_sink_size ^= value + 0x9e3779b9U;
    }

    // Consumes a checksum produced by parsing, serialization, or traversal.
    void consumeHash(std::uint64_t value) {
        g_sink_hash ^= value + 0x517cc1b727220a95ULL;
    }

    // Recursively visits every pjson node and incorporates types, values, keys,
    // and collection sizes into a checksum. Each backend below mirrors this work.
    std::uint64_t traversePjson(const pjson& value) {
        std::uint64_t hash =
            mixHash(0x84222325cbf29ce4ULL, static_cast<std::uint64_t>(value.getType()));
        switch (value.getType()) {
            case pjson::jsonNull:
                return mixHash(hash, 0ULL);
            case pjson::jsonString: {
                pjson::StringView text;
                return value.tryGet(text) ? mixHash(hash, hashBytes(text.data(), text.size()))
                                          : hash;
            }
            case pjson::jsonNumberInt: {
                int64_t number = 0;
                return value.tryGet(number) ? mixHash(hash, static_cast<std::uint64_t>(number))
                                            : hash;
            }
            case pjson::jsonNumberUInt: {
                std::uint64_t number = 0;
                return value.tryGet(number) ? mixHash(hash, number) : hash;
            }
            case pjson::jsonNumberDouble: {
                double number = 0.0;
                if (!value.tryGet(number)) {
                    return hash;
                }
                std::uint64_t bits = 0;
                std::memcpy(&bits, &number, sizeof(bits));
                return mixHash(hash, bits);
            }
            case pjson::jsonBoolean: {
                bool boolean = false;
                return value.tryGet(boolean) ? mixHash(hash, boolean ? 1ULL : 0ULL) : hash;
            }
            case pjson::jsonArray: {
                const std::size_t elementCount = value.size();
                hash = mixHash(hash, static_cast<std::uint64_t>(elementCount));
                for (std::size_t i = 0; i < elementCount; ++i) {
                    const pjson* child = value.findIndex(i);
                    if (child != NULL) {
                        hash = mixHash(hash, traversePjson(*child));
                    }
                }
                return hash;
            }
            case pjson::jsonObject: {
                hash = mixHash(hash, static_cast<std::uint64_t>(value.size()));
                const std::vector<std::string> objectKeys = value.keys();
                for (std::size_t i = 0; i < objectKeys.size(); ++i) {
                    hash = mixHash(hash, hashString(objectKeys[i]));
                    const pjson* child = value.find(objectKeys[i]);
                    if (child != NULL) {
                        hash = mixHash(hash, traversePjson(*child));
                    }
                }
                return hash;
            }
        }
        return hash;
    }

    // -------------------------------------------------------------------------
    // Deterministic generated workloads
    // -------------------------------------------------------------------------

    // Produces fixed-width decimal fragments for repeatable generated keys and IDs.
    std::string makePaddedNumber(int value, int width) {
        std::ostringstream stream;
        stream << std::setw(width) << std::setfill('0') << value;
        return stream.str();
    }

    // Builds a compact object representative of a small API/session response.
    pjson buildSmallDocument() {
        pjson root;
        root["kind"] = "session";
        root["active"] = true;
        root["version"] = static_cast<int64_t>(3);
        root["user"]["id"] = static_cast<int64_t>(42);
        root["user"]["name"] = "Ada Lovelace";
        root["user"]["region"] = "us-west";
        root["tags"] += "json";
        root["tags"] += "cxx11";
        root["tags"] += "perf";
        root["stats"]["latency_ms"] = 12.75;
        root["stats"]["count"] = static_cast<int64_t>(7);
        for (int i = 0; i < 4; ++i) {
            pjson event;
            event["index"] = static_cast<int64_t>(i);
            event["ok"] = (i % 2) == 0;
            event["label"] = std::string("evt-") + makePaddedNumber(i, 2);
            root["events"][i] = event;
        }
        return root;
    }

    // Builds a medium user dataset with nested sessions, scalar arrays, and a
    // separate time series to exercise a varied but predictable DOM shape.
    pjson buildMediumDocument() {
        pjson root;
        root["dataset"] = "medium-generated";
        root["meta"]["page"] = static_cast<int64_t>(5);
        root["meta"]["source"] = "benchmark";
        root["meta"]["region"] = "us-central";

        for (int i = 0; i < 160; ++i) {
            pjson user;
            user["id"] = static_cast<int64_t>(1000 + i);
            user["name"] = std::string("user-") + makePaddedNumber(i, 4);
            user["email"] = std::string("user-") + makePaddedNumber(i, 4) + "@example.com";
            user["enabled"] = (i % 3) != 0;
            user["ratio"] = 0.5 + static_cast<double>(i % 11) / 10.0;
            for (int j = 0; j < 12; ++j) {
                user["scores"] += static_cast<int64_t>((i * 13 + j * 7) % 1000);
            }
            for (int j = 0; j < 6; ++j) {
                pjson session;
                session["id"] =
                    std::string("sess-") + makePaddedNumber(i, 4) + "-" + makePaddedNumber(j, 2);
                session["duration_ms"] = static_cast<int64_t>(100 + ((i + 1) * (j + 3)) % 4000);
                session["success"] = ((i + j) % 5) != 0;
                session["endpoint"] =
                    std::string("/v1/resource/") + makePaddedNumber((i + j) % 23, 2);
                user["sessions"][j] = session;
            }
            user["prefs"]["theme"] = (i % 2 == 0) ? "light" : "dark";
            user["prefs"]["lang"] = (i % 4 == 0) ? "en" : "fr";
            user["prefs"]["alerts"] = (i % 7) != 0;
            root["users"][i] = user;
        }

        for (int i = 0; i < 48; ++i) {
            pjson snapshot;
            snapshot["timestamp"] = std::string("2026-08-") + makePaddedNumber((i % 28) + 1, 2);
            snapshot["value"] = static_cast<int64_t>(9000 + i * 17);
            snapshot["rolling_avg"] = 1.25 + static_cast<double>(i) * 0.125;
            root["series"][i] = snapshot;
        }

        return root;
    }

    // Builds a large inventory dataset whose repeated nested arrays and objects
    // make traversal, serialization, and deep-copy costs visible.
    pjson buildLargeDocument() {
        pjson root;
        root["dataset"] = "large-generated";
        root["metadata"]["tenant"] = "benchmark";
        root["metadata"]["version"] = static_cast<int64_t>(20260826);
        root["metadata"]["replicas"] = static_cast<int64_t>(3);
        root["metadata"]["healthy"] = true;

        for (int i = 0; i < 900; ++i) {
            pjson item;
            item["id"] = std::string("item-") + makePaddedNumber(i, 5);
            item["sku"] = std::string("SKU-") + makePaddedNumber(100000 + i, 6);
            item["title"] = std::string("Generated payload entry ") + makePaddedNumber(i, 5);
            item["price"] = 9.5 + static_cast<double>((i * 17) % 2500) / 10.0;
            item["inventory"] = static_cast<int64_t>((i * 37) % 1200);
            item["active"] = (i % 9) != 0;
            item["shipping"]["weight_g"] = static_cast<int64_t>(200 + (i % 45) * 13);
            item["shipping"]["width_cm"] = 10.0 + static_cast<double>(i % 15);
            item["shipping"]["height_cm"] = 6.0 + static_cast<double>((i * 3) % 9);
            item["shipping"]["depth_cm"] = 4.0 + static_cast<double>((i * 5) % 11);

            for (int j = 0; j < 5; ++j) {
                item["tags"] += std::string("tag-") + makePaddedNumber((i + j) % 40, 2);
            }

            for (int j = 0; j < 8; ++j) {
                pjson metric;
                metric["bucket"] = static_cast<int64_t>(j);
                metric["count"] = static_cast<int64_t>(((i + 3) * (j + 5)) % 5000);
                metric["avg"] = 0.25 + static_cast<double>((i + j) % 100) / 8.0;
                metric["max"] = 1.5 + static_cast<double>((i * (j + 1)) % 700) / 5.0;
                item["metrics"][j] = metric;
            }

            for (int j = 0; j < 4; ++j) {
                pjson change;
                change["ts"] = std::string("2026-08-") + makePaddedNumber((j % 28) + 1, 2) +
                               "T12:" + makePaddedNumber((i + j) % 60, 2) + ":00Z";
                change["actor"] = std::string("svc-") + makePaddedNumber((i + j) % 17, 2);
                change["delta"] = static_cast<int64_t>(((i + 1) * (j + 2)) % 31) - 10;
                item["history"][j] = change;
            }

            root["items"][i] = item;
        }

        return root;
    }

    // Isolates lookup and iteration costs for objects with many siblings.
    pjson buildWideObjectDocument() {
        pjson root;
        for (int i = 0; i < 2048; ++i) {
            root[std::string("field-") + makePaddedNumber(i, 5)] = static_cast<int64_t>(i * 17);
        }
        return root;
    }

    // Isolates contiguous array parsing, traversal, serialization, and copying.
    pjson buildLargeArrayDocument() {
        pjson root;
        for (int i = 0; i < 8192; ++i) {
            root += static_cast<int64_t>((i * 7919) % 1000003);
        }
        return root;
    }

    // Uses long, unescaped UTF-8 values so string storage dominates node overhead.
    pjson buildStringHeavyDocument() {
        pjson root;
        const std::string base =
            "The quick brown fox jumps over the lazy dog; pjson benchmark payload ";
        for (int i = 0; i < 1024; ++i) {
            root += base + makePaddedNumber(i, 5) + " \xE2\x98\x83";
        }
        return root;
    }

    // Forces the wire representation through quote, slash, control-character,
    // and Unicode escaping paths instead of measuring only raw string copying.
    pjson buildEscapeHeavyDocument() {
        pjson root;
        const std::string escaped =
            "quote=\" backslash=\\ newline=\n tab=\t control=\x01 snowman=\xE2\x98\x83";
        for (int i = 0; i < 1024; ++i) {
            root += escaped + makePaddedNumber(i, 5);
        }
        return root;
    }

    // Keeps the payload numeric while spanning signed and unsigned-looking values.
    pjson buildIntegerHeavyDocument() {
        pjson root;
        for (int i = 0; i < 8192; ++i) {
            const int64_t magnitude = static_cast<int64_t>(i) * 1000003LL + 17LL;
            root += (i % 3 == 0) ? -magnitude : magnitude;
        }
        return root;
    }

    // Exercises decimal conversion with fractional values and varied magnitudes.
    pjson buildFloatingHeavyDocument() {
        pjson root;
        for (int i = 0; i < 8192; ++i) {
            const double scale = (i % 2 == 0) ? 0.000001 : 1000000.0;
            root += (static_cast<double>((i * 104729) % 10000019) + 0.125) * scale;
        }
        return root;
    }

    // -------------------------------------------------------------------------
    // Workload preparation
    // -------------------------------------------------------------------------

    // Validates and stores one workload, then consumes its initial DOM and size so
    // preparation itself cannot be optimized away in whole-program builds.
    Workload makeWorkload(const std::string& name, const std::string& origin,
                          const std::string& jsonText) {
        Workload workload;
        workload.name = name;
        workload.origin = origin;
        workload.jsonText = jsonText;
        pJsonParser::Error parseError;
        workload.parsed = pJsonParser().parse(workload.jsonText, parseError);
        if (!parseError.ok) {
            std::cerr << "failed to parse benchmark workload: " << name << "\n";
            std::exit(1);
        }
        consumeHash(traversePjson(workload.parsed));
        consumeSize(workload.jsonText.size());
        return workload;
    }

    // Reads an optional corpus file as raw bytes; an empty string signals an
    // unreadable or empty input and is handled by buildWorkloads.
    std::string readFile(const std::string& path) {
        std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
        if (!input) {
            return std::string();
        }
        std::ostringstream buffer;
        buffer << input.rdbuf();
        return buffer.str();
    }

    // Extracts a display name while accepting both POSIX and Windows separators.
    std::string basenameOf(const std::string& path) {
        const std::string::size_type slash = path.find_last_of("/\\");
        if (slash == std::string::npos) {
            return path;
        }
        return path.substr(slash + 1);
    }

    // Creates the built-in mixed and shape-specific workloads and appends valid
    // corpus document. Invalid inputs are warned about rather than aborting a run.
    std::vector<Workload> buildWorkloads(const std::vector<std::string>& inputFiles) {
        std::vector<Workload> workloads;
        workloads.push_back(makeWorkload("small", "generated", buildSmallDocument().toString()));
        workloads.push_back(makeWorkload("medium", "generated", buildMediumDocument().toString()));
        workloads.push_back(makeWorkload("large", "generated", buildLargeDocument().toString()));
        workloads.push_back(
            makeWorkload("wide-object", "generated", buildWideObjectDocument().toString()));
        workloads.push_back(
            makeWorkload("large-array", "generated", buildLargeArrayDocument().toString()));
        workloads.push_back(
            makeWorkload("string-heavy", "generated", buildStringHeavyDocument().toString()));
        workloads.push_back(
            makeWorkload("escape-heavy", "generated", buildEscapeHeavyDocument().toString()));
        workloads.push_back(
            makeWorkload("integer-heavy", "generated", buildIntegerHeavyDocument().toString()));
        workloads.push_back(
            makeWorkload("floating-heavy", "generated", buildFloatingHeavyDocument().toString()));

        for (std::size_t i = 0; i < inputFiles.size(); ++i) {
            const std::string jsonText = readFile(inputFiles[i]);
            if (jsonText.empty()) {
                std::cerr << "warning: unable to read benchmark input '" << inputFiles[i] << "'\n";
                continue;
            }

            pJsonParser::Error parseError;
            pjson parsed = pJsonParser().parse(jsonText, parseError);
            if (!parseError.ok) {
                std::cerr << "warning: benchmark input is not valid JSON and was skipped: "
                          << inputFiles[i] << "\n";
                continue;
            }

            Workload workload;
            workload.name = std::string("corpus:") + basenameOf(inputFiles[i]);
            workload.origin = inputFiles[i];
            workload.jsonText = jsonText;
            workload.parsed = std::move(parsed);
            workloads.push_back(std::move(workload));
        }

        return workloads;
    }

    // -------------------------------------------------------------------------
    // Adaptive timing and statistics
    // -------------------------------------------------------------------------

    // Executes one operation repeatedly under a single steady-clock measurement.
    template <typename Operation> double runBatch(std::size_t iterations, Operation operation) {
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < iterations; ++i) {
            operation();
        }
        const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
        const std::chrono::duration<double> elapsed = end - start;
        return elapsed.count();
    }

    // Warms up the operation, doubles the batch size until it reaches the target
    // duration, then records six equal-sized samples. Adaptive batches reduce timer
    // noise for fast cases while the fixed cap bounds unexpectedly expensive runs.
    template <typename Operation>
    RunStats measure(const std::string& payload, Operation operation) {
        // Perform one untimed call to trigger lazy initialization before calibration.
        operation();

        // Calibrate with powers of two so every timed sample has enough useful work.
        std::size_t iterations = 1;
        while (iterations < kMaxIterations) {
            const double seconds = runBatch(iterations, operation);
            if (seconds >= kTargetSeconds) {
                break;
            }
            const std::size_t next = iterations * 2U;
            if (next <= iterations) {
                break;
            }
            iterations = next;
        }

        // Normalize samples to per-operation nanoseconds before summarizing them.
        std::vector<double> sampleNs;
        sampleNs.reserve(kSamples);
        double totalNs = 0.0;
        double bestNs = std::numeric_limits<double>::max();
        for (int i = 0; i < kSamples; ++i) {
            const double seconds = runBatch(iterations, operation);
            const double nsPerIteration =
                (seconds * 1000000000.0) / static_cast<double>(iterations);
            sampleNs.push_back(nsPerIteration);
            totalNs += nsPerIteration;
            if (nsPerIteration < bestNs) {
                bestNs = nsPerIteration;
            }
        }

        // Sorting places the middle samples needed for the conventional median.
        std::sort(sampleNs.begin(), sampleNs.end());
        RunStats stats;
        stats.iterations = iterations;
        stats.bestNs = bestNs;
        const std::size_t middle = sampleNs.size() / 2;
        if ((sampleNs.size() % 2U) == 0U) {
            stats.medianNs = (sampleNs[middle - 1U] + sampleNs[middle]) / 2.0;
        } else {
            stats.medianNs = sampleNs[middle];
        }
        stats.averageNs = totalNs / static_cast<double>(sampleNs.size());
        stats.mibPerSecond = 0.0;
        // All operations use source payload bytes for a consistent normalized rate.
        if (stats.medianNs > 0.0) {
            stats.mibPerSecond =
                (static_cast<double>(payload.size()) * 1000000000.0 / stats.medianNs) /
                (1024.0 * 1024.0);
        }
        return stats;
    }

    // -------------------------------------------------------------------------
    // Command help and report rendering
    // -------------------------------------------------------------------------

    // Prints accepted arguments and the high-level benchmark scope.
    void printUsage(const char* argv0) {
        std::cout << "Usage: " << argv0 << " [--input <json-file>]... [--compare] [--json <path>]\n"
                  << "Benchmarks parse, compact serialize, traversal, and deep copy\n"
                  << "across mixed and shape-specific generated documents, plus any\n"
                  << "JSON files supplied with --input. --json writes a versioned,\n"
                  << "machine-readable result document; use '-' for JSON-only stdout.\n"
                  << "PJSON_BENCH_ENVIRONMENT may name a controlled runner. When built\n"
                  << "with optional\n"
                  << "third-party dependencies, --compare groups every benchmark case\n"
                  << "with adjacent cross-library rows. Timing is lower-is-better;\n"
                  << "throughput is higher-is-better.\n";
    }

    // Explains which reported columns are measurements and which provide context.
    void printMetricGuide() {
        std::cout
            << "How to read the measurements:\n"
            << "  best/median/avg us : microseconds per operation; LOWER is better.\n"
            << "  MiB/s              : input-size-normalized rate from median time; HIGHER is "
               "better.\n"
            << "  bytes              : original input JSON size; context only, not a score.\n"
            << "  iters              : operations in each of six timed samples; context only, "
               "not a score.\n"
            << "  MiB/s always uses original input size; it is not actual output, traversal, or "
               "copy bytes.\n\n";
    }

    // Prints the fixed-width table heading shared by baseline and comparison runs.
    void printHeader() {
        std::cout << std::left << std::setw(14) << "library" << std::setw(22) << "workload"
                  << std::setw(12) << "operation" << std::right << std::setw(12) << "bytes"
                  << std::setw(12) << "iters" << std::setw(14) << "best us" << std::setw(14)
                  << "median us" << std::setw(14) << "avg us" << std::setw(12) << "MiB/s" << "\n";
        std::cout << std::string(126, '-') << "\n";
    }

    // Renders one recorded case, converting nanoseconds to microseconds at the edge.
    void printResultRow(const char* library, const Workload& workload, const char* operation,
                        const RunStats& stats) {
        std::cout << std::left << std::setw(14) << library << std::setw(22) << workload.name
                  << std::setw(12) << operation << std::right << std::setw(12)
                  << workload.jsonText.size() << std::setw(12) << stats.iterations << std::setw(14)
                  << std::fixed << std::setprecision(2) << (stats.bestNs / 1000.0) << std::setw(14)
                  << (stats.medianNs / 1000.0) << std::setw(14) << (stats.averageNs / 1000.0)
                  << std::setw(12) << std::setprecision(1) << stats.mibPerSecond << "\n";
    }

    // Appends a result with its workload index so no workload text or DOM is copied.
    void recordResult(std::vector<BenchmarkResult>& results, std::size_t workloadIndex,
                      const char* library, const char* operation, const RunStats& stats) {
        BenchmarkResult result;
        result.workloadIndex = workloadIndex;
        result.library = library;
        result.operation = operation;
        result.stats = stats;
        results.push_back(result);
    }

    // Emits results in workload/operation order. Since each runner appends results
    // in library order, comparable implementations appear on adjacent rows.
    void printResultsByCase(const std::vector<Workload>& workloads,
                            const std::vector<BenchmarkResult>& results) {
        // This canonical order also determines which operation groups are emitted.
        static const char* const kOperations[] = {"parse", "serialize", "traverse", "copy"};

        printMetricGuide();
        printHeader();
        for (std::size_t workloadIndex = 0; workloadIndex < workloads.size(); ++workloadIndex) {
            for (std::size_t operationIndex = 0;
                 operationIndex < sizeof(kOperations) / sizeof(kOperations[0]); ++operationIndex) {
                bool printedCase = false;
                for (std::size_t resultIndex = 0; resultIndex < results.size(); ++resultIndex) {
                    const BenchmarkResult& result = results[resultIndex];
                    if (result.workloadIndex != workloadIndex ||
                        result.operation != kOperations[operationIndex]) {
                        continue;
                    }
                    printResultRow(result.library.c_str(), workloads[workloadIndex],
                                   result.operation.c_str(), result.stats);
                    printedCase = true;
                }
                if (printedCase) {
                    std::cout << "\n";
                }
            }
        }
    }

    std::string currentUtcTime() {
        const std::time_t now = std::time(NULL);
        const std::tm* utc = std::gmtime(&now);
        if (utc == NULL) {
            return "unknown";
        }
        char text[32] = {};
        if (std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", utc) == 0U) {
            return "unknown";
        }
        return text;
    }

    // Builds a stable JSON artifact for storage and comparison by external tools.
    // It deliberately records raw measurements without applying a regression
    // threshold: only callers with a controlled machine can set a meaningful one.
    pjson buildMachineReport(const std::vector<Workload>& workloads,
                             const std::vector<BenchmarkResult>& results, bool compared) {
        pjson report;
        report["format"] = "pjson-benchmark";
        report["format_version"] = static_cast<int64_t>(1);
        report["captured_at_utc"] = currentUtcTime();
        report["library_version"] = PJSON_VERSION;

        report["source"]["commit"] = PJSON_BENCH_GIT_COMMIT;
        report["source"]["fingerprint"] = PJSON_BENCH_SOURCE_FINGERPRINT;
        report["source"]["dirty_known"] = std::string(PJSON_BENCH_GIT_DIRTY) != "unknown";
        report["source"]["dirty"] = std::string(PJSON_BENCH_GIT_DIRTY) == "true";

        const char* environment = std::getenv("PJSON_BENCH_ENVIRONMENT");
        const char* cpu = std::getenv("PJSON_BENCH_CPU");
        const char* allocator = std::getenv("PJSON_BENCH_ALLOCATOR");
        report["environment"]["label"] =
            environment != NULL && environment[0] != '\0' ? environment : "unspecified";
        report["environment"]["operating_system"] = PJSON_BENCH_SYSTEM_NAME;
        report["environment"]["operating_system_version"] = PJSON_BENCH_SYSTEM_VERSION;
        report["environment"]["architecture"] = PJSON_BENCH_SYSTEM_PROCESSOR;
        report["environment"]["cpu"] = cpu != NULL && cpu[0] != '\0' ? cpu : "unspecified";
        report["environment"]["allocator"] =
            allocator != NULL && allocator[0] != '\0'
                ? allocator
                : "default C++ runtime allocator (implementation unspecified)";

        report["build"]["type"] = PJSON_BENCH_BUILD_TYPE;
        report["build"]["compiler_path"] = PJSON_BENCH_COMPILER_PATH;
        report["build"]["compiler_id"] = PJSON_BENCH_COMPILER_ID;
        report["build"]["compiler_version"] = PJSON_BENCH_COMPILER_VERSION;
        report["build"]["cxx_standard"] = compared ? "C++17" : "C++11";
        report["build"]["flags"] = std::string(PJSON_BENCH_BUILD_FLAGS) +
                                   (PJSON_BENCH_BUILD_FLAGS[0] == '\0' ? "" : " ") +
                                   PJSON_BENCH_TARGET_FLAGS;

        report["methodology"]["clock"] = "std::chrono::steady_clock";
        report["methodology"]["warmup_calls"] = static_cast<int64_t>(1);
        report["methodology"]["timed_samples"] = static_cast<int64_t>(kSamples);
        report["methodology"]["target_sample_seconds"] = kTargetSeconds;
        report["methodology"]["maximum_iterations_per_sample"] =
            static_cast<uint64_t>(kMaxIterations);
        report["methodology"]["primary_statistic"] = "median_ns";
        report["methodology"]["throughput_basis"] =
            "original input bytes divided by median latency";
        report["methodology"]["threshold_policy"] = "none; compare controlled runs externally";

        report["implementations"][0]["name"] = "pjson";
        report["implementations"][0]["version"] = PJSON_VERSION;
#ifdef PJSON_BENCH_COMPARE
        if (compared) {
            report["implementations"][1]["name"] = "nlohmann/json";
            report["implementations"][1]["version"] = "3.11.3";
            report["implementations"][2]["name"] = "RapidJSON";
            report["implementations"][2]["version"] = "1.1.0";
            report["implementations"][3]["name"] = "simdjson";
            report["implementations"][3]["version"] = "3.12.2";
        }
#else
        (void)compared;
#endif

        for (std::size_t i = 0; i < workloads.size(); ++i) {
            report["workloads"][i]["name"] = workloads[i].name;
            report["workloads"][i]["origin"] = workloads[i].origin;
            report["workloads"][i]["input_bytes"] =
                static_cast<uint64_t>(workloads[i].jsonText.size());
        }
        for (std::size_t i = 0; i < results.size(); ++i) {
            const BenchmarkResult& result = results[i];
            const Workload& workload = workloads[result.workloadIndex];
            pjson& row = report["results"][i];
            row["library"] = result.library;
            row["workload"] = workload.name;
            row["operation"] = result.operation;
            row["input_bytes"] = static_cast<uint64_t>(workload.jsonText.size());
            row["iterations_per_sample"] = static_cast<uint64_t>(result.stats.iterations);
            row["best_ns"] = result.stats.bestNs;
            row["median_ns"] = result.stats.medianNs;
            row["average_ns"] = result.stats.averageNs;
            row["mib_per_second"] = result.stats.mibPerSecond;
        }
        return report;
    }

    bool writeMachineReport(const std::string& path, const pjson& report) {
        pjson::SerializeOptions options;
        options.pretty = true;
        options.indentWidth = 2;
        if (path == "-") {
            report.write(std::cout, options);
            std::cout << "\n";
            return static_cast<bool>(std::cout);
        }
        std::ofstream output(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
        if (!output) {
            std::cerr << "unable to open benchmark JSON output '" << path << "'\n";
            return false;
        }
        report.write(output, options);
        output << "\n";
        if (!output) {
            std::cerr << "failed to write benchmark JSON output '" << path << "'\n";
            return false;
        }
        return true;
    }

    // -------------------------------------------------------------------------
    // pjson benchmark cases
    // -------------------------------------------------------------------------

    // Measures all pjson operations. Only parse constructs its input DOM in the
    // timed body; serialize, traverse, and copy start from the prepared DOM.
    void runPjsonBenchmarks(const std::vector<Workload>& workloads,
                            std::vector<BenchmarkResult>& results) {
        for (std::size_t i = 0; i < workloads.size(); ++i) {
            const Workload& workload = workloads[i];

            const RunStats parseStats = measure(workload.jsonText, [&workload]() {
                pJsonParser::Error parseError;
                pjson parsed = pJsonParser().parse(workload.jsonText, parseError);
                if (!parseError.ok) {
                    std::cerr << "benchmark parse failed for " << workload.name << "\n";
                    std::exit(1);
                }
                consumeHash(traversePjson(parsed));
            });
            recordResult(results, i, "pjson", "parse", parseStats);

            const RunStats serializeStats = measure(workload.jsonText, [&workload]() {
                const std::string jsonText = workload.parsed.toString();
                consumeSize(jsonText.size());
                consumeHash(hashString(jsonText));
            });
            recordResult(results, i, "pjson", "serialize", serializeStats);

            const RunStats traverseStats = measure(
                workload.jsonText, [&workload]() { consumeHash(traversePjson(workload.parsed)); });
            recordResult(results, i, "pjson", "traverse", traverseStats);

            const RunStats copyStats = measure(workload.jsonText, [&workload]() {
                pjson copy(workload.parsed);
                consumeHash(traversePjson(copy));
                consumeSize(copy.size());
            });
            recordResult(results, i, "pjson", "copy", copyStats);
        }
    }

#ifdef PJSON_BENCH_COMPARE
    // -------------------------------------------------------------------------
    // Optional third-party traversal adapters
    // -------------------------------------------------------------------------

    // Mirrors traversePjson for nlohmann/json so traversal measurements perform
    // equivalent recursive reads and produce an observable checksum.
    std::uint64_t traverseNlohmann(const nlohmann::json& value) {
        std::uint64_t hash =
            mixHash(0x0df1cc84222325cbULL, static_cast<std::uint64_t>(value.type()));
        if (value.is_null()) {
            return mixHash(hash, 0ULL);
        }
        if (value.is_boolean()) {
            return mixHash(hash, value.get<bool>() ? 1ULL : 0ULL);
        }
        if (value.is_number_integer()) {
            return mixHash(hash, static_cast<std::uint64_t>(value.get<long long>()));
        }
        if (value.is_number_unsigned()) {
            return mixHash(hash, static_cast<std::uint64_t>(value.get<unsigned long long>()));
        }
        if (value.is_number_float()) {
            const double number = value.get<double>();
            std::uint64_t bits = 0;
            std::memcpy(&bits, &number, sizeof(bits));
            return mixHash(hash, bits);
        }
        if (value.is_string()) {
            return mixHash(hash, hashString(value.get<std::string>()));
        }
        if (value.is_array()) {
            hash = mixHash(hash, static_cast<std::uint64_t>(value.size()));
            for (nlohmann::json::const_iterator it = value.begin(); it != value.end(); ++it) {
                hash = mixHash(hash, traverseNlohmann(*it));
            }
            return hash;
        }
        if (value.is_object()) {
            hash = mixHash(hash, static_cast<std::uint64_t>(value.size()));
            for (nlohmann::json::const_iterator it = value.begin(); it != value.end(); ++it) {
                hash = mixHash(hash, hashString(it.key()));
                hash = mixHash(hash, traverseNlohmann(it.value()));
            }
            return hash;
        }
        return hash;
    }

    // Mirrors the traversal workload for RapidJSON, preserving explicit string
    // lengths so embedded null bytes are included in the checksum.
    std::uint64_t traverseRapidJson(const rapidjson::Value& value) {
        std::uint64_t hash =
            mixHash(0x1f1236bb5aa45d11ULL, static_cast<std::uint64_t>(value.GetType()));
        if (value.IsNull()) {
            return mixHash(hash, 0ULL);
        }
        if (value.IsBool()) {
            return mixHash(hash, value.GetBool() ? 1ULL : 0ULL);
        }
        if (value.IsInt64()) {
            return mixHash(hash, static_cast<std::uint64_t>(value.GetInt64()));
        }
        if (value.IsUint64()) {
            return mixHash(hash, value.GetUint64());
        }
        if (value.IsNumber()) {
            const double number = value.GetDouble();
            std::uint64_t bits = 0;
            std::memcpy(&bits, &number, sizeof(bits));
            return mixHash(hash, bits);
        }
        if (value.IsString()) {
            return mixHash(hash,
                           hashString(std::string(value.GetString(), value.GetStringLength())));
        }
        if (value.IsArray()) {
            hash = mixHash(hash, static_cast<std::uint64_t>(value.Size()));
            for (rapidjson::Value::ConstValueIterator it = value.Begin(); it != value.End(); ++it) {
                hash = mixHash(hash, traverseRapidJson(*it));
            }
            return hash;
        }
        if (value.IsObject()) {
            hash = mixHash(hash, static_cast<std::uint64_t>(value.MemberCount()));
            for (rapidjson::Value::ConstMemberIterator it = value.MemberBegin();
                 it != value.MemberEnd(); ++it) {
                hash = mixHash(hash, hashString(std::string(it->name.GetString(),
                                                            it->name.GetStringLength())));
                hash = mixHash(hash, traverseRapidJson(it->value));
            }
            return hash;
        }
        return hash;
    }

    // Mirrors the traversal workload for simdjson's DOM API. Accessors can report
    // errors, so a failed scalar read contributes only the already mixed-in type.
    std::uint64_t traverseSimdjson(simdjson::dom::element element) {
        simdjson::dom::element_type type = element.type();
        std::uint64_t hash = mixHash(0xc3137b0f3a6f1ae7ULL, static_cast<std::uint64_t>(type));
        switch (type) {
            case simdjson::dom::element_type::ARRAY: {
                simdjson::dom::array array = element.get_array();
                std::size_t count = 0;
                for (simdjson::dom::array::iterator it = array.begin(); it != array.end(); ++it) {
                    ++count;
                    hash = mixHash(hash, traverseSimdjson(*it));
                }
                return mixHash(hash, static_cast<std::uint64_t>(count));
            }
            case simdjson::dom::element_type::OBJECT: {
                simdjson::dom::object object = element.get_object();
                std::size_t count = 0;
                for (simdjson::dom::object::iterator it = object.begin(); it != object.end();
                     ++it) {
                    ++count;
                    std::string_view key = it.key();
                    hash = mixHash(hash, hashString(std::string(key.data(), key.size())));
                    hash = mixHash(hash, traverseSimdjson(it.value()));
                }
                return mixHash(hash, static_cast<std::uint64_t>(count));
            }
            case simdjson::dom::element_type::INT64: {
                int64_t value = 0;
                if (element.get(value)) {
                    return hash;
                }
                return mixHash(hash, static_cast<std::uint64_t>(value));
            }
            case simdjson::dom::element_type::UINT64: {
                uint64_t value = 0;
                if (element.get(value)) {
                    return hash;
                }
                return mixHash(hash, value);
            }
            case simdjson::dom::element_type::DOUBLE: {
                double number = 0.0;
                if (element.get(number)) {
                    return hash;
                }
                std::uint64_t bits = 0;
                std::memcpy(&bits, &number, sizeof(bits));
                return mixHash(hash, bits);
            }
            case simdjson::dom::element_type::STRING: {
                std::string_view value;
                if (element.get(value)) {
                    return hash;
                }
                return mixHash(hash, hashString(std::string(value.data(), value.size())));
            }
            case simdjson::dom::element_type::BOOL: {
                bool value = false;
                if (element.get(value)) {
                    return hash;
                }
                return mixHash(hash, value ? 1ULL : 0ULL);
            }
            case simdjson::dom::element_type::NULL_VALUE:
                return mixHash(hash, 0ULL);
        }
        return hash;
    }

    // -------------------------------------------------------------------------
    // Optional cross-library benchmark cases
    // -------------------------------------------------------------------------

    // Records equivalent nlohmann/json, RapidJSON, and simdjson cases for each
    // workload. Prepared DOMs live through every timed lambda; in particular, the
    // simdjson parser must outlive the element views it owns.
    void runCompareBenchmarks(const std::vector<Workload>& workloads,
                              std::vector<BenchmarkResult>& results) {
        for (std::size_t i = 0; i < workloads.size(); ++i) {
            const Workload& workload = workloads[i];

            // nlohmann/json: parse owns a fresh DOM each iteration; the remaining
            // cases use this pre-parsed DOM to isolate their respective operations.
            const RunStats nlohmannParse = measure(workload.jsonText, [&workload]() {
                nlohmann::json parsed = nlohmann::json::parse(workload.jsonText);
                consumeHash(traverseNlohmann(parsed));
            });
            recordResult(results, i, "nlohmann", "parse", nlohmannParse);

            nlohmann::json nlohmannParsed = nlohmann::json::parse(workload.jsonText);
            const RunStats nlohmannSerialize = measure(workload.jsonText, [&nlohmannParsed]() {
                const std::string jsonText = nlohmannParsed.dump();
                consumeSize(jsonText.size());
                consumeHash(hashString(jsonText));
            });
            recordResult(results, i, "nlohmann", "serialize", nlohmannSerialize);

            const RunStats nlohmannTraverse = measure(workload.jsonText, [&nlohmannParsed]() {
                consumeHash(traverseNlohmann(nlohmannParsed));
            });
            recordResult(results, i, "nlohmann", "traverse", nlohmannTraverse);

            const RunStats nlohmannCopy = measure(workload.jsonText, [&nlohmannParsed]() {
                nlohmann::json copy = nlohmannParsed;
                consumeHash(traverseNlohmann(copy));
                consumeSize(copy.size());
            });
            recordResult(results, i, "nlohmann", "copy", nlohmannCopy);

            // RapidJSON follows the same split. CopyFrom performs the comparable
            // allocator-aware deep copy measured by the copy case.
            const RunStats rapidjsonParse = measure(workload.jsonText, [&workload]() {
                rapidjson::Document document;
                document.Parse(workload.jsonText.c_str(), workload.jsonText.size());
                if (document.HasParseError()) {
                    std::cerr << "rapidjson parse failed for " << workload.name << "\n";
                    std::exit(1);
                }
                consumeHash(traverseRapidJson(document));
            });
            recordResult(results, i, "rapidjson", "parse", rapidjsonParse);

            rapidjson::Document rapidjsonParsed;
            rapidjsonParsed.Parse(workload.jsonText.c_str(), workload.jsonText.size());
            if (rapidjsonParsed.HasParseError()) {
                std::cerr << "rapidjson parse failed for " << workload.name << "\n";
                std::exit(1);
            }
            const RunStats rapidjsonSerialize = measure(workload.jsonText, [&rapidjsonParsed]() {
                rapidjson::StringBuffer buffer;
                rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
                rapidjsonParsed.Accept(writer);
                consumeSize(buffer.GetSize());
                consumeHash(hashString(std::string(buffer.GetString(), buffer.GetSize())));
            });
            recordResult(results, i, "rapidjson", "serialize", rapidjsonSerialize);

            const RunStats rapidjsonTraverse = measure(workload.jsonText, [&rapidjsonParsed]() {
                consumeHash(traverseRapidJson(rapidjsonParsed));
            });
            recordResult(results, i, "rapidjson", "traverse", rapidjsonTraverse);

            const RunStats rapidjsonCopy = measure(workload.jsonText, [&rapidjsonParsed]() {
                rapidjson::Document copy;
                copy.CopyFrom(rapidjsonParsed, copy.GetAllocator());
                consumeHash(traverseRapidJson(copy));
                consumeSize(copy.IsArray() ? copy.Size()
                                           : (copy.IsObject() ? copy.MemberCount() : 0));
            });
            recordResult(results, i, "rapidjson", "copy", rapidjsonCopy);

            // simdjson parsing includes a fresh parser because a DOM element borrows
            // parser-owned storage. The prepared parser below stays alive for the
            // serialization and traversal measurements that reuse its element.
            const RunStats simdjsonParse = measure(workload.jsonText, [&workload]() {
                simdjson::dom::parser parser;
                simdjson::dom::element doc;
                simdjson::error_code error = parser.parse(workload.jsonText).get(doc);
                if (error) {
                    std::cerr << "simdjson parse failed for " << workload.name << ": "
                              << simdjson::error_message(error) << "\n";
                    std::exit(1);
                }
                consumeHash(traverseSimdjson(doc));
            });
            recordResult(results, i, "simdjson", "parse", simdjsonParse);

            simdjson::dom::parser simdjsonParser;
            simdjson::dom::element simdjsonParsed;
            simdjson::error_code simdjsonError =
                simdjsonParser.parse(workload.jsonText).get(simdjsonParsed);
            if (simdjsonError) {
                std::cerr << "simdjson parse failed for " << workload.name << ": "
                          << simdjson::error_message(simdjsonError) << "\n";
                std::exit(1);
            }
            const RunStats simdjsonSerialize = measure(workload.jsonText, [&simdjsonParsed]() {
                const std::string jsonText = simdjson::minify(simdjsonParsed);
                consumeSize(jsonText.size());
                consumeHash(hashString(jsonText));
            });
            recordResult(results, i, "simdjson", "serialize", simdjsonSerialize);

            const RunStats simdjsonTraverse = measure(workload.jsonText, [&simdjsonParsed]() {
                consumeHash(traverseSimdjson(simdjsonParsed));
            });
            recordResult(results, i, "simdjson", "traverse", simdjsonTraverse);
            // simdjson intentionally has no copy row: its borrowed DOM does not offer
            // an owned mutable deep-copy operation comparable to the other libraries.
        }
    }
#endif

} // namespace

// Parses command-line inputs, prepares workloads, records enabled implementations,
// and renders the accumulated results only after every timed case has completed.
int main(int argc, char** argv) {
    // --- Parse command-line inputs --------------------------------------------
    std::vector<std::string> inputFiles;
    bool requestCompare = false;
    std::string jsonOutput;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        }
        if (arg == "--compare") {
            requestCompare = true;
            continue;
        }
        if (arg == "--json") {
            if (i + 1 >= argc) {
                std::cerr << "--json requires a file path or '-'\n";
                return 2;
            }
            jsonOutput = argv[++i];
            continue;
        }
        if (arg.compare(0, 7, "--json=") == 0) {
            jsonOutput = arg.substr(7);
            if (jsonOutput.empty()) {
                std::cerr << "--json requires a file path or '-'\n";
                return 2;
            }
            continue;
        }
        if (arg == "--input") {
            if (i + 1 >= argc) {
                std::cerr << "--input requires a file path\n";
                return 2;
            }
            inputFiles.push_back(argv[++i]);
            continue;
        }
        if (arg.compare(0, 8, "--input=") == 0) {
            inputFiles.push_back(arg.substr(8));
            continue;
        }
        // Bare arguments are accepted as corpus paths for direct invocation.
        inputFiles.push_back(arg);
    }

    // --- Prepare and run every enabled implementation -------------------------
    const std::vector<Workload> workloads = buildWorkloads(inputFiles);
    if (workloads.empty()) {
        std::cerr << "no benchmark workloads available\n";
        return 1;
    }

    const bool jsonOnly = jsonOutput == "-";
    std::ostream& progress = jsonOnly ? std::cerr : std::cout;
    progress << "pjson benchmark suite\n";
    progress << "generated workloads: small, medium, large, wide-object, large-array, "
                "string-heavy, escape-heavy, integer-heavy, floating-heavy";
    if (!inputFiles.empty()) {
        progress << " | requested extra inputs: " << inputFiles.size();
    }
    if (requestCompare) {
        progress << " | compare requested";
    }
    progress << "\n";

    std::vector<BenchmarkResult> results;
    runPjsonBenchmarks(workloads, results);

#ifdef PJSON_BENCH_COMPARE
    if (requestCompare) {
        runCompareBenchmarks(workloads, results);
    }
#else
    if (requestCompare) {
        std::cerr << "compare mode requested, but this benchmark binary was built without "
                     "PJSON_BENCH_COMPARE enabled\n";
        return 2;
    }
#endif

    // --- Render grouped results and the anti-optimization checksum -------------
    if (!jsonOnly) {
        printResultsByCase(workloads, results);
        std::cout << std::string(126, '-') << "\n";
        std::cout << "sink=" << g_sink_size << "/" << g_sink_hash
                  << " (anti-optimization checksum; not a performance measurement)\n";
    }
    if (!jsonOutput.empty() &&
        !writeMachineReport(jsonOutput, buildMachineReport(workloads, results, requestCompare))) {
        return 1;
    }
    return 0;
}
