# Chapter 12 — Custom allocators {#custom-allocators}

Most applications should use pjson's built-in allocator. If an embedded system,
arena, pool, or allocation tracer needs control over persistent DOM storage,
implement `pjson::Allocator` and bind values to it. Follow along with
[`examples/src/09_custom_allocator.cpp`](../examples/src/09_custom_allocator.cpp).

## Default ownership

Without an allocator argument, a value uses pjson's built-in allocator:

```cpp
pjson value;
pJsonParser::Error error;
pjson parsed = pJsonParser().parse(R"({"answer":42})", error);
```

The direct value is owned by its C++ scope. The parse result is a plain `pjson`
value returned by every DOM parse overload, so it is also released
automatically when it goes out of scope. Children returned by `find()` or
`findPointer()` are borrowed views into the owning tree—never delete them
yourself.

Every pjson value is allocator-bound, including default-constructed values. A
custom allocator changes where selected persistent DOM allocations come from; it
does not change the tree's ownership model.

## Implementing `pjson::Allocator`

Derive from the runtime interface and implement both operations:

```cpp
class PoolAllocator : public pjson::Allocator {
public:
    void* allocate(size_t bytes, size_t alignment, AllocationKind kind) override;
    void deallocate(void* memory, size_t bytes, size_t alignment,
                    AllocationKind kind) noexcept override;
};
```

The contract is:

- `allocate` returns non-null storage satisfying the requested byte size and
  alignment, or throws (normally `std::bad_alloc`). Returning `nullptr` is not a
  supported failure signal.
- `deallocate` receives the original pointer and matching size, alignment, and
  kind. It must not throw.
- The allocator object is borrowed. It must outlive every root and descendant
  bound to it, including roots returned by allocator-aware parsing.
- If one allocator instance is shared between threads, that allocator is
  responsible for whatever synchronization its implementation requires.

`AllocationKind` lets a pool maintain separate free lists or statistics:

| Kind | Persistent allocation represented |
|---|---|
| `NodeAllocation` | A dynamically owned `pjson` node, including a parsed root |
| `StringAllocation` | The `std::string` wrapper for a string-valued node |
| `ArrayAllocation` | The internal wrapper for an array-valued node |
| `ObjectAllocation` | The internal wrapper for an object-valued node |

The hook deliberately does not replace every allocation in the process. The
internal buffers/nodes allocated by `std::string`, `std::vector`, and `std::map`,
and transient parsing, serialization, pointer, patch, and validation workspaces,
continue to use the standard allocator.

## Direct roots versus parsed roots

A directly constructed root remains owned by the place where it was created:

```cpp
PoolAllocator pool;

{
    pjson document(pool); // document itself is on the stack
    document["name"] = "Ada";
    // Heap-backed wrappers and descendants use pool.
} // document's destructor returns its bound storage to pool
```

Parsing must allocate the root's descendants dynamically, but every overload
returns the document **by value**, bound to the supplied allocator:

```cpp
pJsonParser::Error error;
pJsonParser::Options options;
pjson document = pJsonParser(pool, options).parse(text, error);
if (!error.ok) {
    std::cerr << error.line << ':' << error.column << ": "
              << error.message << '\n';
}
```

The returned value is bound to `pool`: its wrapper objects and descendants were
obtained from `pool`, and its destructor returns them through `pool`. There is
no smart pointer and no manual `delete`. Moving the value transfers the tree but
does not own or extend the allocator's lifetime.

An allocator-configured parser accepts `std::string`, `(const char*, size_t)`,
and `std::istream`, with optional `pJsonParser::Error`. Its `Options` are fixed
at construction. `parseStream()` uses standard allocation for its temporary
input buffer but uses the supplied allocator for the persistent DOM. SAX
parsing builds no persistent DOM; the parser's allocator is unused by SAX calls.

## Copy, move, assignment, and swap

Allocator provenance is part of a value's lifetime contract:

| Operation | Allocator behavior |
|---|---|
| `pjson copy(source)` | Deep copy using `source`'s allocator |
| `pjson copy(source, destinationAllocator)` | Deep copy into the named allocator |
| `destination = source` / `copyFrom(source)` | Deep copy while preserving the destination allocator |
| `pjson moved(std::move(source))` | O(1) transfer with the source allocator; source becomes null |
| `pjson moved(std::move(source), destinationAllocator)` | O(1) when allocators match; otherwise deep-transfer, then source becomes null |
| `destination = std::move(source)` | Preserves the destination allocator; same-allocator storage transfer may still destroy the old destination tree, while cross-allocator transfer may allocate |
| `left.swap(right)` | O(1) only when `left.canSwap(right)`; otherwise a safe no-op |

Check compatibility whenever two values may have come from different allocator
domains:

```cpp
if (left.canSwap(right)) {
    left.swap(right);
} else {
    left = right; // deep copy into left's allocator
}
```

Do not infer allocator ownership from equality or value type. Use
`&value.getAllocator()` when provenance matters. A successfully moved-from
source is JSON null but remains bound to its original allocator.

## Failure behavior

Allocator-aware in-memory parsing catches failures during DOM construction,
destroys partial trees, returns a JSON `null` value, and fills `pJsonParser::Error` when
supplied. `parseStream()` first fills a standard-allocated input buffer, so an
exception-enabled stream or failure in that buffer can still throw before DOM
construction.

Other operations that allocate—such as string/container mutation, deep copy,
and cross-allocator move—may propagate `std::bad_alloc`. Copy assignment,
cross-allocator move assignment, `resetTo`, string replacement, missing-key
insertion, and array growth preserve existing data in their documented/tested
failure paths. JSON Patch and Merge Patch are `noexcept`; allocation or
internal failures are reported through `false` and `PatchError`, and the target
is left unchanged.

An allocator should remain usable while failed operations unwind, because pjson
may need it to release partially constructed nodes.

## Complete counting example

The companion example implements a small counting allocator using global
`operator new`/`delete`. It is intentionally an instrumentation example, not an
arena implementation:

```cpp
CountingAllocator storage;
{
    pjson direct(storage);
    direct["kind"] = "direct root";

    pJsonParser::Error error;
    pjson parsed = pJsonParser(storage).parse(
        R"({"kind":"parsed root","values":[1,2,3]})", error);
    if (!error.ok)
        return 1;

    pjson copy(parsed, storage);
    if (direct.canSwap(copy))
        direct.swap(copy);
}
// All custom allocations have now been returned to storage.
```

## What you learned

- Default values require no allocator setup and every DOM parse returns a plain
  `pjson` value.
- Every value is allocator-bound; a supplied `Allocator` is borrowed and must
  outlive the entire bound tree.
- Direct roots remain caller-owned, and an allocator-parsed value is bound to,
  and freed through, the allocator passed to `parse()`.
- Copies are deep, assignments preserve the destination allocator, and moves may
  allocate across allocator domains.
- `canSwap()` distinguishes the O(1) same-allocator path from a cross-allocator
  no-op.
- The hook covers persistent DOM nodes and wrapper objects, not every allocation
  made by their standard-library internals or temporary algorithms.

Return to the
[tutorial index](https://github.com/Pico-Developer/pjson/blob/main/docs/README.md),
or consult the
[browsable API reference](https://pico-developer.github.io/pjson/).
