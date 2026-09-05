#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ByteDance Ltd. and/or its affiliates
# SPDX-License-Identifier: Apache-2.0

"""Record portable build-artifact sizes alongside a benchmark report."""

import argparse
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("report", help="pjson-benchmark v1 JSON report")
    parser.add_argument("--artifact", action="append", default=[], metavar="PATH")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    with open(args.report, encoding="utf-8") as stream:
        benchmark = json.load(stream)
    if benchmark.get("format") != "pjson-benchmark" or benchmark.get("format_version") != 1:
        raise SystemExit("unsupported benchmark report format")

    artifacts = []
    for item in args.artifact:
        path = Path(item)
        if not path.is_file():
            raise SystemExit(f"artifact is not a file: {item}")
        artifacts.append({"path": item, "bytes": path.stat().st_size})

    output = {
        "format": "pjson-benchmark-aux",
        "format_version": 1,
        "source": benchmark.get("source", {}),
        "environment": benchmark.get("environment", {}),
        "build": benchmark.get("build", {}),
        "artifacts": artifacts,
        "notes": [
            "Artifact sizes are filesystem byte counts.",
            "Peak RSS and allocation counts are intentionally omitted unless a controlled platform-specific collector supplies them.",
        ],
    }
    with open(args.output, "w", encoding="utf-8") as stream:
        json.dump(output, stream, indent=2, sort_keys=True)
        stream.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
