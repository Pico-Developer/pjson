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
// Author: Praveen Babu J D
// License: Apache 2.0
//
#include "pjson_internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <random>
#include <stdexcept>
#include <type_traits>
#include <utility>

using namespace ByteDance;

/*static*/
const char* pjson::getVersion() {
    return PJSON_VERSION;
}

namespace {
    uint64_t mixObjectHashSeed(uint64_t value) noexcept {
        value ^= value >> 30U;
        value *= UINT64_C(0xbf58476d1ce4e5b9);
        value ^= value >> 27U;
        value *= UINT64_C(0x94d049bb133111eb);
        return value ^ (value >> 31U);
    }

    struct ObjectHashKey {
        uint64_t first;
        uint64_t second;
    };

    const ObjectHashKey& objectHashKey() noexcept {
        static const ObjectHashKey key = []() {
            const uint64_t clock = static_cast<uint64_t>(
                std::chrono::high_resolution_clock::now().time_since_epoch().count());
            const uintptr_t address = reinterpret_cast<uintptr_t>(&objectHashKey);
            ObjectHashKey result = {
                mixObjectHashSeed(clock ^ static_cast<uint64_t>(address)),
                mixObjectHashSeed(~clock ^ (static_cast<uint64_t>(address) << 1U))};
            try {
                std::random_device random;
                const uint64_t first = (static_cast<uint64_t>(random()) << 32U) ^ random();
                const uint64_t second = (static_cast<uint64_t>(random()) << 32U) ^ random();
                result.first = mixObjectHashSeed(result.first ^ first);
                result.second = mixObjectHashSeed(result.second ^ second);
            } catch (...) {
                // Clock and ASLR-derived state remain a non-throwing fallback.
                (void)0;
            }
            return result;
        }();
        return key;
    }

    uint64_t rotateLeft(uint64_t value, unsigned int shift) noexcept {
        return (value << shift) | (value >> (64U - shift));
    }

    void sipRound(uint64_t& v0, uint64_t& v1, uint64_t& v2, uint64_t& v3) noexcept {
        v0 += v1;
        v1 = rotateLeft(v1, 13U);
        v1 ^= v0;
        v0 = rotateLeft(v0, 32U);
        v2 += v3;
        v3 = rotateLeft(v3, 16U);
        v3 ^= v2;
        v0 += v3;
        v3 = rotateLeft(v3, 21U);
        v3 ^= v0;
        v2 += v1;
        v1 = rotateLeft(v1, 17U);
        v1 ^= v2;
        v2 = rotateLeft(v2, 32U);
    }

    // Adapts the process-wide operator new/delete pair to the allocator API.
    class DefaultPjsonAllocator : public pjson::Allocator {
    public:
        // Allocates raw storage; size is the only parameter needed by operator new.
        void* allocate(size_t aSize, size_t, AllocationKind) override {
            return ::operator new(aSize);
        }

        // Releases storage previously obtained from allocate.
        void deallocate(void* aPtr, size_t, size_t, AllocationKind) noexcept override {
            ::operator delete(aPtr);
        }
    };

    template <typename T>
    // Constructs an internal DOM object and returns raw storage on constructor failure.
    T* allocateDomObject(pjson::Allocator& aAlloc, pjson::Allocator::AllocationKind aKind) {
        void* storage = aAlloc.allocate(sizeof(T), alignof(T), aKind);
        try {
            return new (storage) T();
        } catch (...) {
            aAlloc.deallocate(storage, sizeof(T), alignof(T), aKind);
            throw;
        }
    }

    template <typename T>
    // Runs an internal object's destructor before returning its exact allocation.
    void destroyDomObject(pjson::Allocator& aAlloc, T* aObject,
                          pjson::Allocator::AllocationKind aKind) noexcept {
        if (aObject == nullptr)
            return;
        aObject->~T();
        aAlloc.deallocate(aObject, sizeof(T), alignof(T), aKind);
    }
} // namespace

// SipHash-2-4 hashes untrusted object names with a process-specific key so
// reusable collision sets cannot target the implementation's default hash.
size_t pjsonImpl::ObjectHash::operator()(const std::string& aKey) const noexcept {
    const ObjectHashKey& key = objectHashKey();
    uint64_t v0 = UINT64_C(0x736f6d6570736575) ^ key.first;
    uint64_t v1 = UINT64_C(0x646f72616e646f6d) ^ key.second;
    uint64_t v2 = UINT64_C(0x6c7967656e657261) ^ key.first;
    uint64_t v3 = UINT64_C(0x7465646279746573) ^ key.second;
    size_t offset = 0;
    while (aKey.size() - offset >= size_t(8)) {
        uint64_t word = 0;
        for (unsigned int byte = 0; byte < 8U; ++byte)
            word |= static_cast<uint64_t>(static_cast<unsigned char>(aKey[offset + byte]))
                    << (byte * 8U);
        v3 ^= word;
        sipRound(v0, v1, v2, v3);
        sipRound(v0, v1, v2, v3);
        v0 ^= word;
        offset += 8;
    }
    uint64_t tail = static_cast<uint64_t>(aKey.size()) << 56U;
    for (size_t byte = 0; offset + byte < aKey.size(); ++byte)
        tail |= static_cast<uint64_t>(static_cast<unsigned char>(aKey[offset + byte]))
                << (byte * 8U);
    v3 ^= tail;
    sipRound(v0, v1, v2, v3);
    sipRound(v0, v1, v2, v3);
    v0 ^= tail;
    v2 ^= UINT64_C(0xff);
    for (unsigned int round = 0; round < 4U; ++round)
        sipRound(v0, v1, v2, v3);
    return static_cast<size_t>(v0 ^ v1 ^ v2 ^ v3);
}

// Gives allocator implementations a safe virtual destruction point.
pjson::Allocator::~Allocator() {}
/*static*/
// Returns the stateless process-lifetime allocator used by ordinary values.
pjson::Allocator& pjsonImpl::_defaultAllocator() noexcept {
    static DefaultPjsonAllocator allocator;
    return allocator;
}
/*static*/
// Returns the immutable-by-convention process-lifetime representation shared by
// every null value. Mutating paths materialize a private implementation first.
pjsonImpl& pjsonImpl::_nullImpl() noexcept {
    static pjsonImpl nullValue;
    return nullValue;
}
/*static*/
bool pjsonImpl::_isNullImpl(const pjsonImpl* aImpl) noexcept {
    return aImpl == &_nullImpl();
}
/*static*/
pjsonImpl* pjsonImpl::_allocateImpl(pjson::Allocator& aAlloc) {
    return allocateDomObject<pjsonImpl>(aAlloc, pjson::Allocator::ImplementationAllocation);
}
/*static*/
void pjsonImpl::_destroyImpl(pjson::Allocator& aAlloc, pjsonImpl* aImpl) noexcept {
    if (!_isNullImpl(aImpl))
        destroyDomObject(aAlloc, aImpl, pjson::Allocator::ImplementationAllocation);
}
/*static*/
// Constructs a child node in allocator storage. _destroyNode is used only for
// nodes created here; caller-created roots are destroyed with normal delete.
pjson* pjsonImpl::_allocateNode(pjson::Allocator& aAlloc) {
    void* storage =
        aAlloc.allocate(sizeof(pjson), alignof(pjson), pjson::Allocator::NodeAllocation);
    try {
        return new (storage) pjson(aAlloc);
    } catch (...) {
        aAlloc.deallocate(storage, sizeof(pjson), alignof(pjson), pjson::Allocator::NodeAllocation);
        throw;
    }
}
/*static*/
// Destroys a library-created child node through its retained allocator.
void pjsonImpl::_destroyNode(pjson* aValue) noexcept {
    if (aValue == nullptr)
        return;
    pjson::Allocator& allocator = *aValue->_allocator;
    aValue->~pjson();
    allocator.deallocate(aValue, sizeof(pjson), alignof(pjson), pjson::Allocator::NodeAllocation);
}

//===----------------------------------------------------------------------===//
// DOM value lifetime, storage transfer, and type access
//
// A pjson's allocator identity is immutable. Contents may be transferred in
// constant time only between values using the same allocator; cross-allocator
// moves become deep copies so every descendant remains owned consistently.
//===----------------------------------------------------------------------===//

// Constructs an allocation-free null root using the default allocator.
pjson::pjson() noexcept
        : _allocator(&pjsonImpl::_defaultAllocator())
        , _pImpl(&pjsonImpl::_nullImpl()) {}
// Constructs an allocation-free null root backed by a caller allocator.
pjson::pjson(Allocator& aAlloc) noexcept
        : _allocator(&aAlloc)
        , _pImpl(&pjsonImpl::_nullImpl()) {}
