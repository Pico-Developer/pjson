<!-- SPDX-FileCopyrightText: 2026 ByteDance Ltd. and/or its affiliates -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# pjson backlog

There are no known release-blocking correctness issues. The core library
supports strict RFC 8259 JSON, JSON Pointer, JSON Patch, JSON Merge Patch, and
the documented JSON Schema subset. Object members use private, process-seeded
hash storage; insertion, traversal, `keys()`, and serialization order are
intentionally unspecified.

## Optional JSON Schema formats

Implement additional asserted formats only when users need full format-assertion
coverage: duration, email/IDN email, hostname/IDN hostname, IRI/IRI-reference,
JSON Pointer/relative JSON Pointer, URI/URI-reference, and URI template. These
formats are optional in JSON Schema and are not required for the current
documented subset or required Draft 2020-12 vocabularies.

Arbitrary-precision numbers and cross-draft compatibility remain outside the
library's explicit numeric and dialect contracts.

## Controlled performance baseline

Before enforcing benchmark thresholds, establish a dedicated runner and stable
release baseline, then agree per-workload limits. Hosted-runner measurements
should remain advisory. Allocation counts, peak RSS, binary size, and build time
need separate measurement protocols from operation latency.

## Parser maintenance

DOM and SAX parsing still have separate token, Unicode, and container control
flows. Continue sharing small, independently testable pieces only when a defect
or maintenance cost justifies the change. Preserve error offsets, duplicate-key
policies, resource limits, streaming behavior, and the differential tests. A
wholesale parser rewrite is not currently justified.
