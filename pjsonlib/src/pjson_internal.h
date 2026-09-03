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
// pjson_internal.h — private, library-only header.
//
// This header is NOT installed and is not part of the public API. It defines
// the pjsonImpl friend struct and shared internal aliases so the library
// implementation can span the DOM, serialization, Pointer, and Patch
// translation units while keeping the public pjson.h declaration-focused.
// Parser implementation details live in pjson_parser_internal.h and depend on
// this core header; this header never depends on the parser.
//===----------------------------------------------------------------------===//
#ifndef PRAVEENJSON_INTERNAL_H
#define PRAVEENJSON_INTERNAL_H

#include "pjson.h"

#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

//===----------------------------------------------------------------------===//
// pjsonImpl — private DOM, ownership, and serialization helpers.
//
// Keeping implementation-only DOM operations in one friend struct leaves
// pjson.h declaration-focused while allowing these helpers to maintain DOM
// invariants. pJsonSchemaValidator does not include this header.
//===----------------------------------------------------------------------===//
struct ByteDance::pjsonImpl {
    // Public APIs deliberately hide the owning container representation.
    typedef std::vector<pjson*> ArrayStorage;
    typedef std::map<std::string, pjson*> ObjectStorage;

    // One suspended container in the iterative serializer. Exactly one of
    // array/object is active according to isObject; the associated cursor
    // always denotes the next child to emit.
    struct SerializeFrame {
        bool isObject;
        size_t depth;
        bool first;
        const ArrayStorage* array;
        size_t arrayIndex;
        const ObjectStorage* object;
        ObjectStorage::const_iterator objectIt;
        ObjectStorage::const_reverse_iterator objectReverseIt;
    };

    static int _utf8Len(const char* src, size_t pos, size_t end);
    static std::string _formatDouble(double aValue);
    template <typename Sink>
    static bool _writeEscapedTo(Sink& aOut, const std::string& aIn, bool bEscapeNonAscii);
    template <typename Sink>
    static bool _openOrEmit(Sink& aOut, const pjson* aValue, size_t aDepth,
                            const pjson::SerializeOptions& aOpts,
                            std::vector<SerializeFrame>& aFrames);
    template <typename Sink>
    static bool _writeValueTo(Sink& aOut, const pjson& aValue,
                              const pjson::SerializeOptions& aOpts);
    static void _appendValue(std::string& aOut, const pjson& aValue,
                             const pjson::SerializeOptions& aOpts);
    static bool _writeValue(std::ostream& aOut, const pjson& aValue,
                            const pjson::SerializeOptions& aOpts);

    // Internal typed/storage access keeps representation and permissive
    // conversion helpers out of the public API. Callers first establish type.
    static ArrayStorage& _array(pjson& aValue) { return *aValue._uValue._pValueArray; }
    static const ArrayStorage& _array(const pjson& aValue) { return *aValue._uValue._pValueArray; }
    static ObjectStorage& _object(pjson& aValue) { return *aValue._uValue._pValueMap; }
    static const ObjectStorage& _object(const pjson& aValue) { return *aValue._uValue._pValueMap; }
    static int64_t _integer(const pjson& aValue) { return aValue._uValue._valueInt; }
    static uint64_t _unsigned(const pjson& aValue) { return aValue._uValue._valueUInt; }
    static double _floating(const pjson& aValue) { return aValue._uValue._valueDouble; }
    static double _numberAsDouble(const pjson& aValue) {
        if (aValue._eType == pjson::jsonNumberInt)
            return static_cast<double>(aValue._uValue._valueInt);
        if (aValue._eType == pjson::jsonNumberUInt)
            return static_cast<double>(aValue._uValue._valueUInt);
        return aValue._uValue._valueDouble;
    }
    static bool _boolean(const pjson& aValue) { return aValue._uValue._valueBool; }
    static const std::string& _string(const pjson& aValue) { return *aValue._uValue._pValueString; }
    // Returns -1, 0, or 1, and 2 when either floating operand is NaN.
    static int _compareNumbers(const pjson& aLeft, const pjson& aRight);

    // Iteratively frees every descendant pjson of node's array/map, leaving the
    // node's own top-level container allocated but empty (a no-op for scalars).
    // Using an explicit work-list instead of the recursive destructor keeps
    // teardown safe on arbitrarily deep documents. Marked noexcept: it is
    // reached from ~pjson, so an allocation failure here terminates rather than
    // escaping a destructor.
    static void _disposeChildren(pjson& node) noexcept;
    static pjson::Allocator& _defaultAllocator() noexcept;
    static pjson* _allocateNode(pjson::Allocator& aAlloc);
    static void _destroyNode(pjson* aValue) noexcept;

    // Internal origin-aware owning pointer. Replaces the former public
    // pjsonImpl::OwnedNode/ValueDeleter: parser and mutation helpers still get
    // RAII cleanup during construction, but no smart pointer leaks into the
    // public API. Destruction routes through _destroyNode so allocator-backed
    // and ordinary `new` roots are both freed correctly.
    struct NodeDeleter {
        void operator()(pjson* aValue) const noexcept { pjsonImpl::_destroyNode(aValue); }
    };
    typedef std::unique_ptr<pjson, NodeDeleter> OwnedNode;

    static OwnedNode _makeNode(pjson::Allocator& aAlloc);
    static OwnedNode _cloneNode(const pjson& aValue, pjson::Allocator& aAlloc);

    // Instance behavior that must touch pjson's private storage lives here rather
    // than as private methods on pjson, so the public header carries no instance
    // helper declarations. pjsonImpl is a friend, so these reach _eType/_uValue
    // directly. Keeping them static and .cpp-local means a future data-member
    // change is contained to this file.
    //
    // Iteratively deep-copies aFrom's contents into aDst using aDst's allocator.
    static void _copyContentsInto(pjson& aDst, const pjson& aFrom);
    // O(1) storage exchange for two same-allocator nodes the caller has already
    // proven are not aliased (e.g. a target and a freshly cloned/detached value).
    // Bypasses the public swap()'s ancestor/descendant guard.
    static void _swapStorage(pjson& aLeft, pjson& aRight) noexcept;
    // Returns whether aNode is aRoot or lies within aRoot's subtree.
    static bool _containsNode(const pjson& aRoot, const pjson* aNode) noexcept;
};

// File-scope aliases keep internal type names concise without exposing the
// owning containers in the public header. They are visible in every library
// translation unit that includes this header.
typedef ByteDance::pjson::jsonType jsonType;
typedef ByteDance::pjsonImpl::ArrayStorage PJSONARRAY;
typedef ByteDance::pjsonImpl::ObjectStorage PJSONMAP;
#endif /* !PRAVEENJSON_INTERNAL_H */