// Releases the active value and all descendants through their retained allocator.
pjson::~pjson() {
    reset();
}
// Deep-copies a value while preserving its allocator identity.
pjson::pjson(const pjson& aFrom)
        : _allocator(aFrom._allocator)
        , _pImpl(&pjsonImpl::_nullImpl()) {
    pjsonImpl::_copyContentsInto(*this, aFrom);
}
// Deep-copies a value into a specifically selected allocator domain.
pjson::pjson(const pjson& aFrom, Allocator& aAlloc)
        : _allocator(&aAlloc)
        , _pImpl(&pjsonImpl::_nullImpl()) {
    pjsonImpl::_copyContentsInto(*this, aFrom);
}
// Steals storage from a same-allocator source and leaves it as null.
pjson::pjson(pjson&& aFrom) noexcept
        : _allocator(aFrom._allocator)
        , _pImpl(aFrom._pImpl) {
    aFrom._pImpl = &pjsonImpl::_nullImpl();
}
// Steals when allocator domains match; otherwise deep-copies into aAlloc and
// resets the source only after the copy succeeds.
pjson::pjson(pjson&& aFrom, Allocator& aAlloc)
        : _allocator(&aAlloc)
        , _pImpl(&pjsonImpl::_nullImpl()) {
    if (_allocator == aFrom._allocator) {
        _pImpl = aFrom._pImpl;
        aFrom._pImpl = &pjsonImpl::_nullImpl();
    } else {
        pjsonImpl::_copyContentsInto(*this, aFrom);
        aFrom.reset();
    }
}
// Replaces this value from an rvalue, transferring storage when allocator
// domains match after an ancestry-safety check. Self-move is a no-op.
//
// Aliasing safety (PJSON-COR-002): aFrom may be an ancestor or descendant of
// *this. A source descendant can be detached safely before the old destination
// tree is destroyed. When the destination is a descendant of the source, copy
// first because consuming the ancestor would invalidate the destination and can
// create an ownership cycle.
pjson& pjson::operator=(pjson&& aFrom) {
    if (&aFrom == this)
        return *this;
    // A destination that lives inside aFrom cannot outlive a destructive move
    // from that ancestor. Preserve value semantics by snapshotting the source
    // before replacing the descendant; a moved-from object is permitted to
    // retain its value. This also avoids creating an unreachable ownership
    // cycle such as root["child"] = std::move(root).
    if (pjsonImpl::_containsNode(aFrom, this)) {
        copyFrom(aFrom);
        return *this;
    }

    if (_allocator == aFrom._allocator) {
        pjson snapshot(*_allocator);              // null placeholder in the same allocator domain
        pjsonImpl::_swapStorage(snapshot, aFrom); // snapshot adopts aFrom's storage; aFrom -> null
        pjsonImpl::_swapStorage(*this, snapshot); // *this adopts that storage
        // snapshot's destructor frees our previous contents, which may include the
        // now-null aFrom node when aFrom was one of our descendants.
    } else {
        pjson tmp(std::move(aFrom), *_allocator);
        pjsonImpl::_swapStorage(*this, tmp);
    }

    return *this;
}
// O(1) exchange of two nodes' private representations. noexcept,
// which is what lets the move operations and copy-and-swap assignment below
// offer their exception guarantees.
//
// Aliasing safety (PJSON-COR-002): swapping a node with one of its own
// ancestors or descendants would splice a container into its own child slot and
// create an ownership cycle. Such an overlapping swap is rejected as a safe
// no-op; callers that need it should copy instead. canSwap() already rejects
// cross-allocator pairs.
void pjson::swap(pjson& aOther) noexcept {
    if (this == &aOther || !canSwap(aOther))
        return;
    pjsonImpl::_swapStorage(*this, aOther);
}
// Performs the raw storage exchange with no aliasing or allocator checks. Used
// internally where the caller has already established that the two nodes are
// distinct, non-overlapping, and share an allocator domain.
/*static*/
void pjsonImpl::_swapStorage(pjson& aLeft, pjson& aRight) noexcept {
    std::swap(aLeft._pImpl, aRight._pImpl);
}
// Reports whether aNode is aRoot or a descendant of it. The walk is iterative so
// it stays stack-safe on deep documents. Allocation failure is treated
// conservatively as overlap, preserving the noexcept callers' safety contract.
/*static*/
bool pjsonImpl::_containsNode(const pjson& aRoot, const pjson* aNode) noexcept {
    if (aNode == nullptr)
        return false;
    try {
        std::vector<const pjson*> work;
        work.push_back(&aRoot);
        while (!work.empty()) {
            const pjson* cur = work.back();
            work.pop_back();
            if (cur == aNode)
                return true;
            if (cur->_pImpl->_eType == pjson::jsonType::jsonArray) {
                const pjsonImpl::ArrayStorage& arr = *cur->_pImpl->_pValueArray;
                for (size_t i = 0; i < arr.size(); ++i)
                    work.push_back(arr[i]);
            } else if (cur->_pImpl->_eType == pjson::jsonType::jsonObject) {
                const pjsonImpl::ObjectStorage& obj = *cur->_pImpl->_pValueMap;
                for (pjsonImpl::ObjectStorage::const_iterator it = obj.begin(); it != obj.end();
                     ++it)
                    work.push_back(it->second);
            }
        }
        return false;
    } catch (...) {
        return true;
    }
}
// Returns the allocator permanently associated with this value and its descendants.
pjson::Allocator& pjson::getAllocator() const noexcept {
    return *_allocator;
}
// Reports whether contents can be exchanged without crossing allocator domains.
bool pjson::canSwap(const pjson& aOther) const noexcept {
    return _allocator == aOther._allocator &&
           (this == &aOther ||
            (!pjsonImpl::_containsNode(*this, &aOther) && !pjsonImpl::_containsNode(aOther, this)));
}
// Copy assignment (copy-and-swap: safe even when aFrom aliases a child of
// this, because the deep copy completes before any of our storage is freed).
pjson& pjson::operator=(const pjson& aFrom) {
    if (&aFrom == this)
        return *this;

    pjson tmp(aFrom, *_allocator);
    pjsonImpl::_swapStorage(*this, tmp);
    return *this;
}
// Returns the active storage tag.
pjson::jsonType pjson::getType() const {
    return _pImpl->_eType;
};
// Type predicates inspect the tag only and never coerce the stored value.
bool pjson::isNull() const {
    return _pImpl->_eType == jsonNull;
}
bool pjson::isString() const {
    return _pImpl->_eType == jsonString;
}
bool pjson::isNumber() const {
    return _pImpl->_eType == jsonNumberInt || _pImpl->_eType == jsonNumberUInt ||
           _pImpl->_eType == jsonNumberDouble;
}
bool pjson::isInt() const {
    return _pImpl->_eType == jsonNumberInt;
}
bool pjson::isUInt() const {
    return _pImpl->_eType == jsonNumberUInt;
}
bool pjson::isInteger() const {
    return _pImpl->_eType == jsonNumberInt || _pImpl->_eType == jsonNumberUInt;
}
bool pjson::isDouble() const {
    return _pImpl->_eType == jsonNumberDouble;
}
bool pjson::isBool() const {
    return _pImpl->_eType == jsonBoolean;
}
bool pjson::isArray() const {
    return _pImpl->_eType == jsonArray;
}
bool pjson::isObject() const {
    return _pImpl->_eType == jsonObject;
}
// Constructs an empty non-owning view.
pjson::StringView::StringView() noexcept
        : _data(nullptr)
        , _size(0) {}
// Constructs a non-owning byte view; the caller controls the pointed-to lifetime.
pjson::StringView::StringView(const char* aData, size_t aSize) noexcept
        : _data(aData)
        , _size(aSize) {}
