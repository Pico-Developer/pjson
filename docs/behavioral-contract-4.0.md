<!-- SPDX-FileCopyrightText: 2026 ByteDance Ltd. and/or its affiliates -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# pjson 4.0 behavioral and ABI contract

Status: normative public behavior and ABI policy for pjson 4.0.x
Applies to: `pjson.h`, `pjson_parser.h`, `pjson_schema.h`, and the
`pjson::pjson` library target

The behavioral guarantees from the
[pjson 2.0 behavioral contract](behavioral-contract-2.0.md) and the opaque-handle
design from the [pjson 3.0 contract](behavioral-contract-3.0.md) continue to apply
except for the object-order changes below.

## Object representation and order

Objects use private, process-seeded hash-table storage. Lookup and insertion are
average constant time. JSON object members are semantically unordered, and pjson
does not retain insertion order or impose a sorted order. `keys()`,
`forEachMember()`, `toString()`, and `write()` use unspecified native storage
order. Callers must compare parsed values rather than serialized object bytes
unless they apply their own canonicalization layer. Array order remains stable
and semantically significant.

`SerializeOptions::KeyOrder` and its `keyOrder` field have been removed. This is
a source and ABI break from 3.x.

## ABI baseline

`PJSON_ABI_VERSION` is 4. Within compatible 4.x releases:

- `pjson` remains a two-pointer opaque handle containing a borrowed allocator
  pointer and a private implementation pointer;
- `pJsonParser` and `pJsonSchemaValidator` remain one-pointer opaque handles;
- public virtual interfaces, option/error structure layouts, enum values,
  function signatures, calling conventions, and exported symbols remain
  compatible; and
- private implementation layouts and source-file organization may change.

ABI compatibility applies only when producer and consumer use the same compiler
ABI, standard-library ABI, architecture, and compatible build settings. A major
version change, including a different `PJSON_ABI_VERSION` or shared-library
`SOVERSION`, is an explicit binary-compatibility boundary.

## Public surface and ownership

The installed headers remain declaration-focused. Container representations and
the keyed hash implementation are private implementation details. Ownership,
allocator behavior, parser and schema-validator separation, structured errors,
resource limits, and symbol visibility otherwise retain the 3.0 contract.
