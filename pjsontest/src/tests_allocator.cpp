//
// Copyright 2025 ByteDance Ltd. and/or its affiliates. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//===----------------------------------------------------------------------===//
// Allocator ownership, provenance, failure safety, and lifecycle tests.
//
// Public API covered by this file:
//
//   struct pjson::Allocator {
//       enum AllocationKind {
//           NodeAllocation,
//           StringAllocation,
//           ArrayAllocation,
//           ObjectAllocation
//       };
//       virtual ~Allocator();
//       virtual void* allocate(size_t aSize, size_t aAlignment,
//                              AllocationKind aKind) = 0;
//       virtual void deallocate(void* aPtr, size_t aSize, size_t aAlignment,
//                               AllocationKind aKind) noexcept = 0;
//   };
//
//   struct pjson::ValueDeleter {
//       void operator()(pjson* aValue) const noexcept;
//   };
//   typedef std::unique_ptr<pjson, ValueDeleter> unique_ptr;
//
//   explicit pjson(Allocator& aAlloc) noexcept;
//   pjson(const pjson& aFrom, Allocator& aAlloc);
//   pjson(pjson&& aFrom, Allocator& aAlloc);
//   Allocator& getAllocator() const noexcept;
//   bool canSwap(const pjson& aOther) const noexcept;
//
//   static unique_ptr parse(const std::string& aStr, Allocator& aAlloc,
//                           const ParseOptions& aOpts = ParseOptions());
//   static unique_ptr parse(const char* aSrc, size_t aSize, Allocator& aAlloc,
//                           const ParseOptions& aOpts = ParseOptions());
//   static unique_ptr parse(const std::string& aStr, ParseError& aError,
//                           Allocator& aAlloc,
//                           const ParseOptions& aOpts = ParseOptions());
//   static unique_ptr parse(const char* aSrc, size_t aSize, ParseError& aError,
//                           Allocator& aAlloc,
//                           const ParseOptions& aOpts = ParseOptions());
//   static unique_ptr parseStream(std::istream& aIn, Allocator& aAlloc,
//                                 const ParseOptions& aOpts = ParseOptions());
//   static unique_ptr parseStream(std::istream& aIn, ParseError& aError,
//                                 Allocator& aAlloc,
//                                 const ParseOptions& aOpts = ParseOptions());
//
// Semantics covered here:
//   - every node stores allocator provenance and children inherit it
//   - parse uses the supplied allocator for DOM nodes and wrapper objects
//   - parse failure unwinds all partial allocations
//   - resetTo/copy assignment/cross-allocator move assignment offer strong safety
//   - same-allocator swap is O(1) and allocation-free
//   - cross-allocator swap is explicitly rejected via canSwap()==false
//
#include "pjson.h"
#include "test_harness.h"

#include <cstddef>
#include <map>
#include <new>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace ByteDance;

namespace {

    typedef pjson::Allocator::AllocationKind AllocationKind;

    // Per-kind accounting distinguishes balanced frees from cross-kind provenance bugs.
    struct KindStats {
        size_t allocations = 0;
        size_t deallocations = 0;
        size_t liveBlocks = 0;
        size_t peakLiveBlocks = 0;
        size_t liveBytes = 0;
    };

    // Exact metadata that deallocate() must receive for each outstanding pointer.
    struct AllocationRecord {
        size_t size;
        size_t alignment;
        AllocationKind kind;
    };

    // Test allocator that records allocation metadata and can inject a failure by kind.
    // Its live-allocation table also verifies that deallocation repeats the original contract.
    class TrackingAllocator : public pjson::Allocator {
    public:
        explicit TrackingAllocator(const std::string& aName)
                : _name(aName) {
            clearFailures();
        }

        virtual void* allocate(size_t aSize, size_t aAlignment, AllocationKind aKind) {
            if (_armedFailures[aKind] >= 0) {
                if (_armedFailures[aKind] == 0) {
                    throw std::bad_alloc();
                }
                --_armedFailures[aKind];
            }

            void* ptr = ::operator new(aSize);
            AllocationRecord rec = {aSize, aAlignment, aKind};
            _live[ptr] = rec;

            KindStats& s = _stats[aKind];
            ++s.allocations;
            ++s.liveBlocks;
            s.liveBytes += aSize;
            if (s.liveBlocks > s.peakLiveBlocks) {
                s.peakLiveBlocks = s.liveBlocks;
            }
            return ptr;
        }