// Returns the first viewed byte, which may be null for an empty default view.
const char* pjson::StringView::data() const noexcept {
    return _data;
}
// Returns the number of viewed bytes.
size_t pjson::StringView::size() const noexcept {
    return _size;
}
// Reports whether the view contains no bytes.
bool pjson::StringView::empty() const noexcept {
    return _size == 0;
}
// Exact extraction overloads leave the destination unchanged on type mismatch.
// A signed read accepts an unsigned value only when it fits in int64_t; an
// unsigned read accepts a signed value only when it is non-negative; a double
// read widens either integer representation.
bool pjson::tryGet(int64_t& aResult) const noexcept {
    if (_pImpl->_eType == pjson::jsonType::jsonNumberInt) {
        aResult = _pImpl->_valueInt;
        return true;
    }
    if (_pImpl->_eType == pjson::jsonType::jsonNumberUInt &&
        _pImpl->_valueUInt <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        aResult = static_cast<int64_t>(_pImpl->_valueUInt);
        return true;
    }
    return false;
}
bool pjson::tryGet(uint64_t& aResult) const noexcept {
    if (_pImpl->_eType == pjson::jsonType::jsonNumberUInt) {
        aResult = _pImpl->_valueUInt;
        return true;
    }
    if (_pImpl->_eType == pjson::jsonType::jsonNumberInt && _pImpl->_valueInt >= 0) {
        aResult = static_cast<uint64_t>(_pImpl->_valueInt);
        return true;
    }
    return false;
}
bool pjson::tryGet(double& aResult) const noexcept {
    if (_pImpl->_eType == pjson::jsonType::jsonNumberInt) {
        aResult = static_cast<double>(_pImpl->_valueInt);
        return true;
    }
    if (_pImpl->_eType == pjson::jsonType::jsonNumberUInt) {
        aResult = static_cast<double>(_pImpl->_valueUInt);
        return true;
    }
    if (_pImpl->_eType != pjson::jsonType::jsonNumberDouble)
        return false;
    aResult = _pImpl->_valueDouble;
    return true;
}
bool pjson::tryGet(bool& aResult) const noexcept {
    if (_pImpl->_eType != pjson::jsonType::jsonBoolean)
        return false;
    aResult = _pImpl->_valueBool;
    return true;
}
bool pjson::tryGet(std::string& aResult) const {
    if (_pImpl->_eType != pjson::jsonType::jsonString)
        return false;
    aResult = *_pImpl->_pValueString;
    return true;
}
bool pjson::tryGet(StringView& aResult) const noexcept {
    if (_pImpl->_eType != pjson::jsonType::jsonString)
        return false;
    const std::string& value = *_pImpl->_pValueString;
    aResult = StringView(value.data(), value.size());
    return true;
}
// Resets to the canonical null state, releasing any owned subtree.
void pjson::reset() {
    if (pjsonImpl::_isNullImpl(_pImpl))
        return;

    switch (_pImpl->_eType) {
        case pjson::jsonType::jsonString:
            destroyDomObject(*_allocator, _pImpl->_pValueString, Allocator::StringAllocation);
            break;
        case pjson::jsonType::jsonArray:
            pjsonImpl::_disposeChildren(*this);
            destroyDomObject(*_allocator, _pImpl->_pValueArray, Allocator::ArrayAllocation);
            break;
        case pjson::jsonType::jsonObject:
            pjsonImpl::_disposeChildren(*this);
            destroyDomObject(*_allocator, _pImpl->_pValueMap, Allocator::ObjectAllocation);
            break;
        default:
            break;
    }

    pjsonImpl* implementation = _pImpl;
    _pImpl = &pjsonImpl::_nullImpl();
    pjsonImpl::_destroyImpl(*_allocator, implementation);
}
// Idempotent reset: rebuild as an empty value of aeType only when the node is
// not already that type, so an existing array/object keeps its contents.
void pjson::resetIfNeeded(jsonType aeType) {
    if (_pImpl->_eType != aeType) {
        resetTo(aeType);
    }
}
// Rebuilds storage for aeType and initializes its empty/default value. The new
// container/string allocation occurs before teardown to preserve validity on throw.
void pjson::resetTo(pjson::jsonType aeType) {
    // Reject forged enum values before allocation or teardown so the strong
    // exception guarantee also covers an invalid requested discriminator.
    // jsonNumberUInt is the highest-valued tag (see the header enum).
    if (aeType < pjson::jsonType::jsonNull || aeType > pjson::jsonType::jsonNumberUInt)
        throw std::invalid_argument("invalid pjson::jsonType");

    if (aeType == pjson::jsonNull) {
        reset();
        return;
    }

    // Build a complete replacement before publishing it. This includes both
    // the opaque implementation and any wrapper object required by the type.
    pjson replacement(*_allocator);
    pjsonImpl* implementation = pjsonImpl::_allocateImpl(*_allocator);
    try {
        switch (aeType) {
            case pjson::jsonType::jsonString:
                implementation->_pValueString =
                    allocateDomObject<std::string>(*_allocator, Allocator::StringAllocation);
                break;
            case pjson::jsonType::jsonArray:
                implementation->_pValueArray = allocateDomObject<pjsonImpl::ArrayStorage>(
                    *_allocator, Allocator::ArrayAllocation);
                break;
            case pjson::jsonType::jsonObject:
                implementation->_pValueMap = allocateDomObject<pjsonImpl::ObjectStorage>(
                    *_allocator, Allocator::ObjectAllocation);
                break;
            case pjson::jsonType::jsonNumberInt:
                implementation->_valueInt = 0;
                break;
            case pjson::jsonType::jsonNumberUInt:
                implementation->_valueUInt = 0;
                break;
            case pjson::jsonType::jsonNumberDouble:
                implementation->_valueDouble = 0.0;
                break;
            case pjson::jsonType::jsonBoolean:
                implementation->_valueBool = false;
                break;
            default:
                break;
        }
    } catch (...) {
        pjsonImpl::_destroyImpl(*_allocator, implementation);
        throw;
    }
    implementation->_eType = aeType;
    replacement._pImpl = implementation;
    pjsonImpl::_swapStorage(*this, replacement);
}
// Replaces this value with a deep copy allocated in this value's allocator.
// Building the replacement first gives the operation a strong guarantee.
void pjson::copyFrom(const pjson& aFrom) {
    if (this == &aFrom)
        return;
    pjson replacement(aFrom, *_allocator);
    pjsonImpl::_swapStorage(*this, replacement);
}
// Populates aDst from aFrom without recursion. If copying fails, partial
// descendants are reclaimed and aDst is reset to a valid null state.
/*static*/
void pjsonImpl::_copyContentsInto(pjson& aDst, const pjson& aFrom) {
    // Iterative deep copy. A recursive copy would overflow the stack on very
    // deep documents, so we walk with an explicit work-list: each item pairs a
    // source node with the destination node to populate from it. Scalars are
    // copied immediately; array/map children are queued.
    try {
        aDst.resetTo(aFrom.getType());
        if (aDst._pImpl->_eType != pjson::jsonType::jsonArray &&
            aDst._pImpl->_eType != pjson::jsonType::jsonObject) {
            switch (aDst._pImpl->_eType) {
                case pjson::jsonType::jsonString:
                    *aDst._pImpl->_pValueString = *(aFrom._pImpl->_pValueString);
                    break;
                case pjson::jsonType::jsonNumberInt:
                    aDst._pImpl->_valueInt = aFrom._pImpl->_valueInt;
                    break;
                case pjson::jsonType::jsonNumberUInt:
                    aDst._pImpl->_valueUInt = aFrom._pImpl->_valueUInt;
                    break;
                case pjson::jsonType::jsonNumberDouble:
                    aDst._pImpl->_valueDouble = aFrom._pImpl->_valueDouble;
                    break;
                case pjson::jsonType::jsonBoolean:
                    aDst._pImpl->_valueBool = aFrom._pImpl->_valueBool;
                    break;
                default:
                    break; // null: nothing to copy
            }
            return;
        }

        struct Item {
            const pjson* src;
            pjson* dst;
        };
        std::vector<Item> work;
        Item start = {&aFrom, &aDst};
        work.push_back(start);

        while (!work.empty()) {
            Item cur = work.back();
            work.pop_back();
            const pjson& src = *cur.src;
            pjson& dst = *cur.dst;

            // dst has already been resetTo(src type) by the parent (or caller).
            if (src._pImpl->_eType == pjson::jsonType::jsonArray) {
                dst._pImpl->_pValueArray->reserve(src._pImpl->_pValueArray->size());
                for (const pjson* elem : *src._pImpl->_pValueArray) {
                    pjsonImpl::OwnedNode child = pjsonImpl::_makeNode(*dst._allocator);
                    child->resetTo(elem->getType());
                    dst._pImpl->_pValueArray->push_back(child.get());
                    pjson* attached = child.release();
                    if (elem->_pImpl->_eType == pjson::jsonType::jsonArray ||
                        elem->_pImpl->_eType == pjson::jsonType::jsonObject) {
                        Item it = {elem, attached};
                        work.push_back(it);
                    } else {
                        pjsonImpl::_copyContentsInto(*attached, *elem);
                    }
                }
            } else { // jsonObject
                for (const auto& kv : *src._pImpl->_pValueMap) {
                    pjsonImpl::OwnedNode child = pjsonImpl::_makeNode(*dst._allocator);
                    child->resetTo(kv.second->getType());
                    const std::pair<pjsonImpl::ObjectStorage::iterator, bool> inserted =
                        dst._pImpl->_pValueMap->insert(
                            std::make_pair(kv.first, static_cast<pjson*>(nullptr)));
                    if (!inserted.second)
                        throw std::logic_error("duplicate key while copying pjson object");
                    pjson* attached = child.release();
                    inserted.first->second = attached;
                    if (kv.second->_pImpl->_eType == pjson::jsonType::jsonArray ||
                        kv.second->_pImpl->_eType == pjson::jsonType::jsonObject) {
                        Item it = {kv.second, attached};
                        work.push_back(it);
                    } else {
                        pjsonImpl::_copyContentsInto(*attached, *kv.second);
                    }
                }
            }
        }
    } catch (...) {
        aDst.reset();
        throw;
    }
}
// Frees every descendant of node iteratively, so tearing down a very deep
// tree cannot overflow the call stack (as the recursive destructor would).
// Leaves node's own top-level array/map allocated but empty.
/*static*/
void pjsonImpl::_disposeChildren(pjson& node) noexcept {
    if (node._pImpl->_eType != pjson::jsonType::jsonArray &&
        node._pImpl->_eType != pjson::jsonType::jsonObject) {
        return;
    }
    // Use an intrusive pending list so teardown never allocates and therefore
    // remains noexcept even for very deep trees or an exhausted heap.
    pjson* pending = nullptr;
    if (node._pImpl->_eType == pjson::jsonType::jsonArray) {
        for (pjson* c : *node._pImpl->_pValueArray) {
            if (pjsonImpl::_isNullImpl(c->_pImpl)) {
                pjsonImpl::_destroyNode(c);
            } else {
                c->_pImpl->_disposeNext = pending;
                pending = c;
            }
        }
        node._pImpl->_pValueArray->clear();
    } else {
        for (const auto& kv : *node._pImpl->_pValueMap) {
            if (pjsonImpl::_isNullImpl(kv.second->_pImpl)) {
                pjsonImpl::_destroyNode(kv.second);
            } else {
                kv.second->_pImpl->_disposeNext = pending;
                pending = kv.second;
            }
        }
        node._pImpl->_pValueMap->clear();
    }

    while (pending != nullptr) {
        pjson* p = pending;
        pending = p->_pImpl->_disposeNext;
        p->_pImpl->_disposeNext = nullptr;
        // Move this node's children into the work-list, then detach so its own
        // destructor has nothing left to recurse into.
        if (p->_pImpl->_eType == pjson::jsonType::jsonArray) {
            for (pjson* c : *p->_pImpl->_pValueArray) {
                if (pjsonImpl::_isNullImpl(c->_pImpl)) {
                    pjsonImpl::_destroyNode(c);
                } else {
                    c->_pImpl->_disposeNext = pending;
                    pending = c;
                }
            }
            p->_pImpl->_pValueArray->clear();
        } else if (p->_pImpl->_eType == pjson::jsonType::jsonObject) {
            for (const auto& kv : *p->_pImpl->_pValueMap) {
                if (pjsonImpl::_isNullImpl(kv.second->_pImpl)) {
                    pjsonImpl::_destroyNode(kv.second);
                } else {
                    kv.second->_pImpl->_disposeNext = pending;
                    pending = kv.second;
                }
            }
            p->_pImpl->_pValueMap->clear();
        }
        pjsonImpl::_destroyNode(p); // now a leaf (or emptied container)
    }
}
// Returns the length (1..4) of the valid UTF-8 sequence starting at
// src[pos], or 0 if the bytes there are not valid UTF-8. Requires pos < end.
int pjsonImpl::_utf8Len(const char* src, size_t pos, size_t end) {
    unsigned char c0 = static_cast<unsigned char>(src[pos]);
    int n;
    uint32_t cp;
    uint32_t lo;
    if (c0 < 0x80)
        return 1;
    else if ((c0 & 0xE0) == 0xC0) {
        n = 2;
        cp = c0 & 0x1F;
        lo = 0x80;
    } else if ((c0 & 0xF0) == 0xE0) {
        n = 3;
        cp = c0 & 0x0F;
        lo = 0x800;
    } else if ((c0 & 0xF8) == 0xF0) {
        n = 4;
        cp = c0 & 0x07;
        lo = 0x10000;
    } else
        return 0; // stray continuation / invalid lead
    if (pos + static_cast<size_t>(n) > end)
        return 0;
    for (size_t k = 1; k < static_cast<size_t>(n); ++k) {
        unsigned char ck = static_cast<unsigned char>(src[pos + k]);
        if ((ck & 0xC0) != 0x80)
            return 0; // not a continuation byte
        cp = (cp << 6) | (ck & 0x3F);
    }
    if (cp < lo)
        return 0; // overlong encoding
    if (cp > 0x10FFFF)
        return 0; // beyond Unicode
    if (cp >= 0xD800 && cp <= 0xDFFF)
        return 0; // surrogate half in UTF-8
    return n;
}

