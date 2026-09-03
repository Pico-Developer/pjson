#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ByteDance Ltd. and/or its affiliates
# SPDX-License-Identifier: Apache-2.0

"""Compare two pjson-benchmark v1 reports without overclaiming precision."""

import argparse
import json
import sys


def load(path):
    with open(path, encoding="utf-8") as stream:
        report = json.load(stream)
    if report.get("format") != "pjson-benchmark" or report.get("format_version") != 1:
        raise ValueError(f"{path}: unsupported benchmark report format")
    return report


def environment_key(report):
    environment = report.get("environment", {})
    build = report.get("build", {})
    return {
        "label": environment.get("label"),
        "operating_system": environment.get("operating_system"),
        "architecture": environment.get("architecture"),
        "cpu": environment.get("cpu"),
        "allocator": environment.get("allocator"),
        "compiler_id": build.get("compiler_id"),
        "compiler_version": build.get("compiler_version"),
        "build_type": build.get("type"),
        "flags": build.get("flags"),
    }


def source_key(report):
    source = report.get("source", {})
    return source.get("fingerprint") or source.get("commit")


def indexed(report):
    return {
        (row["library"], row["workload"], row["operation"]): row
        for row in report.get("results", [])
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("baseline")
    parser.add_argument("candidate")
    parser.add_argument("--threshold-percent", type=float, default=10.0)
    parser.add_argument("--allow-environment-mismatch", action="store_true")
    parser.add_argument("--fail-on-regression", action="store_true")
    args = parser.parse_args()

    baseline = load(args.baseline)
    candidate = load(args.candidate)
    before_environment = environment_key(baseline)
    after_environment = environment_key(candidate)
    differences = [
        key for key in before_environment if before_environment[key] != after_environment[key]
    ]
    if differences and not args.allow_environment_mismatch:
        print("benchmark reports are not from comparable environments:", file=sys.stderr)
        for key in differences:
            print(
                f"  {key}: {before_environment[key]!r} != {after_environment[key]!r}",
                file=sys.stderr,
            )
        return 2
    if source_key(baseline) == source_key(candidate):
        print("warning: reports have the same source fingerprint", file=sys.stderr)

    before = indexed(baseline)
    after = indexed(candidate)
    common = sorted(set(before) & set(after))
    if not common:
        print("benchmark reports have no comparable cases", file=sys.stderr)
        return 2

    regressions = 0
    print("library workload operation baseline_us candidate_us change status")
    for key in common:
        old = float(before[key]["median_ns"])
        new = float(after[key]["median_ns"])
        change = ((new / old) - 1.0) * 100.0 if old > 0.0 else 0.0
        status = "REGRESSION" if change > args.threshold_percent else "ok"
        if status == "REGRESSION":
            regressions += 1
        print(
            f"{key[0]} {key[1]} {key[2]} {old / 1000:.2f} {new / 1000:.2f} "
            f"{change:+.1f}% {status}"
        )

    print(
        f"compared={len(common)} regressions={regressions} "
        f"threshold={args.threshold_percent:.1f}%"
    )
    return 1 if regressions and args.fail_on_regression else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
        print(error, file=sys.stderr)
        sys.exit(2)
