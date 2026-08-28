//
// Copyright 2025 ByteDance Ltd. and/or its affiliates. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License").
//
//===----------------------------------------------------------------------===//
// 09 — Bind persistent pjson DOM storage to a custom allocator.
// Referenced by docs/12-custom-allocators.md.
//
#include "pjson.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <new>
#include <utility>

using ByteDance::pjson;

// Minimal instrumentation allocator for the example. It delegates storage to
// global new/delete while counting pjson's four persistent allocation kinds.
class CountingAllocator : public pjson::Allocator {
public:
    // The following methods implement pjson's allocation contract while
    // maintaining per-kind lifetime totals and one aggregate live-block count.
    CountingAllocator()
            : _liveBlocks(0) {
        for (size_t i = 0; i < 4; ++i) {
            _allocations[i] = 0;
            _deallocations[i] = 0;
        }
    }

    void* allocate(size_t bytes, size_t alignment, AllocationKind kind) override {
        // Global operator new meets alignments through std::max_align_t in C++11.
        // A real pool would provide an over-aligned path if its contract needed one.
        if (alignment > alignof(std::max_align_t))
            throw std::bad_alloc();
        void* memory = ::operator new(bytes);
        ++_allocations[index(kind)];
        ++_liveBlocks;
        return memory;
    }

    void deallocate(void* memory, size_t bytes, size_t alignment,
                    AllocationKind kind) noexcept override {
        (void)bytes;
        (void)alignment;
        if (!memory)
            return;
        ++_deallocations[index(kind)];
        --_liveBlocks;
        ::operator delete(memory);
    }

    size_t allocations(AllocationKind kind) const { return _allocations[index(kind)]; }

    size_t deallocations(AllocationKind kind) const { return _deallocations[index(kind)]; }

    size_t liveBlocks() const { return _liveBlocks; }

private:
    // AllocationKind is deliberately contiguous, so it is a safe statistics index.
    static size_t index(AllocationKind kind) { return static_cast<size_t>(kind); }

    // Keep allocation and deallocation totals even after all live blocks have
    // been released so the example can report lifetime activity separately.
    size_t _allocations[4];
    size_t _deallocations[4];
    size_t _liveBlocks;
};

// Demonstrates default allocation, custom-bound roots, allocator-aware parsing,
// and transfers both within and across allocator domains.
int main() {
    // --- Default allocation ------------------------------------------------
    // Every parse overload uses the provenance-aware pjson::unique_ptr owner.
    pjson::unique_ptr ordinary = pjson::parse(R"({"storage":"default"})");
    if (!ordinary)
        return 1;

    // --- Custom allocator domains -----------------------------------------
    // Allocators are declared before bound values so they outlive every root
    // and descendant that may call back into them during destruction.
    CountingAllocator first;
    CountingAllocator second;
    {
        // This root object lives on the stack. Its persistent wrapper objects
        // and dynamically created children use `first`.
        pjson direct(first);
        direct["kind"] = "direct root";
        direct["values"] += int64_t(1);
        direct["values"] += int64_t(2);

        pjson::ParseError error;
        // Allocator-aware parsing returns pjson::unique_ptr; its custom deleter
        // returns the dynamically allocated root through `first`.
        pjson::unique_ptr parsed =
            pjson::parse(R"({"kind":"parsed root","values":[3,4]})", error, first);
        if (!parsed) {
            std::cerr << error.message << '\n';
            return 1;
        }

        // --- Transfer between domains -------------------------------------
        // Explicit allocator construction deep-copies into another domain.
        pjson rehomed(*parsed, second);
        if (direct.canSwap(*parsed))
            direct.swap(*parsed); // same allocator: constant-time exchange

        // Move assignment preserves the destination allocator. Because these
        // allocators differ, this may allocate while deep-transferring the tree.
        direct = std::move(rehomed);

        // These cumulative counts are printed while both allocator-bound trees
        // are still alive; live-block verification happens after destruction.
        pjson::SerializeOptions compact;
        compact.maxOutputBytes = size_t(64) * 1024 * 1024;
        std::cout << direct.toString(compact) << '\n';
        std::cout << "first node allocations: "
                  << first.allocations(pjson::Allocator::NodeAllocation) << '\n';
        std::cout << "second node allocations: "
                  << second.allocations(pjson::Allocator::NodeAllocation) << '\n';
    }

    // Leaving the inner scope destroys every custom-bound value before this
    // final balance check.
    if (first.liveBlocks() != 0 || second.liveBlocks() != 0) {
        std::cerr << "allocator leak detected\n";
        return 1;
    }
    return 0;
}