/*static*/
// Allocates a null node under the supplied allocator and wraps origin-aware cleanup.
pjsonImpl::OwnedNode pjsonImpl::_makeNode(pjson::Allocator& aAlloc) {
    return pjsonImpl::OwnedNode(pjsonImpl::_allocateNode(aAlloc));
}
/*static*/
// Deep-clones a complete subtree into the supplied allocator domain.
pjsonImpl::OwnedNode pjsonImpl::_cloneNode(const pjson& aValue, pjson::Allocator& aAlloc) {
    pjsonImpl::OwnedNode result = _makeNode(aAlloc);
    _copyContentsInto(*result, aValue);
    return result;
}

//===----------------------------------------------------------------------===//
// Scalar assignment and vector-backed array mutation
//
// Scalar assignments reuse compatible storage. Array helpers allocate children
// under RAII and publish raw pointers only after container insertion succeeds.
// Multi-element mutation either swaps a complete replacement or rolls back to
// the original size, so allocation failures never leave a partial append.
//===----------------------------------------------------------------------===//

// Replaces the current value with a copied JSON string.
pjson& pjson::operator=(const std::string& aString) {
    resetIfNeeded(pjson::jsonType::jsonString);
    *_pImpl->_pValueString = aString;
    return *this;
}
// Replaces the current value with the null-terminated string's bytes.
pjson& pjson::operator=(const char* aCString) {
    if (aCString == nullptr)
        throw std::invalid_argument("pjson string assignment requires non-null input");
    resetIfNeeded(pjson::jsonType::jsonString);
    *_pImpl->_pValueString = aCString;
    return *this;
}
// Replaces the current value with a JSON boolean.
pjson& pjson::operator=(const bool aBool) {
    resetIfNeeded(pjson::jsonType::jsonBoolean);
    _pImpl->_valueBool = aBool;
    return *this;
}
// Native integer overloads keep ordinary literals unambiguous while routing
// storage through the exact-width numeric implementation.
pjson& pjson::operator=(const int aInt) {
    operator=(static_cast<int64_t>(aInt));
    return *this;
}
pjson& pjson::operator=(const unsigned int aUInt) {
    operator=(static_cast<uint64_t>(aUInt));
    return *this;
}
pjson& pjson::operator=(const short aInt) {
    operator=(static_cast<int64_t>(aInt));
    return *this;
}
pjson& pjson::operator=(const unsigned short aUInt) {
    operator=(static_cast<uint64_t>(aUInt));
    return *this;
}
// Wider native integer overloads cover both LP64 and LLP64 without relying on
// the platform-specific fundamental type selected by int64_t/uint64_t.
pjson& pjson::operator=(const long aInt) {
    resetIfNeeded(pjson::jsonType::jsonNumberInt);
    _pImpl->_valueInt = static_cast<int64_t>(aInt);
    return *this;
}
pjson& pjson::operator=(const unsigned long aUInt) {
    resetIfNeeded(pjson::jsonType::jsonNumberUInt);
    _pImpl->_valueUInt = static_cast<uint64_t>(aUInt);
    return *this;
}
pjson& pjson::operator=(const long long aInt) {
    resetIfNeeded(pjson::jsonType::jsonNumberInt);
    _pImpl->_valueInt = static_cast<int64_t>(aInt);
    return *this;
}
pjson& pjson::operator=(const unsigned long long aUInt) {
    resetIfNeeded(pjson::jsonType::jsonNumberUInt);
    _pImpl->_valueUInt = static_cast<uint64_t>(aUInt);
    return *this;
}
// Float input is widened exactly to pjson's binary64 representation.
pjson& pjson::operator=(const float aFloat) {
    operator=(static_cast<double>(aFloat));
    return *this;
}
// Replaces the current value with a JSON double.
pjson& pjson::operator=(const double aDouble) {
    resetIfNeeded(pjson::jsonType::jsonNumberDouble);
    _pImpl->_valueDouble = aDouble;
    return *this;
}
pjson& pjson::operator=(const long double aDouble) {
    operator=(static_cast<double>(aDouble));
    return *this;
}
namespace {
    // Appends one converted child. A non-array target is promoted atomically by
    // building and swapping a replacement; an array target changes only after
    // both node construction and vector growth have succeeded.
    template <typename Value> void appendDomValue(pjson& aTarget, const Value& aValue) {
        if (!aTarget.isArray()) {
            pjson replacement(aTarget.getAllocator());
            replacement.resetTo(pjson::jsonArray);
            pjsonImpl::OwnedNode child = pjsonImpl::_makeNode(aTarget.getAllocator());
            *child = aValue;
            pjsonImpl::_array(replacement).push_back(nullptr);
            pjsonImpl::_array(replacement).back() = child.release();
            pjsonImpl::_swapStorage(aTarget, replacement);
            return;
        }

        pjsonImpl::OwnedNode child = pjsonImpl::_makeNode(aTarget.getAllocator());
        *child = aValue;
        pjsonImpl::_array(aTarget).push_back(nullptr);
        pjsonImpl::_array(aTarget).back() = child.release();
    }

    // Replaces a target with a fully constructed array, providing a strong guarantee.
    template <typename Values> void assignDomArray(pjson& aTarget, const Values& aValues) {
        pjson replacement(aTarget.getAllocator());
        replacement.resetTo(pjson::jsonArray);
        for (const auto& value : aValues) {
            appendDomValue(replacement, value);
        }
        pjsonImpl::_swapStorage(aTarget, replacement);
    }

    // Appends a range atomically: newly attached children are reclaimed in
    // reverse order if any later conversion or allocation throws.
    template <typename Values> void appendDomArray(pjson& aTarget, const Values& aValues) {
        if (!aTarget.isArray()) {
            pjson replacement(aTarget.getAllocator());
            replacement.resetTo(pjson::jsonArray);
            for (const auto& value : aValues) {
                appendDomValue(replacement, value);
            }
            pjsonImpl::_swapStorage(aTarget, replacement);
            return;
        }

        pjsonImpl::ArrayStorage& array = pjsonImpl::_array(aTarget);
        const size_t originalSize = array.size();
        try {
            for (const auto& value : aValues) {
                appendDomValue(aTarget, value);
            }
        } catch (...) {
            while (array.size() > originalSize) {
                pjsonImpl::OwnedNode rollback(array.back());
                array.pop_back();
            }
            throw;
        }
    }
} // namespace
// Vector assignment overloads delegate to the same strong-guarantee builder,
// but remain explicit so each supported public type is visible in this source.
pjson& pjson::operator=(const std::vector<bool>& aValueArray) {
    assignDomArray(*this, aValueArray);
    return *this;
}