        virtual void deallocate(void* aPtr, size_t aSize, size_t aAlignment,
                                AllocationKind aKind) noexcept {
            std::map<void*, AllocationRecord>::iterator it = _live.find(aPtr);
            if (it == _live.end()) {
                ++_badDeallocations;
                return;
            }
            if (it->second.size != aSize || it->second.alignment != aAlignment ||
                it->second.kind != aKind) {
                ++_badDeallocations;
            }

            KindStats& s = _stats[it->second.kind];
            ++s.deallocations;
            if (s.liveBlocks > 0) {
                --s.liveBlocks;
            }
            if (s.liveBytes >= it->second.size) {
                s.liveBytes -= it->second.size;
            } else {
                s.liveBytes = 0;
                ++_badDeallocations;
            }

            _live.erase(it);
            ::operator delete(aPtr);
        }

        void failAfter(AllocationKind aKind, size_t aSuccessfulAllocsBeforeThrow) {
            _armedFailures[aKind] = static_cast<long>(aSuccessfulAllocsBeforeThrow);
        }

        // Disarms every failure point without disturbing lifetime counters.
        void clearFailures() {
            for (int i = 0; i < 4; ++i) {
                _armedFailures[static_cast<AllocationKind>(i)] = -1;
            }
        }

        const KindStats& stats(AllocationKind aKind) const {
            std::map<AllocationKind, KindStats>::const_iterator it = _stats.find(aKind);
            if (it != _stats.end()) {
                return it->second;
            }
            static const KindStats empty;
            return empty;
        }

        size_t liveBlockCount() const { return _live.size(); }

        size_t badDeallocations() const { return _badDeallocations; }

        const std::string& name() const { return _name; }

    private:
        std::string _name;
        std::map<void*, AllocationRecord> _live;
        std::map<AllocationKind, KindStats> _stats;
        std::map<AllocationKind, long> _armedFailures;
        size_t _badDeallocations = 0;
    };

    // Checks both global and per-kind balance after all values using an allocator die.
    static void checkAllocatorHealth(const TrackingAllocator& aAlloc) {
        CHECK_EQ(aAlloc.badDeallocations(), size_t(0));
        CHECK_EQ(aAlloc.liveBlockCount(), size_t(0));
        CHECK_EQ(aAlloc.stats(pjson::Allocator::NodeAllocation).liveBlocks, size_t(0));
        CHECK_EQ(aAlloc.stats(pjson::Allocator::StringAllocation).liveBlocks, size_t(0));
        CHECK_EQ(aAlloc.stats(pjson::Allocator::ArrayAllocation).liveBlocks, size_t(0));
        CHECK_EQ(aAlloc.stats(pjson::Allocator::ObjectAllocation).liveBlocks, size_t(0));
    }

    // Walks iteratively so allocator-provenance checks remain safe for deeply nested values.
    static void checkTreeAllocator(const pjson& aRoot, pjson::Allocator& aExpected) {
        std::vector<const pjson*> work;
        work.push_back(&aRoot);
        while (!work.empty()) {
            const pjson* cur = work.back();
            work.pop_back();
            CHECK_EQ(&cur->getAllocator(), &aExpected);
            if (cur->isArray()) {
                for (size_t i = 0; i < cur->size(); ++i) {
                    const pjson* child = cur->find(static_cast<int>(i));
                    CHECK(child != nullptr);
                    if (child != nullptr)
                        work.push_back(child);
                }
            } else if (cur->isObject()) {
                const std::vector<std::string> keys = cur->keys();
                for (size_t i = 0; i < keys.size(); ++i) {
                    const pjson* child = cur->find(keys[i]);
                    CHECK(child != nullptr);
                    if (child != nullptr)
                        work.push_back(child);
                }
            }
        }
    }

    static int64_t intValue(const pjson& aValue) {
        int64_t value = 0;
        CHECK(aValue.tryGet(value));
        return value;
    }

    static bool boolValue(const pjson& aValue) {
        bool value = false;
        CHECK(aValue.tryGet(value));
        return value;
    }

    static std::string stringValue(const pjson& aValue) {
        std::string value;
        CHECK(aValue.tryGet(value));
        return value;
    }

