<!-- SPDX-FileCopyrightText: 2026 ByteDance Ltd. and/or its affiliates -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# pjson 3.0 behavioral and ABI contract

Status: normative public behavior and ABI policy for pjson 3.0.x
Applies to: `pjson.h`, `pjson_parser.h`, `pjson_schema.h`, and the
`pjson::pjson` library target

The behavioral guarantees from the
[pjson 2.0 behavioral contract](behavioral-contract-2.0.md) continue to apply
except where the 3.0 API intentionally moves parsing into `pJsonParser`. The
additive `findIndex(size_t)` lookup is the non-narrowing read path for
non-negative array indexes; signed `find(int)` retains end-relative negatives.

## ABI baseline

`PJSON_ABI_VERSION` is 3. Within compatible 3.x releases:

- `pjson` remains a two-pointer opaque handle containing a borrowed allocator
  pointer and a private implementation pointer;
- `pJsonParser` and `pJsonSchemaValidator` remain one-pointer opaque handles;
- public virtual interfaces, option/error structure layouts, enum values,
  function signatures, calling conventions, and exported symbols remain
  compatible; and
- private implementation layouts and source-file organization may change.

The ABI layout statements are enforced by compile-time size and alignment tests.

ABI compatibility applies only when producer and consumer use the same compiler
ABI, standard-library ABI, architecture, and compatible build settings. A major
version change, including a different `PJSON_ABI_VERSION` or shared-library
`SOVERSION`, is an explicit binary-compatibility boundary.

## DOM representation and lifetime

A null `pjson` uses a process-lifetime private sentinel and performs no private
implementation allocation. A non-null value allocates its implementation through
its bound allocator using `Allocator::ImplementationAllocation`. The allocator
pointer remains directly in the stable handle so null construction and move
construction remain allocation-free, moved-from values remain valid null values,
and custom allocator identity is preserved.

Container types, child ownership pointers, scalar storage, and iterative
destruction links are private implementation details. Destruction remains
iterative and allocation-free.

## Parsing

Parsing is provided by the standalone `pJsonParser` declared in
`<pjson_parser.h>`. It owns a private copy of its options and borrows its selected
allocator. Parser objects are copyable, movable, and reusable; moved-from parsers
remain usable with default options and the default allocator. `pjson` has no
dependency on the parser.

SAX cancellation and ordinary callback exceptions report `CallbackError`;
allocation failures, including `std::bad_alloc` thrown by a callback, report
`AllocationFailure`; and input-stream failures report `StreamError`. These failures
do not escape the SAX API boundary.

## Symbol visibility

`PJSON_API` marks supported binary interfaces. Shared builds export that surface
and use hidden visibility for implementation symbols. Static builds leave the
annotation empty. Consumers should not link against unexported implementation
symbols or include headers under `pjsonlib/src`. The exported CMake target,
pkg-config metadata, and Conan package propagate `PJSON_SHARED` for shared
consumers; `PJSON_BUILDING_LIBRARY` is reserved for the library build itself.