pjson& pjson::operator=(const std::vector<int>& aValueArray) {
    assignDomArray(*this, aValueArray);
    return *this;
}

pjson& pjson::operator=(const std::vector<unsigned int>& aValueArray) {
    assignDomArray(*this, aValueArray);
    return *this;
}

pjson& pjson::operator=(const std::vector<short>& aValueArray) {
    assignDomArray(*this, aValueArray);
    return *this;
}

pjson& pjson::operator=(const std::vector<unsigned short>& aValueArray) {
    assignDomArray(*this, aValueArray);
    return *this;
}

pjson& pjson::operator=(const std::vector<std::string>& aValueArray) {
    assignDomArray(*this, aValueArray);
    return *this;
}

pjson& pjson::operator=(const std::vector<long>& aValueArray) {
    assignDomArray(*this, aValueArray);
    return *this;
}

pjson& pjson::operator=(const std::vector<unsigned long>& aValueArray) {
    assignDomArray(*this, aValueArray);
    return *this;
}

pjson& pjson::operator=(const std::vector<long long>& aValueArray) {
    assignDomArray(*this, aValueArray);
    return *this;
}

pjson& pjson::operator=(const std::vector<unsigned long long>& aValueArray) {
    assignDomArray(*this, aValueArray);
    return *this;
}

pjson& pjson::operator=(const std::vector<float>& aValueArray) {
    assignDomArray(*this, aValueArray);
    return *this;
}

pjson& pjson::operator=(const std::vector<double>& aValueArray) {
    assignDomArray(*this, aValueArray);
    return *this;
}

pjson& pjson::operator=(const std::vector<long double>& aValueArray) {
    assignDomArray(*this, aValueArray);
    return *this;
}

// Scalar append overloads promote non-arrays and publish one fully constructed child.
pjson& pjson::operator+=(const std::string& aValue) {
    appendDomValue(*this, aValue);
    return *this;
}
pjson& pjson::operator+=(const char* aValue) {
    if (aValue == nullptr)
        throw std::invalid_argument("pjson string append requires non-null input");
    appendDomValue(*this, aValue);
    return *this;
}
pjson& pjson::operator+=(const bool aValue) {
    appendDomValue(*this, aValue);
    return *this;
}
pjson& pjson::operator+=(const int aValue) {
    appendDomValue(*this, static_cast<int64_t>(aValue));
    return *this;
}
pjson& pjson::operator+=(const unsigned int aValue) {
    appendDomValue(*this, static_cast<uint64_t>(aValue));
    return *this;
}
pjson& pjson::operator+=(const short aValue) {
    appendDomValue(*this, static_cast<int64_t>(aValue));
    return *this;
}
pjson& pjson::operator+=(const unsigned short aValue) {
    appendDomValue(*this, static_cast<uint64_t>(aValue));
    return *this;
}
pjson& pjson::operator+=(const long aValue) {
    appendDomValue(*this, static_cast<int64_t>(aValue));
    return *this;
}
pjson& pjson::operator+=(const unsigned long aValue) {
    appendDomValue(*this, static_cast<uint64_t>(aValue));
    return *this;
}
pjson& pjson::operator+=(const long long aValue) {
    appendDomValue(*this, static_cast<int64_t>(aValue));
    return *this;
}
pjson& pjson::operator+=(const unsigned long long aValue) {
    appendDomValue(*this, static_cast<uint64_t>(aValue));
    return *this;
}
pjson& pjson::operator+=(const float aValue) {
    appendDomValue(*this, static_cast<double>(aValue));
    return *this;
}
pjson& pjson::operator+=(const double aValue) {
    appendDomValue(*this, aValue);
    return *this;
}
pjson& pjson::operator+=(const long double aValue) {
    appendDomValue(*this, static_cast<double>(aValue));
    return *this;
}
// Vector append overloads share the rollback semantics documented by appendDomArray.
pjson& pjson::operator+=(const std::vector<bool>& aValueArray) {
    appendDomArray(*this, aValueArray);
    return *this;
}

pjson& pjson::operator+=(const std::vector<int>& aValueArray) {
    appendDomArray(*this, aValueArray);
    return *this;
}

pjson& pjson::operator+=(const std::vector<unsigned int>& aValueArray) {
    appendDomArray(*this, aValueArray);
    return *this;
}

pjson& pjson::operator+=(const std::vector<short>& aValueArray) {
    appendDomArray(*this, aValueArray);
    return *this;
}

pjson& pjson::operator+=(const std::vector<unsigned short>& aValueArray) {
    appendDomArray(*this, aValueArray);
    return *this;
}

pjson& pjson::operator+=(const std::vector<std::string>& aValueArray) {
    appendDomArray(*this, aValueArray);
    return *this;
}

pjson& pjson::operator+=(const std::vector<long>& aValueArray) {
    appendDomArray(*this, aValueArray);
    return *this;
}

pjson& pjson::operator+=(const std::vector<unsigned long>& aValueArray) {
    appendDomArray(*this, aValueArray);
    return *this;
}

pjson& pjson::operator+=(const std::vector<long long>& aValueArray) {
    appendDomArray(*this, aValueArray);
    return *this;
}

pjson& pjson::operator+=(const std::vector<unsigned long long>& aValueArray) {
    appendDomArray(*this, aValueArray);
    return *this;
}

pjson& pjson::operator+=(const std::vector<float>& aValueArray) {
    appendDomArray(*this, aValueArray);
    return *this;
}

pjson& pjson::operator+=(const std::vector<double>& aValueArray) {
    appendDomArray(*this, aValueArray);
    return *this;
}

pjson& pjson::operator+=(const std::vector<long double>& aValueArray) {
    appendDomArray(*this, aValueArray);
    return *this;
}

//===----------------------------------------------------------------------===//
// Factories, checked access, and generic child insertion
//===----------------------------------------------------------------------===//

// Explicit typed factories. Each returns a default-allocator value of the
// requested kind so callers never depend on default construction's type.
pjson pjson::null() {
    return pjson();
}
pjson pjson::object() {
    pjson value;
    value.resetTo(pjson::jsonType::jsonObject);
    return value;
}
pjson pjson::array() {
    pjson value;
    value.resetTo(pjson::jsonType::jsonArray);
    return value;
}
// Assigning nullptr resets to JSON null, matching null().
pjson& pjson::operator=(std::nullptr_t) {
    reset();
    return *this;
}

// Checked, non-vivifying object access. Throws std::out_of_range on a missing
// key or a non-object receiver, distinguishing it from vivifying operator[].
pjson& pjson::at(const std::string& aKey) {
    pjson* child = find(aKey);
    if (child == nullptr)
        throw std::out_of_range("pjson::at: object key not found");
    return *child;
}
const pjson& pjson::at(const std::string& aKey) const {
    const pjson* child = find(aKey);
    if (child == nullptr)
        throw std::out_of_range("pjson::at: object key not found");
    return *child;
}
// Checked, non-vivifying array access using a non-negative index. Throws
// std::out_of_range for a non-array receiver or an out-of-range index.
pjson& pjson::at(size_t aIndex) {
    if (_pImpl->_eType != pjson::jsonType::jsonArray || aIndex >= _pImpl->_pValueArray->size())
        throw std::out_of_range("pjson::at: array index out of range");
    return *(*_pImpl->_pValueArray)[aIndex];
}
const pjson& pjson::at(size_t aIndex) const {
    if (_pImpl->_eType != pjson::jsonType::jsonArray || aIndex >= _pImpl->_pValueArray->size())
        throw std::out_of_range("pjson::at: array index out of range");
    return *(*_pImpl->_pValueArray)[aIndex];
}