    // The following adapters turn allocation failure into a boolean while leaving each test
    // responsible for checking the operation's strong exception-safety invariant.
    static bool throwsBadAllocDuringAssignString(pjson& aValue, const std::string& aText) {
        try {
            aValue = aText;
            return false;
        } catch (const std::bad_alloc&) {
            return true;
        }
    }

    static bool throwsBadAllocDuringReset(pjson& aValue, pjson::jsonType aType) {
        try {
            aValue.resetTo(aType);
            return false;
        } catch (const std::bad_alloc&) {
            return true;
        }
    }

    static bool throwsBadAllocDuringCopyAssign(pjson& aDst, const pjson& aSrc) {
        try {
            aDst = aSrc;
            return false;
        } catch (const std::bad_alloc&) {
            return true;
        }
    }

    static bool throwsBadAllocDuringMoveAssign(pjson& aDst, pjson& aSrc) {
        try {
            aDst = std::move(aSrc);
            return false;
        } catch (const std::bad_alloc&) {
            return true;
        }
    }

    static bool throwsBadAllocDuringMissingKeyInsert(pjson& aRoot) {
        try {
            aRoot["newKey"]["leaf"] = int64_t(7);
            return false;
        } catch (const std::bad_alloc&) {
            return true;
        }
    }

    static bool throwsBadAllocDuringArrayGrowth(pjson& aRoot) {
        try {
            aRoot[4] = int64_t(9);
            return false;
        } catch (const std::bad_alloc&) {
            return true;
        }
    }

    // Preserve the custom-deleter return type while keeping parse-overload tests concise.
    static pjson::unique_ptr parseWithAllocator(const std::string& aText,
                                                TrackingAllocator& aAlloc) {
        return pjson::parse(aText, aAlloc);
    }

    static pjson::unique_ptr parseWithAllocator(const std::string& aText, pjson::ParseError& aErr,
                                                TrackingAllocator& aAlloc) {
        return pjson::parse(aText, aErr, aAlloc);
    }

} // namespace

//===----------------------------------------------------------------------===//
// Allocator API shape and allocation provenance
//===----------------------------------------------------------------------===//

static_assert(std::is_abstract<pjson::Allocator>::value,
              "pjson::Allocator must be an abstract runtime interface");
static_assert(std::has_virtual_destructor<pjson::Allocator>::value,
              "pjson::Allocator must have a virtual destructor");
static_assert(std::is_constructible<pjson, TrackingAllocator&>::value,
              "pjson must support explicit allocator construction");
static_assert(std::is_constructible<pjson, const pjson&, TrackingAllocator&>::value,
              "pjson must support deep copy into an explicit allocator");
static_assert(std::is_constructible<pjson, pjson&&, TrackingAllocator&>::value,
              "pjson must support cross-allocator move construction");

TEST(allocator_api_surface_is_explicit_and_non_template) {
    TrackingAllocator alloc("api");
    pjson value(alloc);
    CHECK_EQ(&value.getAllocator(), &alloc);
    CHECK(value.canSwap(value));

    pjson copied(value, alloc);
    CHECK_EQ(&copied.getAllocator(), &alloc);

    pjson moved(std::move(copied), alloc);
    CHECK_EQ(&moved.getAllocator(), &alloc);
}

TEST(allocator_mutation_tracks_nodes_strings_arrays_and_objects) {
    TrackingAllocator alloc("mutate");
    {
        pjson doc(alloc);
        doc["name"] = std::string("ada");
        doc["scores"] += int64_t(1);
        doc["scores"] += int64_t(2);
        doc["meta"]["admin"] = true;
        doc["meta"]["pi"] = double(3.5);

        CHECK(doc.isObject());
        CHECK_EQ(stringValue(doc["name"]), std::string("ada"));
        CHECK_EQ(doc["scores"].size(), size_t(2));
        CHECK(boolValue(doc["meta"]["admin"]));

        checkTreeAllocator(doc, alloc);
        CHECK(alloc.stats(pjson::Allocator::NodeAllocation).allocations >= size_t(4));
        CHECK(alloc.stats(pjson::Allocator::StringAllocation).allocations >= size_t(1));
        CHECK(alloc.stats(pjson::Allocator::ArrayAllocation).allocations >= size_t(1));
        CHECK(alloc.stats(pjson::Allocator::ObjectAllocation).allocations >= size_t(1));
    }

    checkAllocatorHealth(alloc);
}

