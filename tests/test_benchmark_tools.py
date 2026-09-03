#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ByteDance Ltd. and/or its affiliates
# SPDX-License-Identifier: Apache-2.0

import json
import subprocess
import sys
import tempfile
from pathlib import Path


def report(label, median):
    return {
        "format": "pjson-benchmark",
        "format_version": 1,
        "source": {"commit": "abc", "fingerprint": "abc:123"},
        "environment": {
            "label": label,
            "operating_system": "test",
            "architecture": "test",
            "cpu": "test",
            "allocator": "test",
        },
        "build": {
            "compiler_id": "test",
            "compiler_version": "1",
            "type": "Release",
            "flags": "-O2",
        },
        "results": [
            {
                "library": "pjson",
                "workload": "small",
                "operation": "parse",
                "median_ns": median,
            }
        ],
    }


def run(command, expected):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if result.returncode != expected:
        raise SystemExit(
            f"expected exit {expected}, got {result.returncode}: {result.stdout}{result.stderr}"
        )


def main():
    root = Path(__file__).resolve().parents[1]
    comparator = root / "scripts" / "compare-benchmarks.py"
    with tempfile.TemporaryDirectory() as directory:
        directory = Path(directory)
        baseline = directory / "baseline.json"
        candidate = directory / "candidate.json"
        mismatch = directory / "mismatch.json"
        baseline.write_text(json.dumps(report("controlled", 100.0)), encoding="utf-8")
        candidate.write_text(json.dumps(report("controlled", 120.0)), encoding="utf-8")
        mismatch.write_text(json.dumps(report("different", 100.0)), encoding="utf-8")
        run([sys.executable, str(comparator), str(baseline), str(candidate)], 0)
        run(
            [
                sys.executable,
                str(comparator),
                str(baseline),
                str(candidate),
                "--threshold-percent",
                "10",
                "--fail-on-regression",
            ],
            1,
        )
        run([sys.executable, str(comparator), str(baseline), str(mismatch)], 2)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