// Generic child append. Promotes a non-array target to an array, then attaches
// a deep copy (copy overload) or a moved/cross-allocator-copied value.
pjson& pjson::pushBack(const pjson& aValue) {
    // When converting a container that owns aValue, build the complete
    // replacement before destroying the old tree. This also gives all
    // non-array promotions a strong exception guarantee.
    if (_pImpl->_eType != pjson::jsonType::jsonArray) {
        pjson replacement(*_allocator);
        replacement.resetTo(pjson::jsonType::jsonArray);
        replacement.pushBack(aValue);
        pjsonImpl::_swapStorage(*this, replacement);
        return *this;
    }
    pjsonImpl::OwnedNode child = pjsonImpl::_makeNode(*_allocator);
    pjsonImpl::_copyContentsInto(*child, aValue);
    _pImpl->_pValueArray->push_back(nullptr);
    _pImpl->_pValueArray->back() = child.release();
    return *this;
}
pjson& pjson::pushBack(pjson&& aValue) {
    // Moving an ancestor into its descendant cannot consume the source without
    // invalidating the destination. Snapshot it instead; moved-from values are
    // allowed to retain their value. Self-append therefore appends a finite
    // snapshot rather than creating an ownership cycle.
    if (pjsonImpl::_containsNode(aValue, this))
        return pushBack(static_cast<const pjson&>(aValue));
    if (_pImpl->_eType != pjson::jsonType::jsonArray) {
        pjson replacement(*_allocator);
        replacement.resetTo(pjson::jsonType::jsonArray);
        replacement.pushBack(std::move(aValue));
        pjsonImpl::_swapStorage(*this, replacement);
        return *this;
    }
    pjsonImpl::OwnedNode child = pjsonImpl::_makeNode(*_allocator);
    // Reserve before consuming aValue so allocation failure leaves both values
    // logically unchanged. Child nodes are separately allocated, so a vector
    // reallocation cannot invalidate an aliased descendant source.
    if (_pImpl->_pValueArray->size() == _pImpl->_pValueArray->max_size())
        throw std::length_error("pjson array exceeds maximum size");
    _pImpl->_pValueArray->reserve(_pImpl->_pValueArray->size() + 1);
    if (child->_allocator == aValue._allocator) {
        pjsonImpl::_swapStorage(*child, aValue);
        aValue.reset();
    } else {
        pjsonImpl::_copyContentsInto(*child, aValue);
        aValue.reset();
    }
    _pImpl->_pValueArray->push_back(nullptr);
    _pImpl->_pValueArray->back() = child.release();
    return *this;
}
// Insert-or-assign an object member from an arbitrary pjson value.
pjson& pjson::insertOrAssign(const std::string& aKey, const pjson& aValue) {
    // Snapshot an ancestor (including *this) before adding/replacing its child,
    // otherwise insertion could change the very source being copied.
    if (pjsonImpl::_containsNode(aValue, this)) {
        pjson sourceCopy(aValue, *_allocator);
        return insertOrAssign(aKey, std::move(sourceCopy));
    }
    if (_pImpl->_eType != pjson::jsonType::jsonObject) {
        pjson replacement(*_allocator);
        replacement.resetTo(pjson::jsonType::jsonObject);
        replacement.insertOrAssign(aKey, aValue);
        pjsonImpl::_swapStorage(*this, replacement);
        return *this;
    }
    pjsonImpl::ObjectStorage& object = *_pImpl->_pValueMap;
    pjsonImpl::ObjectStorage::iterator existing = object.find(aKey);
    if (existing != object.end()) {
        existing->second->copyFrom(aValue);
        return *this;
    }
    pjsonImpl::OwnedNode child = pjsonImpl::_cloneNode(aValue, *_allocator);
    const std::pair<pjsonImpl::ObjectStorage::iterator, bool> inserted =
        object.insert(std::make_pair(aKey, static_cast<pjson*>(nullptr)));
    if (!inserted.second) {
        inserted.first->second->copyFrom(aValue);
        return *this;
    }
    inserted.first->second = child.release();
    return *this;
}
pjson& pjson::insertOrAssign(const std::string& aKey, pjson&& aValue) {
    if (pjsonImpl::_containsNode(aValue, this)) {
        pjson sourceCopy(aValue, *_allocator);
        return insertOrAssign(aKey, std::move(sourceCopy));
    }
    if (_pImpl->_eType != pjson::jsonType::jsonObject) {
        pjson replacement(*_allocator);
        replacement.resetTo(pjson::jsonType::jsonObject);
        replacement.insertOrAssign(aKey, std::move(aValue));
        pjsonImpl::_swapStorage(*this, replacement);
        return *this;
    }
    pjsonImpl::ObjectStorage& object = *_pImpl->_pValueMap;
    pjsonImpl::ObjectStorage::iterator existing = object.find(aKey);
    if (existing != object.end()) {
        *existing->second = std::move(aValue);
        return *this;
    }
    pjsonImpl::OwnedNode child = pjsonImpl::_makeNode(*_allocator);
    const std::pair<pjsonImpl::ObjectStorage::iterator, bool> inserted =
        object.insert(std::make_pair(aKey, child.get()));
    if (!inserted.second) {
        *inserted.first->second = std::move(aValue);
        return *this;
    }
    pjson* attached = child.release();
    if (attached->_allocator == aValue._allocator) {
        pjsonImpl::_swapStorage(*attached, aValue);
    } else {
        // Clone before consuming a cross-allocator source. If copying throws,
        // erase the newly attached null node and preserve both input values.
        try {
            pjsonImpl::_copyContentsInto(*attached, aValue);
        } catch (...) {
            pjsonImpl::_destroyNode(attached);
            object.erase(inserted.first);
            throw;
        }
        aValue.reset();
    }
    return *this;
}
// Reserves array capacity. Promotes a non-array to an empty array first so the
// reservation is always meaningful; a no-op count of zero still normalizes type.
pjson& pjson::reserve(size_t aCount) {
    if (_pImpl->_eType != pjson::jsonType::jsonArray)
        resetTo(pjson::jsonType::jsonArray);
    _pImpl->_pValueArray->reserve(aCount);
    return *this;
}
// contains() is a readable alias for hasKey().
bool pjson::contains(const std::string& aKey) const {
    return hasKey(aKey);
}
bool pjson::contains(const char* aKey) const {
    return hasKey(aKey);
}

//===----------------------------------------------------------------------===//
// Non-allocating traversal (PJSON-API-001)
//
// Callback-style visitors keep the public header declaration-only and ABI
// stable. Each visits borrowed children directly with no key copy and no second
// lookup; aContext carries caller state because a plain function pointer cannot
// capture. Returning false stops early and propagates as the call's result.
//===----------------------------------------------------------------------===//
bool pjson::forEachMember(ConstMemberVisitor aVisitor, void* aContext) const {
    if (_pImpl->_eType != pjson::jsonType::jsonObject || aVisitor == nullptr)
        return true;
    for (pjsonImpl::ObjectStorage::const_iterator it = _pImpl->_pValueMap->begin();
         it != _pImpl->_pValueMap->end(); ++it) {
        StringView keyView(it->first.data(), it->first.size());
        if (!aVisitor(keyView, static_cast<const pjson&>(*it->second), aContext))
            return false;
    }
    return true;
}
bool pjson::forEachMember(MemberVisitor aVisitor, void* aContext) {
    if (_pImpl->_eType != pjson::jsonType::jsonObject || aVisitor == nullptr)
        return true;
    for (pjsonImpl::ObjectStorage::iterator it = _pImpl->_pValueMap->begin();
         it != _pImpl->_pValueMap->end(); ++it) {
        StringView keyView(it->first.data(), it->first.size());
        if (!aVisitor(keyView, *it->second, aContext))
            return false;
    }
    return true;
}
bool pjson::forEachElement(ConstElementVisitor aVisitor, void* aContext) const {
    if (_pImpl->_eType != pjson::jsonType::jsonArray || aVisitor == nullptr)
        return true;
    const pjsonImpl::ArrayStorage& arr = *_pImpl->_pValueArray;
    for (size_t i = 0; i < arr.size(); ++i) {
        if (!aVisitor(static_cast<const pjson&>(*arr[i]), aContext))
            return false;
    }
    return true;
}
bool pjson::forEachElement(ElementVisitor aVisitor, void* aContext) {
    if (_pImpl->_eType != pjson::jsonType::jsonArray || aVisitor == nullptr)
        return true;
    pjsonImpl::ArrayStorage& arr = *_pImpl->_pValueArray;
    for (size_t i = 0; i < arr.size(); ++i) {
        if (!aVisitor(*arr[i], aContext))
            return false;
    }
    return true;
}
//===----------------------------------------------------------------------===//
// Container access and lookup
//
// Mutating operator[] access auto-vivifies missing containers and children;
// find() is non-mutating. Negative array indices count from the end. Mutating
// indices before the beginning throw without changing the receiver, while lookup
// indices simply miss.
//===----------------------------------------------------------------------===//

// Returns or creates an object member, atomically promoting non-object values.
// The std::string overload is the length-aware primary implementation so keys
// containing embedded U+0000 are preserved byte-for-byte; the const char*
// overload deliberately keeps conventional NUL-terminated semantics.
pjson& pjson::operator[](const std::string& aString) {
    if (_pImpl->_eType != pjson::jsonType::jsonObject) {
        pjson replacement(*_allocator);
        replacement.resetTo(pjson::jsonType::jsonObject);
        pjsonImpl::OwnedNode child = pjsonImpl::_makeNode(*_allocator);
        const std::pair<pjsonImpl::ObjectStorage::iterator, bool> inserted =
            replacement._pImpl->_pValueMap->insert(
                std::make_pair(aString, static_cast<pjson*>(nullptr)));
        pjson* result = child.release();
        inserted.first->second = result;
        pjsonImpl::_swapStorage(*this, replacement);
        return *result;
    }
    pjsonImpl::ObjectStorage::iterator it = _pImpl->_pValueMap->find(aString);
    if (it != _pImpl->_pValueMap->end()) {
        return *(it->second);
    }
    pjsonImpl::OwnedNode child = pjsonImpl::_makeNode(*_allocator);
    const std::pair<pjsonImpl::ObjectStorage::iterator, bool> inserted =
        _pImpl->_pValueMap->insert(std::make_pair(aString, static_cast<pjson*>(nullptr)));
    pjson* result = inserted.first->second;
    if (inserted.second) {
        result = child.release();
        inserted.first->second = result;
    }
    return *result;
}
pjson& pjson::operator[](const char* aSkey) {
    if (aSkey == nullptr)
        throw std::invalid_argument("pjson object key requires non-null input");
    return (*this)[std::string(aSkey)];
}
// Returns or creates an array element, filling gaps with null nodes. Any failed
// growth destroys every node appended by this call before rethrowing.
pjson& pjson::operator[](int index) {
    if (index < 0) {
        if (_pImpl->_eType != pjson::jsonType::jsonArray)
            throw std::out_of_range("pjson array negative index requires an existing array");
        pjsonImpl::ArrayStorage& array = *_pImpl->_pValueArray;
        const size_t fromEnd = static_cast<size_t>(-(index + 1)) + size_t(1);
        if (fromEnd > array.size())
            throw std::out_of_range("pjson array negative index out of range");
        return *array[array.size() - fromEnd];
    }
    return (*this)[static_cast<size_t>(index)];
}