//===----------------------------------------------------------------------===//
// DOM parsing, teardown, and allocator-aware erase/reset
//===----------------------------------------------------------------------===//

TEST(allocator_parse_success_uses_supplied_allocator_for_dom) {
    TrackingAllocator alloc("parse-ok");
    {
        pjson::unique_ptr doc =
            parseWithAllocator(R"({"name":"ada","list":[1,2,3],"obj":{"flag":true}})", alloc);
        CHECK(doc != nullptr);
        std::string name;
        int64_t third = 0;
        bool flag = false;
        CHECK(doc->tryGet("name", name));
        CHECK_EQ(name, std::string("ada"));
        CHECK(doc->find("list")->tryGet(2, third));
        CHECK_EQ(third, int64_t(3));
        CHECK(doc->find("obj")->tryGet("flag", flag));
        CHECK(flag);

        checkTreeAllocator(*doc, alloc);
        CHECK(alloc.stats(pjson::Allocator::NodeAllocation).allocations >= size_t(6));
        CHECK(alloc.stats(pjson::Allocator::StringAllocation).allocations >= size_t(1));
        CHECK(alloc.stats(pjson::Allocator::ArrayAllocation).allocations >= size_t(1));
        CHECK(alloc.stats(pjson::Allocator::ObjectAllocation).allocations >= size_t(2));
    }

    checkAllocatorHealth(alloc);
}

TEST(allocator_parse_failure_unwinds_partials_and_keeps_balance) {
    TrackingAllocator alloc("parse-fail");
    pjson::ParseError err;
    pjson::unique_ptr doc = parseWithAllocator(R"({"a":[1,2,{"b":[3,4,})", err, alloc);
    CHECK(doc == nullptr);
    CHECK(!err.ok);
    CHECK(!err.message.empty());

    checkAllocatorHealth(alloc);
}

TEST(allocator_parse_bad_alloc_returns_null_and_reports_error) {
    TrackingAllocator alloc("parse-oom");
    alloc.failAfter(pjson::Allocator::NodeAllocation, 2);

    pjson::ParseError err;
    pjson::unique_ptr doc = parseWithAllocator(R"({"a":[1,2,3],"b":{"c":"text"}})", err, alloc);
    CHECK(doc == nullptr);
    CHECK(!err.ok);
    CHECK(err.message.find("memory") != std::string::npos ||
          err.message.find("alloc") != std::string::npos);

    checkAllocatorHealth(alloc);
}

TEST(allocator_erase_and_reset_release_removed_storage_with_correct_provenance) {
    TrackingAllocator alloc("erase-reset");
    {
        pjson doc(alloc);
        doc["drop"]["name"] = "gone";
        doc["drop"]["items"] += int64_t(1);
        doc["keep"] = int64_t(7);

        const size_t nodeAllocsBeforeErase =
            alloc.stats(pjson::Allocator::NodeAllocation).allocations;
        const size_t nodeFreesBeforeErase =
            alloc.stats(pjson::Allocator::NodeAllocation).deallocations;
        CHECK(doc.erase("drop"));
        CHECK_EQ(doc.size(), size_t(1));
        CHECK(doc.hasKey("keep"));
        CHECK(!doc.hasKey("drop"));
        CHECK_EQ(alloc.stats(pjson::Allocator::NodeAllocation).allocations, nodeAllocsBeforeErase);
        CHECK(alloc.stats(pjson::Allocator::NodeAllocation).deallocations > nodeFreesBeforeErase);

        doc.resetTo(pjson::jsonString);
        doc = std::string("reused");
        CHECK(doc.isString());
        CHECK_EQ(stringValue(doc), std::string("reused"));
        CHECK_EQ(&doc.getAllocator(), &alloc);
    }

    checkAllocatorHealth(alloc);
}

//===----------------------------------------------------------------------===//
// Copy, move, and swap behavior within and across allocator domains
//===----------------------------------------------------------------------===//