// Returns or creates an array element at a non-negative index, filling gaps
// with null nodes. Any failed growth destroys nodes appended by this call.
pjson& pjson::operator[](size_t index) {
    if (_pImpl->_eType != pjson::jsonType::jsonArray) {
        pjson replacement(*_allocator);
        replacement.resetTo(pjson::jsonType::jsonArray);
        pjson& result = replacement[index];
        pjson* resultPtr = &result;
        pjsonImpl::_swapStorage(*this, replacement);
        return *resultPtr;
    }
    pjsonImpl::ArrayStorage& array = *_pImpl->_pValueArray;
    const size_t position = index;

    if (position >= array.size()) {
        static const size_t kMaxAutoGrowth = size_t(1000000);
        const size_t growth = position - array.size() + size_t(1);
        if (growth > kMaxAutoGrowth)
            throw std::length_error("pjson array auto-growth exceeds safety limit");
        if (position == std::numeric_limits<size_t>::max() ||
            position + size_t(1) > array.max_size())
            throw std::length_error("pjson array index exceeds maximum size");
        const size_t requiredSize = position + size_t(1);
        const size_t originalSize = array.size();
        try {
            array.reserve(requiredSize);
            while (array.size() < requiredSize) {
                pjsonImpl::OwnedNode child = pjsonImpl::_makeNode(*_allocator);
                array.push_back(nullptr);
                array.back() = child.release();
            }
        } catch (...) {
            while (array.size() > originalSize) {
                pjsonImpl::OwnedNode rollback(array.back());
                array.pop_back();
            }
            throw;
        }
    }

    return *array[position];
}
// Length-aware object lookup: the std::string overload is the primary form so
// keys containing embedded U+0000 resolve on their full byte sequence. The
// const char* overloads keep conventional NUL-terminated behavior.
pjson* pjson::find(const std::string& aKey) {
    if (_pImpl->_eType == pjson::jsonType::jsonObject) {
        auto it = _pImpl->_pValueMap->find(aKey);
        if (it != _pImpl->_pValueMap->end()) {
            return it->second;
        }
    }
    return nullptr;
}
// Finds an object member without inserting or changing the receiver.
pjson* pjson::find(const char* aKey) {
    if (aKey != nullptr)
        return find(std::string(aKey));
    return nullptr;
}
const pjson* pjson::find(const std::string& aKey) const {
    return const_cast<pjson*>(this)->find(aKey);
}
const pjson* pjson::find(const char* aKey) const {
    if (aKey != nullptr)
        return find(std::string(aKey));
    return nullptr;
}
pjson* pjson::find(int aIndex) noexcept {
    return const_cast<pjson*>(static_cast<const pjson*>(this)->find(aIndex));
}
// Finds an array element with end-relative negative-index support.
const pjson* pjson::find(int aIndex) const noexcept {
    if (_pImpl->_eType != pjson::jsonType::jsonArray)
        return nullptr;

    const pjsonImpl::ArrayStorage& values = *_pImpl->_pValueArray;
    size_t position = 0;
    if (aIndex >= 0) {
        position = static_cast<size_t>(aIndex);
        if (position >= values.size())
            return nullptr;
    } else {
        const size_t fromEnd = static_cast<size_t>(-(aIndex + 1)) + size_t(1);
        if (fromEnd > values.size())
            return nullptr;
        position = values.size() - fromEnd;
    }
    return values[position];
}
pjson* pjson::findIndex(size_t aIndex) noexcept {
    return const_cast<pjson*>(static_cast<const pjson*>(this)->findIndex(aIndex));
}
// Finds a non-negative array index without narrowing to int.
const pjson* pjson::findIndex(size_t aIndex) const noexcept {
    if (_pImpl->_eType != pjson::jsonType::jsonArray || aIndex >= _pImpl->_pValueArray->size())
        return nullptr;
    return (*_pImpl->_pValueArray)[aIndex];
}

// Key/index extraction overloads combine non-mutating lookup with exact
// tryGet conversion and leave output parameters unchanged on any miss. The
// std::string forms are length-aware so embedded-NUL keys resolve correctly.
bool pjson::tryGet(const std::string& aKey, int64_t& aResult) const {
    const pjson* value = find(aKey);
    return value != nullptr && value->tryGet(aResult);
}
bool pjson::tryGet(const std::string& aKey, uint64_t& aResult) const {
    const pjson* value = find(aKey);
    return value != nullptr && value->tryGet(aResult);
}
bool pjson::tryGet(const std::string& aKey, double& aResult) const {
    const pjson* value = find(aKey);
    return value != nullptr && value->tryGet(aResult);
}
bool pjson::tryGet(const std::string& aKey, bool& aResult) const {
    const pjson* value = find(aKey);
    return value != nullptr && value->tryGet(aResult);
}
bool pjson::tryGet(const std::string& aKey, std::string& aResult) const {
    const pjson* value = find(aKey);
    return value != nullptr && value->tryGet(aResult);
}
bool pjson::tryGet(const std::string& aKey, StringView& aResult) const {
    const pjson* value = find(aKey);
    return value != nullptr && value->tryGet(aResult);
}
bool pjson::tryGet(const char* aKey, int64_t& aResult) const {
    const pjson* value = find(aKey);
    return value != nullptr && value->tryGet(aResult);
}
bool pjson::tryGet(const char* aKey, uint64_t& aResult) const {
    const pjson* value = find(aKey);
    return value != nullptr && value->tryGet(aResult);
}
bool pjson::tryGet(const char* aKey, double& aResult) const {
    const pjson* value = find(aKey);
    return value != nullptr && value->tryGet(aResult);
}
bool pjson::tryGet(const char* aKey, bool& aResult) const {
    const pjson* value = find(aKey);
    return value != nullptr && value->tryGet(aResult);
}
bool pjson::tryGet(const char* aKey, std::string& aResult) const {
    const pjson* value = find(aKey);
    return value != nullptr && value->tryGet(aResult);
}
bool pjson::tryGet(const char* aKey, StringView& aResult) const {
    const pjson* value = find(aKey);
    return value != nullptr && value->tryGet(aResult);
}
bool pjson::tryGet(int aIndex, int64_t& aResult) const noexcept {
    const pjson* value = find(aIndex);
    return value != nullptr && value->tryGet(aResult);
}
bool pjson::tryGet(int aIndex, uint64_t& aResult) const noexcept {
    const pjson* value = find(aIndex);
    return value != nullptr && value->tryGet(aResult);
}
bool pjson::tryGet(int aIndex, double& aResult) const noexcept {
    const pjson* value = find(aIndex);
    return value != nullptr && value->tryGet(aResult);
}
bool pjson::tryGet(int aIndex, bool& aResult) const noexcept {
    const pjson* value = find(aIndex);
    return value != nullptr && value->tryGet(aResult);
}
bool pjson::tryGet(int aIndex, std::string& aResult) const {
    const pjson* value = find(aIndex);
    return value != nullptr && value->tryGet(aResult);
}
bool pjson::tryGet(int aIndex, StringView& aResult) const noexcept {
    const pjson* value = find(aIndex);
    return value != nullptr && value->tryGet(aResult);
}

//===----------------------------------------------------------------------===//

// Container queries and mutation
//===----------------------------------------------------------------------===//

bool pjson::hasKey(const std::string& aKey) const {
    if (_pImpl->_eType == pjson::jsonType::jsonObject) {
        auto it = _pImpl->_pValueMap->find(aKey);
        return (it != _pImpl->_pValueMap->end());
    }
    return false;
}
// Reports whether an object contains a non-null C-string key.
bool pjson::hasKey(const char* cStr) const {
    if (cStr != nullptr)
        return hasKey(std::string(cStr));
    return false;
}
// Reports whether an array index resolves under find()'s negative-index rules.
bool pjson::hasIndex(int aIndex) const noexcept {
    return find(aIndex) != nullptr;
}
// Returns the member/element count for containers and zero for scalars.
size_t pjson::size() const {
    if (_pImpl->_eType == pjson::jsonType::jsonArray) {
        return _pImpl->_pValueArray->size();
    }
    if (_pImpl->_eType == pjson::jsonType::jsonObject) {
        return _pImpl->_pValueMap->size();
    }
    return 0;
}
// Reports whether size() is zero, so every scalar is considered empty.
bool pjson::empty() const {
    return size() == 0;
}
// Clears container children without changing container type; scalars become null.
void pjson::clear() {
    // Arrays and maps become empty containers of the same type; anything else
    // resets to null.
    switch (_pImpl->_eType) {
        case pjson::jsonType::jsonArray: {
            pjsonImpl::_disposeChildren(*this);
            _pImpl->_pValueArray->clear();
            break;
        }
        case pjson::jsonType::jsonObject: {
            pjsonImpl::_disposeChildren(*this);
            _pImpl->_pValueMap->clear();
            break;
        }
        default:
            reset();
            break;
    }
}
// Returns copied object keys in the private container's unspecified native order.
std::vector<std::string> pjson::keys() const {
    std::vector<std::string> result;
    if (_pImpl->_eType == pjson::jsonType::jsonObject) {
        result.reserve(_pImpl->_pValueMap->size());
        for (const auto& kv : *_pImpl->_pValueMap) {
            result.push_back(kv.first);
        }
    }
    return result;
}
bool pjson::erase(const std::string& aKey) {
    if (_pImpl->_eType == pjson::jsonType::jsonObject) {
        auto it = _pImpl->_pValueMap->find(aKey);
        if (it != _pImpl->_pValueMap->end()) {
            pjsonImpl::_destroyNode(it->second);
            _pImpl->_pValueMap->erase(it);
            return true;
        }
    }
    return false;
}
// Removes an object member and destroys its owned subtree.
bool pjson::erase(const char* aKey) {
    if (aKey != nullptr)
        return erase(std::string(aKey));
    return false;
}
// Removes an array element and destroys its owned subtree, shifting later indices.
bool pjson::erase(size_t aIndex) {
    if (_pImpl->_eType == pjson::jsonType::jsonArray && aIndex < _pImpl->_pValueArray->size()) {
        pjsonImpl::_destroyNode((*_pImpl->_pValueArray)[aIndex]);
        _pImpl->_pValueArray->erase(_pImpl->_pValueArray->begin() +
                                    static_cast<std::ptrdiff_t>(aIndex));
        return true;
    }
    return false;
}
// Compares stored JSON numbers exactly across signed, unsigned, and double
// representations without rounding an integer through binary64. The result is
// -1/0/1, or 2 when a NaN makes the ordering unordered.
int pjsonImpl::_compareNumbers(const pjson& aLeft, const pjson& aRight) {
    const pjson::jsonType lt = aLeft._pImpl->_eType;
    const pjson::jsonType rt = aRight._pImpl->_eType;

    // ---- integer vs integer (any signedness) ----
    if (lt != pjson::jsonNumberDouble && rt != pjson::jsonNumberDouble) {
        const bool lu = lt == pjson::jsonNumberUInt;
        const bool ru = rt == pjson::jsonNumberUInt;
        if (!lu && !ru) {
            const int64_t l = aLeft._pImpl->_valueInt;
            const int64_t r = aRight._pImpl->_valueInt;
            return l < r ? -1 : (l > r ? 1 : 0);
        }
        if (lu && ru) {
            const uint64_t l = aLeft._pImpl->_valueUInt;
            const uint64_t r = aRight._pImpl->_valueUInt;
            return l < r ? -1 : (l > r ? 1 : 0);
        }
        // One signed, one unsigned. A negative signed value is always smaller.
        const int64_t s = lu ? aRight._pImpl->_valueInt : aLeft._pImpl->_valueInt;
        const uint64_t u = lu ? aLeft._pImpl->_valueUInt : aRight._pImpl->_valueUInt;
        int cmp;
        if (s < 0) {
            cmp = -1; // signed < unsigned
        } else {
            const uint64_t su = static_cast<uint64_t>(s);
            cmp = su < u ? -1 : (su > u ? 1 : 0);
        }
        // cmp expresses (signed operand) vs (unsigned operand); flip if the
        // unsigned operand was on the left.
        return lu ? -cmp : cmp;
    }

    // ---- double vs double ----
    if (lt == pjson::jsonNumberDouble && rt == pjson::jsonNumberDouble) {
        const double left = aLeft._pImpl->_valueDouble;
        const double right = aRight._pImpl->_valueDouble;
        if (std::isnan(left) || std::isnan(right))
            return 2;
        if (left < right)
            return -1;
        return left > right ? 1 : 0;
    }

    // ---- integer vs double ----
    const bool intOnLeft = lt != pjson::jsonNumberDouble;
    const pjson& intNode = intOnLeft ? aLeft : aRight;
    const double floating = intOnLeft ? aRight._pImpl->_valueDouble : aLeft._pImpl->_valueDouble;
    if (std::isnan(floating))
        return 2;

    // Compare the integer against the double exactly. Represent the integer's
    // value and compare via a double truncation plus fractional tiebreak.
    int intVsDouble = 0;
    if (intNode._pImpl->_eType == pjson::jsonNumberUInt) {
        const uint64_t integer = intNode._pImpl->_valueUInt;
        if (floating >= 18446744073709551616.0) { // 2^64
            intVsDouble = -1;
        } else if (floating < 0.0) {
            intVsDouble = 1;
        } else {
            const uint64_t truncated = static_cast<uint64_t>(floating);
            if (integer != truncated) {
                intVsDouble = integer < truncated ? -1 : 1;
            } else {
                const double integralPart = static_cast<double>(truncated);
                if (floating != integralPart)
                    intVsDouble = floating > integralPart ? -1 : 1;
            }
        }
    } else {
        const int64_t integer = intNode._pImpl->_valueInt;
        if (floating >= 9223372036854775808.0) { // exact 2^63
            intVsDouble = -1;
        } else if (floating < -9223372036854775808.0) {
            intVsDouble = 1;
        } else {
            const int64_t truncated = static_cast<int64_t>(floating);
            if (integer != truncated) {
                intVsDouble = integer < truncated ? -1 : 1;
            } else {
                const double integralPart = static_cast<double>(truncated);
                if (floating != integralPart)
                    intVsDouble = floating > integralPart ? -1 : 1;
            }
        }
    }
    return intOnLeft ? intVsDouble : -intVsDouble;
}
// Public exact numeric comparison. Delegates to the internal comparator, which
// returns 2 for a NaN-involved (unordered) comparison; that and any non-numeric
// operand are reported as "no ordering" so callers never see a rounded result.
bool pjson::tryCompareNumber(const pjson& aOther, int& aOrder) const noexcept {
    if (!isNumber() || !aOther.isNumber())
        return false;
    const int order = pjsonImpl::_compareNumbers(*this, aOther);
    if (order == 2) // unordered (NaN)
        return false;
    aOrder = order;
    return true;
}
// Deep structural equality. Numbers compare by value across int/double
// (1 == 1.0); arrays element-wise in order; objects by key/value. The walk is
// iterative (an explicit pair work-list) so it never overflows the call stack
// on deeply nested documents.
bool pjson::operator==(const pjson& aOther) const {
    struct Pair {
        const pjson* a;
        const pjson* b;
    };
    std::vector<Pair> work;
    Pair root = {this, &aOther};
    work.push_back(root);

    while (!work.empty()) {
        Pair cur = work.back();
        work.pop_back();
        const pjson& lhs = *cur.a;
        const pjson& rhs = *cur.b;

        // Numbers compare across int/uint/double as one family.
        bool lNum = lhs.isNumber();
        bool rNum = rhs.isNumber();
        if (lNum && rNum) {
            if (pjsonImpl::_compareNumbers(lhs, rhs) != 0) {
                return false;
            }
            continue;
        }

        if (lhs._pImpl->_eType != rhs._pImpl->_eType) {
            return false;
        }

        switch (lhs._pImpl->_eType) {
            case pjson::jsonType::jsonNull:
                break;
            case pjson::jsonType::jsonString:
                if (*lhs._pImpl->_pValueString != *rhs._pImpl->_pValueString)
                    return false;
                break;
            case pjson::jsonType::jsonBoolean:
                if (lhs._pImpl->_valueBool != rhs._pImpl->_valueBool)
                    return false;
                break;
            case pjson::jsonType::jsonNumberInt:
                if (lhs._pImpl->_valueInt != rhs._pImpl->_valueInt)
                    return false;
                break;
            case pjson::jsonType::jsonNumberUInt:
                if (lhs._pImpl->_valueUInt != rhs._pImpl->_valueUInt)
                    return false;
                break;
            case pjson::jsonType::jsonNumberDouble:
                if (lhs._pImpl->_valueDouble != rhs._pImpl->_valueDouble)
                    return false;
                break;
            case pjson::jsonType::jsonArray: {
                if (lhs._pImpl->_pValueArray->size() != rhs._pImpl->_pValueArray->size()) {
                    return false;
                }
                for (size_t i = 0; i < lhs._pImpl->_pValueArray->size(); ++i) {
                    Pair p = {(*lhs._pImpl->_pValueArray)[i], (*rhs._pImpl->_pValueArray)[i]};
                    work.push_back(p);
                }
                break;
            }
            case pjson::jsonType::jsonObject: {
                if (lhs._pImpl->_pValueMap->size() != rhs._pImpl->_pValueMap->size()) {
                    return false;
                }
                for (pjsonImpl::ObjectStorage::const_iterator it = lhs._pImpl->_pValueMap->begin();
                     it != lhs._pImpl->_pValueMap->end(); ++it) {
                    pjsonImpl::ObjectStorage::const_iterator matching =
                        rhs._pImpl->_pValueMap->find(it->first);
                    if (matching == rhs._pImpl->_pValueMap->end())
                        return false;
                    Pair p = {it->second, matching->second};
                    work.push_back(p);
                }
                break;
            }
        }
    }
    return true;
}
// Implements inequality as the exact complement of structural equality.
bool pjson::operator!=(const pjson& aOther) const {
    return !(*this == aOther);
}