TEST(allocator_copy_construction_and_copy_assignment_rehome_to_destination_allocator) {
    TrackingAllocator sourceAlloc("copy-src");
    TrackingAllocator destAlloc("copy-dst");

    {
        pjson source(sourceAlloc);
        source["name"] = "ada";
        source["nums"] += int64_t(1);
        source["nums"] += int64_t(2);
        source["meta"]["ok"] = true;

        pjson copied(source, destAlloc);
        CHECK(copied == source);
        checkTreeAllocator(source, sourceAlloc);
        checkTreeAllocator(copied, destAlloc);

        pjson assigned(destAlloc);
        assigned["old"] = "value";
        assigned = source;
        CHECK(assigned == source);
        checkTreeAllocator(assigned, destAlloc);
        checkTreeAllocator(source, sourceAlloc);
    }

    checkAllocatorHealth(sourceAlloc);
    checkAllocatorHealth(destAlloc);
}

TEST(allocator_copy_assignment_bad_alloc_preserves_destination_and_source) {
    TrackingAllocator sourceAlloc("copy-oom-src");
    TrackingAllocator destAlloc("copy-oom-dst");

    {
        pjson source(sourceAlloc);
        source["name"] = "ada";
        source["nested"]["list"] += int64_t(1);
        source["nested"]["list"] += int64_t(2);

        pjson dest(destAlloc);
        dest["old"] = "value";
        const std::string beforeDest = dest.toString();
        const std::string beforeSource = source.toString();

        destAlloc.failAfter(pjson::Allocator::NodeAllocation, 0);
        CHECK(throwsBadAllocDuringCopyAssign(dest, source));
        CHECK_EQ(dest.toString(), beforeDest);
        CHECK_EQ(source.toString(), beforeSource);
        checkTreeAllocator(dest, destAlloc);
        checkTreeAllocator(source, sourceAlloc);
    }

    checkAllocatorHealth(sourceAlloc);
    checkAllocatorHealth(destAlloc);
}

TEST(allocator_same_allocator_move_and_swap_do_not_allocate) {
    TrackingAllocator alloc("move-swap-same");
    {
        pjson left(alloc);
        left["left"] = int64_t(1);
        pjson right(alloc);
        right["right"] = int64_t(2);

        const size_t allocsBeforeSwap = alloc.stats(pjson::Allocator::NodeAllocation).allocations;
        CHECK(left.canSwap(right));
        left.swap(right);
        CHECK(left.hasKey("right"));
        CHECK(right.hasKey("left"));
        CHECK_EQ(alloc.stats(pjson::Allocator::NodeAllocation).allocations, allocsBeforeSwap);

        pjson moved(std::move(left));
        CHECK(moved.hasKey("right"));
        CHECK(left.isNull());

        pjson target(alloc);
        target["old"] = int64_t(9);
        const size_t allocsBeforeMoveAssign =
            alloc.stats(pjson::Allocator::NodeAllocation).allocations;
        target = std::move(right);
        CHECK(target.hasKey("left"));
        CHECK(right.isNull());
        CHECK_EQ(alloc.stats(pjson::Allocator::NodeAllocation).allocations, allocsBeforeMoveAssign);
    }

    checkAllocatorHealth(alloc);
}

TEST(allocator_cross_allocator_move_rehomes_to_destination_allocator) {
    TrackingAllocator sourceAlloc("move-src");
    TrackingAllocator destAlloc("move-dst");

    {
        pjson source(sourceAlloc);
        source["name"] = "ada";
        source["items"] += int64_t(1);
        source["items"] += int64_t(2);

        pjson moved(std::move(source), destAlloc);
        CHECK(moved.hasKey("name"));
        CHECK_EQ(intValue(moved["items"][1]), int64_t(2));
        CHECK(source.isNull());
        checkTreeAllocator(moved, destAlloc);

        pjson source2(sourceAlloc);
        source2["flag"] = true;
        pjson dest(destAlloc);
        dest["old"] = "v";
        dest = std::move(source2);
        CHECK(dest.hasKey("flag"));
        CHECK(source2.isNull());
        checkTreeAllocator(dest, destAlloc);
    }

    checkAllocatorHealth(sourceAlloc);
    checkAllocatorHealth(destAlloc);
}

TEST(allocator_cross_allocator_move_assignment_bad_alloc_preserves_both_values) {
    TrackingAllocator sourceAlloc("move-oom-src");
    TrackingAllocator destAlloc("move-oom-dst");

    {
        pjson source(sourceAlloc);
        source["arr"] += int64_t(1);
        source["arr"] += int64_t(2);

        pjson dest(destAlloc);
        dest["old"] = "value";
        const std::string beforeSource = source.toString();
        const std::string beforeDest = dest.toString();

        destAlloc.failAfter(pjson::Allocator::NodeAllocation, 0);
        CHECK(throwsBadAllocDuringMoveAssign(dest, source));
        CHECK_EQ(source.toString(), beforeSource);
        CHECK_EQ(dest.toString(), beforeDest);
        checkTreeAllocator(source, sourceAlloc);
        checkTreeAllocator(dest, destAlloc);
    }

    checkAllocatorHealth(sourceAlloc);
    checkAllocatorHealth(destAlloc);
}

TEST(allocator_cross_allocator_swap_is_explicitly_rejected) {
    TrackingAllocator a("swap-a");
    TrackingAllocator b("swap-b");

    {
        pjson left(a);
        left["x"] = int64_t(1);
        pjson right(b);
        right["y"] = int64_t(2);

        CHECK(!left.canSwap(right));
        CHECK(!right.canSwap(left));
        left.swap(right);
        CHECK(left.hasKey("x"));
        CHECK(right.hasKey("y"));
        checkTreeAllocator(left, a);
        checkTreeAllocator(right, b);
    }

    checkAllocatorHealth(a);
    checkAllocatorHealth(b);
}

//===----------------------------------------------------------------------===//
// Strong exception safety under injected allocation failures
//===----------------------------------------------------------------------===//

TEST(allocator_resetto_allocation_failure_preserves_previous_value) {
    TrackingAllocator alloc("reset-oom");
    {
        pjson value(alloc);
        value = static_cast<int64_t>(7);
        alloc.failAfter(pjson::Allocator::StringAllocation, 0);
        CHECK(throwsBadAllocDuringReset(value, pjson::jsonString));
        CHECK(value.isInt());
        CHECK_EQ(intValue(value), int64_t(7));

        alloc.clearFailures();
        value["k"] = int64_t(1);
        const std::string before = value.toString();
        alloc.failAfter(pjson::Allocator::ArrayAllocation, 0);
        CHECK(throwsBadAllocDuringReset(value, pjson::jsonArray));
        CHECK_EQ(value.toString(), before);
        CHECK(value.isObject());
    }

    checkAllocatorHealth(alloc);
}

TEST(allocator_missing_key_insert_failure_keeps_document_unchanged) {
    TrackingAllocator alloc("insert-oom");
    {
        pjson value(alloc);
        value["keep"] = int64_t(1);
        const std::string before = value.toString();

        alloc.failAfter(pjson::Allocator::NodeAllocation, 0);
        CHECK(throwsBadAllocDuringMissingKeyInsert(value));
        CHECK_EQ(value.toString(), before);
        CHECK(value.hasKey("keep"));
        CHECK(!value.hasKey("newKey"));
    }

    checkAllocatorHealth(alloc);
}

TEST(allocator_array_growth_failure_keeps_existing_prefix_unchanged) {
    TrackingAllocator alloc("array-grow-oom");
    {
        pjson arr(alloc);
        arr += int64_t(1);
        arr += int64_t(2);
        const std::string before = arr.toString();

        alloc.failAfter(pjson::Allocator::NodeAllocation, 0);
        CHECK(throwsBadAllocDuringArrayGrowth(arr));
        CHECK_EQ(arr.toString(), before);
        CHECK_EQ(arr.size(), size_t(2));
        CHECK_EQ(intValue(arr[0]), int64_t(1));
        CHECK_EQ(intValue(arr[1]), int64_t(2));
    }

    checkAllocatorHealth(alloc);
}

TEST(allocator_string_assignment_failure_keeps_old_value) {
    TrackingAllocator alloc("string-assign-oom");
    {
        pjson value(alloc);
        value = static_cast<int64_t>(5);
        alloc.failAfter(pjson::Allocator::StringAllocation, 0);
        CHECK(throwsBadAllocDuringAssignString(value, "hello"));
        CHECK(value.isInt());
        CHECK_EQ(intValue(value), int64_t(5));
    }

    checkAllocatorHealth(alloc);
}

//===----------------------------------------------------------------------===//
// Root deletion and allocator propagation through higher-level APIs
//===----------------------------------------------------------------------===//

TEST(allocator_default_and_custom_root_deleters_match_allocation_origin) {
    TrackingAllocator alloc("root-delete");
    {
        pjson::unique_ptr doc = pjson::parse(R"({"default":[1,2]})");
        CHECK(doc != nullptr);
        CHECK(&doc->getAllocator() != &alloc);
    }
    {
        pjson::unique_ptr ordinaryNode(new pjson());
        (*ordinaryNode)["value"] = int64_t(1);
    }

    {
        pjson::unique_ptr doc = pjson::parse(R"({"custom":[1,2]})", alloc);
        CHECK(doc != nullptr);
        CHECK_EQ(&doc->getAllocator(), &alloc);
        checkTreeAllocator(*doc, alloc);
    }
    checkAllocatorHealth(alloc);
}

TEST(allocator_patch_and_merge_patch_create_nodes_in_destination_allocator) {
    TrackingAllocator destination("patch-destination");
    TrackingAllocator source("patch-source");
    {
        pjson target(destination);
        target["keep"] = int64_t(1);

        pjson patch(source);
        patch[0]["op"] = "add";
        patch[0]["path"] = "/added";
        patch[0]["value"]["nested"] = "text";
        CHECK(target.applyPatch(patch));
        CHECK_EQ(stringValue(target["added"]["nested"]), std::string("text"));
        checkTreeAllocator(target, destination);
        checkTreeAllocator(patch, source);

        pjson merge(source);
        merge["merged"]["list"] += int64_t(3);
        merge["merged"]["list"] += int64_t(4);
        CHECK(target.applyMergePatch(merge));
        CHECK_EQ(intValue(target["merged"]["list"][1]), int64_t(4));
        checkTreeAllocator(target, destination);
        checkTreeAllocator(merge, source);
    }
    checkAllocatorHealth(destination);
    checkAllocatorHealth(source);
}

TEST(allocator_all_dom_parse_overloads_use_custom_root_deletion) {
    TrackingAllocator alloc("parse-overloads");
    {
        const std::string text = R"({"value":[1,2,3]})";
        pjson::ParseOptions opts;
        pjson::ParseError error;

        pjson::unique_ptr fromBuffer = pjson::parse(text.data(), text.size(), alloc, opts);
        CHECK(fromBuffer != nullptr);
        checkTreeAllocator(*fromBuffer, alloc);

        pjson::unique_ptr fromBufferError =
            pjson::parse(text.data(), text.size(), error, alloc, opts);
        CHECK(fromBufferError != nullptr);
        CHECK(error.ok);
        checkTreeAllocator(*fromBufferError, alloc);

        std::istringstream firstStream(text);
        pjson::unique_ptr fromStream = pjson::parseStream(firstStream, alloc, opts);
        CHECK(fromStream != nullptr);
        checkTreeAllocator(*fromStream, alloc);

        std::istringstream secondStream(text);
        pjson::unique_ptr fromStreamError = pjson::parseStream(secondStream, error, alloc, opts);
        CHECK(fromStreamError != nullptr);
        CHECK(error.ok);
        checkTreeAllocator(*fromStreamError, alloc);
    }
    checkAllocatorHealth(alloc);
}

TEST(allocator_patch_and_merge_patch_oom_leave_destination_unchanged) {
    TrackingAllocator destination("patch-oom-destination");
    TrackingAllocator source("patch-oom-source");
    {
        pjson target(destination);
        target["keep"] = int64_t(1);
        target["nested"]["old"] = true;
        const std::string before = target.toString();

        pjson patch(source);
        patch[0]["op"] = "add";
        patch[0]["path"] = "/added";
        patch[0]["value"]["nested"] = "text";

        destination.failAfter(pjson::Allocator::NodeAllocation, 0);
        pjson::PatchError patchError;
        CHECK(!target.applyPatch(patch, patchError));
        CHECK_EQ(patchError.code, pjson::PatchError::AllocationFailure);
        CHECK_EQ(target.toString(), before);
        checkTreeAllocator(target, destination);

        destination.clearFailures();
        pjson merge(source);
        merge["new"]["leaf"] = int64_t(4);
        destination.failAfter(pjson::Allocator::ObjectAllocation, 0);
        pjson::PatchError mergeError;
        CHECK(!target.applyMergePatch(merge, mergeError));
        CHECK_EQ(mergeError.code, pjson::PatchError::AllocationFailure);
        CHECK_EQ(target.toString(), before);
        checkTreeAllocator(target, destination);
    }
    checkAllocatorHealth(destination);
    checkAllocatorHealth(source);
}
