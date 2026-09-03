// SPDX-FileCopyrightText: 2026 ByteDance Ltd. and/or its affiliates
// SPDX-License-Identifier: Apache-2.0
//
// RFC 6902 JSON Patch and RFC 7396 JSON Merge Patch.

#include "pjson_internal.h"
#include "pjson_pointer_internal.h"

#include <algorithm>
#include <exception>
#include <new>
#include <utility>

using namespace ByteDance;
using namespace ByteDance::pjson_pointer_detail;

pjson::PatchError::PatchError()
        : ok(true)
        , code(Ok)
        , opIndex(0)
        , tokenIndex(0) {}

pjson::PatchOptions::PatchOptions()
        : maxOperations(10000)
        , maxClonedNodes(1000000)
        , maxClonedBytes(size_t(64) * 1024U * 1024U)
        , maxWork(1000000) {}

// RFC 6902 JSON Patch and RFC 7396 Merge Patch helpers
//
// Helpers accept ownership of values through pjsonImpl::OwnedNode and release only after
// attachment, so failed insertions cannot leak. Public entry points work on a
// full allocator-local clone and swap it into place only after every operation
// succeeds, giving both patch formats document-level atomicity.
//===----------------------------------------------------------------------===//

namespace {
    typedef pjson::PatchError PatchError;
    bool failPatch(PatchError& aError, PatchError::Code aCode, const char* aMessage);

    struct PatchBudget {
        size_t operations;
        size_t nodes;
        size_t bytes;
        size_t work;
        size_t operationLimit;
        size_t nodeLimit;
        size_t byteLimit;
        size_t workLimit;

        explicit PatchBudget(const pjson::PatchOptions& options)
                : operations(0)
                , nodes(0)
                , bytes(0)
                , work(0)
                , operationLimit(options.maxOperations == 0 ? size_t(10000) : options.maxOperations)
                , nodeLimit(options.maxClonedNodes == 0 ? size_t(1000000) : options.maxClonedNodes)
                , byteLimit(options.maxClonedBytes == 0 ? size_t(64) * 1024U * 1024U
                                                        : options.maxClonedBytes)
                , workLimit(options.maxWork == 0 ? size_t(1000000) : options.maxWork) {}
    };

    bool chargePatch(size_t& used, size_t limit, size_t amount, PatchError& error,
                     const char* message) {
        if (amount > limit - std::min(used, limit))
            return failPatch(error, PatchError::ResourceLimit, message);
        used += amount;
        return true;
    }

    bool measureClone(const pjson& value, PatchBudget& budget, PatchError& error) {
        std::vector<const pjson*> work;
        work.push_back(&value);
        while (!work.empty()) {
            if (!chargePatch(budget.work, budget.workLimit, 1, error,
                             "JSON patch work budget exceeded") ||
                !chargePatch(budget.nodes, budget.nodeLimit, 1, error,
                             "JSON patch cloned-node budget exceeded") ||
                !chargePatch(budget.bytes, budget.byteLimit, sizeof(pjson), error,
                             "JSON patch cloned-byte budget exceeded"))
                return false;
            const pjson* current = work.back();
            work.pop_back();
            if (current->isString()) {
                if (!chargePatch(budget.bytes, budget.byteLimit,
                                 pjsonImpl::_string(*current).size(), error,
                                 "JSON patch cloned-byte budget exceeded"))
                    return false;
            } else if (current->isArray()) {
                const pjsonImpl::ArrayStorage& array = pjsonImpl::_array(*current);
                const size_t remainingWork =
                    budget.workLimit - std::min(budget.work, budget.workLimit);
                if (array.size() > remainingWork)
                    return failPatch(error, PatchError::ResourceLimit,
                                     "JSON patch work budget exceeded");
                work.insert(work.end(), array.begin(), array.end());
            } else if (current->isObject()) {
                const pjsonImpl::ObjectStorage& object = pjsonImpl::_object(*current);
                const size_t remainingWork =
                    budget.workLimit - std::min(budget.work, budget.workLimit);
                if (object.size() > remainingWork)
                    return failPatch(error, PatchError::ResourceLimit,
                                     "JSON patch work budget exceeded");
                for (pjsonImpl::ObjectStorage::const_iterator it = object.begin();
                     it != object.end(); ++it) {
                    if (!chargePatch(budget.bytes, budget.byteLimit, it->first.size(), error,
                                     "JSON patch cloned-byte budget exceeded"))
                        return false;
                    work.push_back(it->second);
                }
            }
        }
        return true;
    }

    // Patch `test` needs bounded structural equality so an adversarial value
    // cannot hide unbounded traversal behind a single operation.
    bool patchEqual(const pjson& left, const pjson& right, PatchBudget& budget, PatchError& error,
                    bool& equal) {
        struct Pair {
            const pjson* left;
            const pjson* right;
        };
        std::vector<Pair> pending;
        Pair root = {&left, &right};
        pending.push_back(root);
        equal = false;
        while (!pending.empty()) {
            if (!chargePatch(budget.work, budget.workLimit, 1, error,
                             "JSON Patch work budget exceeded"))
                return false;
            const Pair current = pending.back();
            pending.pop_back();
            const pjson& lhs = *current.left;
            const pjson& rhs = *current.right;
            if (lhs.isNumber() && rhs.isNumber()) {
                if (pjsonImpl::_compareNumbers(lhs, rhs) != 0)
                    return true;
                continue;
            }
            if (lhs.getType() != rhs.getType())
                return true;
            if (lhs.isString()) {
                const std::string& l = pjsonImpl::_string(lhs);
                const std::string& r = pjsonImpl::_string(rhs);
                if (!chargePatch(budget.work, budget.workLimit, std::max(l.size(), r.size()), error,
                                 "JSON Patch work budget exceeded"))
                    return false;
                if (l != r)
                    return true;
            } else if (lhs.isBool()) {
                if (pjsonImpl::_boolean(lhs) != pjsonImpl::_boolean(rhs))
                    return true;
            } else if (lhs.isArray()) {
                const pjsonImpl::ArrayStorage& l = pjsonImpl::_array(lhs);
                const pjsonImpl::ArrayStorage& r = pjsonImpl::_array(rhs);
                if (l.size() != r.size())
                    return true;
                for (size_t i = 0; i < l.size(); ++i) {
                    Pair child = {l[i], r[i]};
                    pending.push_back(child);
                }
            } else if (lhs.isObject()) {
                const pjsonImpl::ObjectStorage& l = pjsonImpl::_object(lhs);
                const pjsonImpl::ObjectStorage& r = pjsonImpl::_object(rhs);
                if (l.size() != r.size())
                    return true;
                pjsonImpl::ObjectStorage::const_iterator li = l.begin();
                pjsonImpl::ObjectStorage::const_iterator ri = r.begin();
                for (; li != l.end(); ++li, ++ri) {
                    if (!chargePatch(budget.work, budget.workLimit,
                                     std::max(li->first.size(), ri->first.size()) + size_t(1),
                                     error, "JSON Patch work budget exceeded"))
                        return false;
                    if (li->first != ri->first)
                        return true;
                    Pair child = {li->second, ri->second};
                    pending.push_back(child);
                }
            }
        }
        equal = true;
        return true;
    }

    // Restores a reusable PatchError before processing a new patch document.
    void resetPatchError(PatchError& aError) {
        aError.ok = true;
        aError.code = PatchError::Ok;
        aError.opIndex = 0;
        aError.op.clear();
        aError.path.clear();
        aError.from.clear();
        aError.tokenIndex = 0;
        aError.token.clear();
        aError.message.clear();
    }

    // Records a patch failure while preserving operation metadata set by the caller.
    bool failPatch(PatchError& aError, PatchError::Code aCode, const char* aMessage) {
        aError.ok = false;
        aError.code = aCode;
        aError.message = aMessage;
        return false;
    }

    // Records a patch failure associated with one decoded pointer token.
    bool failPatchAtToken(PatchError& aError, PatchError::Code aCode, size_t aTokenIndex,
                          const std::string& aToken, const char* aMessage) {
        aError.tokenIndex = aTokenIndex;
        aError.token = aToken;
        return failPatch(aError, aCode, aMessage);
    }

    // Best-effort noexcept diagnostic used while translating allocation or
    // unexpected exceptions out of the public patch API.
    void failPatchException(PatchError& aError, PatchError::Code aCode,
                            const char* aMessage) noexcept {
        aError.ok = false;
        aError.code = aCode;
        try {
            aError.message = aMessage;
        } catch (...) {
            aError.message.clear();
        }
    }

    // Maps traversal categories into the smaller PatchError vocabulary.
    PatchError::Code pointerCodeForPatch(pjson::PointerError::Code aCode) {
        switch (aCode) {
            case pjson::PointerError::InvalidArrayIndex:
            case pjson::PointerError::AppendTokenNotAllowed:
                return PatchError::InvalidArrayIndex;
            case pjson::PointerError::ArrayIndexOutOfRange:
                return PatchError::ArrayIndexOutOfRange;
            case pjson::PointerError::MissingTarget:
            case pjson::PointerError::ExpectedContainer:
                return PatchError::TargetMissing;
            case pjson::PointerError::AllocationFailure:
                return PatchError::AllocationFailure;
            case pjson::PointerError::InternalError:
                return PatchError::InternalError;
            default:
                return PatchError::TargetMissing;
        }
    }

    // Copies token context from a pointer failure into the active operation error.
    bool failPatchFromPointer(PatchError& aError, const pjson::PointerError& aPointerError) {
        aError.tokenIndex = aPointerError.tokenIndex;
        aError.token = aPointerError.token;
        return failPatch(aError, pointerCodeForPatch(aPointerError.code),
                         aPointerError.message.c_str());
    }

    // Decodes a Patch pointer and classifies syntax failures as path or from errors.
    bool decodePatchPointer(const std::string& aPointer, bool bFrom,
                            std::vector<std::string>& aTokens, PatchBudget& aBudget,
                            PatchError& aError) {
        if (!chargePatch(aBudget.work, aBudget.workLimit, aPointer.size() + size_t(1), aError,
                         "JSON patch work budget exceeded"))
            return false;
        pjson::PointerError pointerError;
        if (decodePointer(aPointer, aTokens, pointerError))
            return true;
        aError.tokenIndex = pointerError.tokenIndex;
        aError.token = pointerError.token;
        return failPatch(aError, bFrom ? PatchError::InvalidFrom : PatchError::InvalidPath,
                         pointerError.message.c_str());
    }

    // Resolves a mutable prefix and translates PointerError into PatchError.
    pjson* resolvePatchTokens(pjson& aRoot, const std::vector<std::string>& aTokens, size_t aCount,
                              const std::string& aPointer, PatchBudget& aBudget,
                              PatchError& aError) {
        if (!chargePatch(aBudget.work, aBudget.workLimit, aCount + size_t(1), aError,
                         "JSON patch work budget exceeded"))
            return nullptr;
        pjson::PointerError pointerError;
        const pjson* result = resolvePointerTokens(aRoot, aTokens, aCount, aPointer, pointerError);
        if (result == nullptr) {
            failPatchFromPointer(aError, pointerError);
            return nullptr;
        }
        return const_cast<pjson*>(result);
    }

    // Validates a destination/source array index. '-' denotes exactly size() and
    // is accepted only for add, whose insertion range includes the end position.
    bool patchArrayIndex(const pjson& aParent, const std::string& aToken, bool bAllowAppend,
                         size_t aTokenIndex, size_t& aIndex, bool& bAppend, PatchError& aError) {
        bAppend = false;
        if (aToken == "-") {
            if (bAllowAppend) {
                bAppend = true;
                aIndex = aParent.size();
                return true;
            }
            return failPatchAtToken(aError, PatchError::InvalidArrayIndex, aTokenIndex, aToken,
                                    "the '-' token is valid only for add destinations");
        }

        const PointerIndexResult result = parsePointerIndex(aToken, aIndex);
        if (result == PointerIndexInvalid)
            return failPatchAtToken(aError, PatchError::InvalidArrayIndex, aTokenIndex, aToken,
                                    "array index is not canonical decimal");
        const size_t size = aParent.size();
        if (result == PointerIndexOverflow || (bAllowAppend ? aIndex > size : aIndex >= size))
            return failPatchAtToken(aError, PatchError::ArrayIndexOutOfRange, aTokenIndex, aToken,
                                    "array index is out of range");
        return true;
    }

    // Consumes an allocator-compatible value and implements Patch add. Existing
    // object members are replaced; array insertion shifts following elements.
    bool addOwnedAtPointer(pjson& aRoot, const std::vector<std::string>& aTokens,
                           const std::string& aPointer, pjsonImpl::OwnedNode aValue,
                           PatchBudget& aBudget, PatchError& aError) {
        if (aTokens.empty()) {
            pjsonImpl::_swapStorage(aRoot, *aValue);
            return true;
        }

        const size_t finalIndex = aTokens.size() - 1;
        pjson* parent = resolvePatchTokens(aRoot, aTokens, finalIndex, aPointer, aBudget, aError);
        if (parent == nullptr)
            return false;
        const std::string& token = aTokens.back();

        if (parent->isObject()) {
            pjsonImpl::ObjectStorage* object = &pjsonImpl::_object(*parent);
            pjsonImpl::ObjectStorage::iterator existing = object->find(token);
            if (existing != object->end()) {
                pjsonImpl::_swapStorage(*existing->second, *aValue);
                return true;
            }
            if (!chargePatch(aBudget.bytes, aBudget.byteLimit, token.size(), aError,
                             "JSON Patch cloned-byte budget exceeded"))
                return false;
            const std::pair<pjsonImpl::ObjectStorage::iterator, bool> inserted =
                object->insert(std::make_pair(token, static_cast<pjson*>(nullptr)));
            if (!inserted.second)
                return failPatchAtToken(aError, PatchError::InternalError, finalIndex, token,
                                        "failed to insert object member");
            inserted.first->second = aValue.release();
            return true;
        }

        if (parent->isArray()) {
            size_t index = 0;
            bool append = false;
            if (!patchArrayIndex(*parent, token, true, finalIndex, index, append, aError))
                return false;
            pjsonImpl::ArrayStorage* array = &pjsonImpl::_array(*parent);
            if (!chargePatch(aBudget.work, aBudget.workLimit, array->size() - index, aError,
                             "JSON Patch work budget exceeded"))
                return false;
            const pjsonImpl::ArrayStorage::iterator inserted =
                array->insert(array->begin() + static_cast<std::ptrdiff_t>(index), nullptr);
            *inserted = aValue.release();
            return true;
        }

        return failPatchAtToken(aError, PatchError::TargetMissing, finalIndex, token,
                                "add destination parent is not a container");
    }

    // Consumes a replacement only after proving the complete target exists.
    bool replaceAtPointer(pjson& aRoot, const std::vector<std::string>& aTokens,
                          const std::string& aPointer, pjsonImpl::OwnedNode aValue,
                          PatchBudget& aBudget, PatchError& aError) {
        if (aTokens.empty()) {
            pjsonImpl::_swapStorage(aRoot, *aValue);
            return true;
        }

        const size_t finalIndex = aTokens.size() - 1;
        pjson* parent = resolvePatchTokens(aRoot, aTokens, finalIndex, aPointer, aBudget, aError);
        if (parent == nullptr)
            return false;
        const std::string& token = aTokens.back();

        if (parent->isObject()) {
            pjsonImpl::ObjectStorage* object = &pjsonImpl::_object(*parent);
            pjsonImpl::ObjectStorage::iterator existing = object->find(token);
            if (existing == object->end())
                return failPatchAtToken(aError, PatchError::TargetMissing, finalIndex, token,
                                        "replace target does not exist");
            pjsonImpl::_swapStorage(*existing->second, *aValue);
            return true;
        }

        if (parent->isArray()) {
            size_t index = 0;
            bool append = false;
            if (!patchArrayIndex(*parent, token, false, finalIndex, index, append, aError))
                return false;
            pjsonImpl::_swapStorage(*pjsonImpl::_array(*parent)[index], *aValue);
            return true;
        }

        return failPatchAtToken(aError, PatchError::TargetMissing, finalIndex, token,
                                "replace target parent is not a container");
    }

    // Detaches a target without destroying it. Removing the document root is
    // represented by replacing the still-addressable root value with JSON null.
    bool detachAtPointer(pjson& aRoot, const std::vector<std::string>& aTokens,
                         const std::string& aPointer, pjsonImpl::OwnedNode& aValue,
                         PatchBudget& aBudget, PatchError& aError) {
        if (aTokens.empty()) {
            if (!chargePatch(aBudget.nodes, aBudget.nodeLimit, 1, aError,
                             "JSON Patch cloned-node budget exceeded") ||
                !chargePatch(aBudget.bytes, aBudget.byteLimit, sizeof(pjson), aError,
                             "JSON Patch cloned-byte budget exceeded"))
                return false;
            pjsonImpl::OwnedNode replacement = pjsonImpl::_makeNode(aRoot.getAllocator());
            pjsonImpl::_swapStorage(aRoot, *replacement);
            aValue = std::move(replacement);
            return true;
        }

        const size_t finalIndex = aTokens.size() - 1;
        pjson* parent = resolvePatchTokens(aRoot, aTokens, finalIndex, aPointer, aBudget, aError);
        if (parent == nullptr)
            return false;
        const std::string& token = aTokens.back();

        if (parent->isObject()) {
            pjsonImpl::ObjectStorage* object = &pjsonImpl::_object(*parent);
            pjsonImpl::ObjectStorage::iterator existing = object->find(token);
            if (existing == object->end())
                return failPatchAtToken(aError, PatchError::TargetMissing, finalIndex, token,
                                        "remove source does not exist");
            aValue.reset(existing->second);
            object->erase(existing);
            return true;
        }

        if (parent->isArray()) {
            size_t index = 0;
            bool append = false;
            if (!patchArrayIndex(*parent, token, false, finalIndex, index, append, aError))
                return false;
            pjsonImpl::ArrayStorage* array = &pjsonImpl::_array(*parent);
            if (!chargePatch(aBudget.work, aBudget.workLimit, array->size() - index - size_t(1),
                             aError, "JSON Patch work budget exceeded"))
                return false;
            aValue.reset((*array)[index]);
            array->erase(array->begin() + static_cast<std::ptrdiff_t>(index));
            return true;
        }

        return failPatchAtToken(aError, PatchError::TargetMissing, finalIndex, token,
                                "remove source parent is not a container");
    }

    // Compares decoded paths so alternate escape spellings cannot affect identity.
    bool samePointerTokens(const std::vector<std::string>& aLeft,
                           const std::vector<std::string>& aRight) {
        return aLeft == aRight;
    }

    // Detects a move into the source's own descendant, which would invalidate
    // the destination during detachment and is forbidden by JSON Patch.
    bool isProperPointerAncestor(const std::vector<std::string>& aAncestor,
                                 const std::vector<std::string>& aDescendant) {
        return aAncestor.size() < aDescendant.size() &&
               std::equal(aAncestor.begin(), aAncestor.end(), aDescendant.begin());
    }

    // Replaces or adopts an allocator-compatible object child without exposing
    // a null map entry if insertion fails.
    bool insertObjectChild(pjson& aObject, const std::string& aKey, pjsonImpl::OwnedNode aChild) {
        pjsonImpl::ObjectStorage* object = &pjsonImpl::_object(aObject);
        pjsonImpl::ObjectStorage::iterator existing = object->find(aKey);
        if (existing != object->end()) {
            pjsonImpl::_swapStorage(*existing->second, *aChild);
            return true;
        }
        const std::pair<pjsonImpl::ObjectStorage::iterator, bool> inserted =
            object->insert(std::make_pair(aKey, static_cast<pjson*>(nullptr)));
        if (!inserted.second)
            return false;
        inserted.first->second = aChild.release();
        return true;
    }

    // Applies Merge Patch iteratively to a private working document. Object
    // patches recurse, null members delete, and every other value replaces via
    // an allocator-local clone. Atomic publication is handled by the caller.
    bool applyMergePatchTo(pjson& aTarget, const pjson& aPatch, PatchBudget& aBudget,
                           PatchError& aError) {
        struct MergeItem {
            // target and patch describe one pending object/object merge.
            pjson* target;
            const pjson* patch;
        };

        if (!aPatch.isObject()) {
            if (!chargePatch(aBudget.operations, aBudget.operationLimit, 1, aError,
                             "JSON Merge Patch operation budget exceeded"))
                return false;
            if (!measureClone(aPatch, aBudget, aError))
                return false;
            pjson replacement(aPatch, aTarget.getAllocator());
            pjsonImpl::_swapStorage(aTarget, replacement);
            return true;
        }

        std::vector<MergeItem> work;
        MergeItem root = {&aTarget, &aPatch};
        work.push_back(root);
        while (!work.empty()) {
            const MergeItem item = work.back();
            work.pop_back();
            if (!item.target->isObject())
                item.target->resetTo(pjson::jsonObject);

            const pjsonImpl::ObjectStorage* patchObject = &pjsonImpl::_object(*item.patch);
            for (pjsonImpl::ObjectStorage::const_iterator it = patchObject->begin();
                 it != patchObject->end(); ++it) {
                if (!chargePatch(aBudget.operations, aBudget.operationLimit, 1, aError,
                                 "JSON Merge Patch operation budget exceeded") ||
                    !chargePatch(aBudget.work, aBudget.workLimit, 1, aError,
                                 "JSON Merge Patch work budget exceeded"))
                    return false;
                const std::string& key = it->first;
                const pjson& patchValue = *it->second;
                if (!chargePatch(aBudget.bytes, aBudget.byteLimit, key.size(), aError,
                                 "JSON Merge Patch cloned-byte budget exceeded"))
                    return false;
                if (patchValue.isNull()) {
                    item.target->erase(key);
                    continue;
                }

                pjson* targetValue = item.target->find(key);
                if (patchValue.isObject()) {
                    if (targetValue == nullptr) {
                        if (!chargePatch(aBudget.nodes, aBudget.nodeLimit, 1, aError,
                                         "JSON Merge Patch cloned-node budget exceeded") ||
                            !chargePatch(aBudget.bytes, aBudget.byteLimit, sizeof(pjson), aError,
                                         "JSON Merge Patch cloned-byte budget exceeded"))
                            return false;
                        pjsonImpl::OwnedNode child =
                            pjsonImpl::_makeNode(item.target->getAllocator());
                        child->resetTo(pjson::jsonObject);
                        targetValue = child.get();
                        if (!insertObjectChild(*item.target, key, std::move(child)))
                            return false;
                    } else if (!targetValue->isObject()) {
                        targetValue->resetTo(pjson::jsonObject);
                    }
                    MergeItem childItem = {targetValue, &patchValue};
                    work.push_back(childItem);
                    continue;
                }

                if (!measureClone(patchValue, aBudget, aError))
                    return false;
                pjsonImpl::OwnedNode replacement =
                    pjsonImpl::_cloneNode(patchValue, item.target->getAllocator());
                if (!insertObjectChild(*item.target, key, std::move(replacement)))
                    return false;
            }
        }
        return true;
    }
} // namespace

// Applies JSON Patch while intentionally discarding detailed diagnostics.
bool pjson::applyPatch(const pjson& aPatch, const PatchOptions& aOpts) noexcept {
    PatchError error;
    return applyPatch(aPatch, error, aOpts);
}
// Applies an RFC 6902 operation sequence atomically. Validation and mutation
// happen on scratch; only a completely successful sequence is published by swap.
bool pjson::applyPatch(const pjson& aPatch, PatchError& aError,
                       const PatchOptions& aOpts) noexcept {
    resetPatchError(aError);
    try {
        if (!aPatch.isArray())
            return failPatch(aError, PatchError::InvalidPatchDocument,
                             "JSON Patch document must be an array");

        PatchBudget budget(aOpts);
        const pjsonImpl::ArrayStorage& operations = pjsonImpl::_array(aPatch);
        if (!chargePatch(budget.operations, budget.operationLimit, operations.size(), aError,
                         "JSON Patch operation budget exceeded") ||
            !measureClone(*this, budget, aError))
            return false;

        // The scratch copy is both the rollback boundary and the allocator domain
        // into which every add/copy/replace value must be cloned.
        pjson scratch(*this, *_allocator);
        for (size_t operationIndex = 0; operationIndex < operations.size(); ++operationIndex) {
            const pjson& operation = *operations[operationIndex];
            aError.opIndex = operationIndex;
            aError.op.clear();
            aError.path.clear();
            aError.from.clear();
            aError.tokenIndex = 0;
            aError.token.clear();
            aError.message.clear();

            if (!operation.isObject())
                return failPatch(aError, PatchError::OperationNotObject,
                                 "JSON Patch operation must be an object");

            const pjson* opNode = operation.find("op");
            if (opNode == nullptr || !opNode->isString())
                return failPatch(aError, PatchError::MissingOp,
                                 "JSON Patch operation requires string member 'op'");
            aError.op = pjsonImpl::_string(*opNode);

            const bool knownOperation = aError.op == "add" || aError.op == "remove" ||
                                        aError.op == "replace" || aError.op == "move" ||
                                        aError.op == "copy" || aError.op == "test";
            if (!knownOperation)
                return failPatch(aError, PatchError::InvalidOp,
                                 "JSON Patch operation name is not supported");

            const pjson* pathNode = operation.find("path");
            if (pathNode == nullptr || !pathNode->isString())
                return failPatch(aError, PatchError::MissingPath,
                                 "JSON Patch operation requires string member 'path'");
            aError.path = pjsonImpl::_string(*pathNode);
            std::vector<std::string> pathTokens;
            if (!decodePatchPointer(aError.path, false, pathTokens, budget, aError))
                return false;

            // Validate and decode this operation's metadata before mutating the
            // private scratch tree. Later operations are processed only after
            // earlier ones succeed; the public target is still untouched.
            const bool needsFrom = aError.op == "move" || aError.op == "copy";
            std::vector<std::string> fromTokens;
            if (needsFrom) {
                const pjson* fromNode = operation.find("from");
                if (fromNode == nullptr || !fromNode->isString())
                    return failPatch(aError, PatchError::MissingFrom,
                                     "move and copy require string member 'from'");
                aError.from = pjsonImpl::_string(*fromNode);
                if (!decodePatchPointer(aError.from, true, fromTokens, budget, aError))
                    return false;
            }

            const bool needsValue =
                aError.op == "add" || aError.op == "replace" || aError.op == "test";
            const pjson* valueNode = operation.find("value");
            if (needsValue && valueNode == nullptr)
                return failPatch(aError, PatchError::MissingValue,
                                 "add, replace, and test require member 'value'");

            if (aError.op == "add") {
                if (!measureClone(*valueNode, budget, aError))
                    return false;
                pjsonImpl::OwnedNode value =
                    pjsonImpl::_cloneNode(*valueNode, scratch.getAllocator());
                if (!addOwnedAtPointer(scratch, pathTokens, aError.path, std::move(value), budget,
                                       aError))
                    return false;
                continue;
            }

            if (aError.op == "remove") {
                pjsonImpl::OwnedNode removed;
                if (!detachAtPointer(scratch, pathTokens, aError.path, removed, budget, aError))
                    return false;
                continue;
            }

            if (aError.op == "replace") {
                if (!measureClone(*valueNode, budget, aError))
                    return false;
                pjsonImpl::OwnedNode value =
                    pjsonImpl::_cloneNode(*valueNode, scratch.getAllocator());
                if (!replaceAtPointer(scratch, pathTokens, aError.path, std::move(value), budget,
                                      aError))
                    return false;
                continue;
            }

            if (aError.op == "test") {
                const pjson* target = resolvePatchTokens(scratch, pathTokens, pathTokens.size(),
                                                         aError.path, budget, aError);
                if (target == nullptr)
                    return false;
                bool equal = false;
                if (!patchEqual(*target, *valueNode, budget, aError, equal))
                    return false;
                if (!equal)
                    return failPatch(aError, PatchError::TestFailed,
                                     "JSON Patch test value does not match target");
                continue;
            }

            const pjson* source = resolvePatchTokens(scratch, fromTokens, fromTokens.size(),
                                                     aError.from, budget, aError);
            if (source == nullptr)
                return false;

            if (aError.op == "copy") {
                if (!measureClone(*source, budget, aError))
                    return false;
                pjsonImpl::OwnedNode value = pjsonImpl::_cloneNode(*source, scratch.getAllocator());
                if (!addOwnedAtPointer(scratch, pathTokens, aError.path, std::move(value), budget,
                                       aError))
                    return false;
                continue;
            }

            if (samePointerTokens(fromTokens, pathTokens))
                continue;
            if (fromTokens.empty()) {
                // Moving the root can only succeed when the destination is
                // also root (handled above); every other path is a descendant.
                return failPatch(aError, PatchError::MoveRootNotAllowed,
                                 "cannot move the document root below itself");
            }
            if (isProperPointerAncestor(fromTokens, pathTokens))
                return failPatch(aError, PatchError::MoveIntoDescendant,
                                 "cannot move a value into one of its descendants");

            pjsonImpl::OwnedNode moved;
            if (!detachAtPointer(scratch, fromTokens, aError.from, moved, budget, aError))
                return false;
            if (!addOwnedAtPointer(scratch, pathTokens, aError.path, std::move(moved), budget,
                                   aError))
                return false;
        }

        // This is the sole publication point; all earlier exits leave *this intact.
        pjsonImpl::_swapStorage(*this, scratch);
        resetPatchError(aError);
        return true;
    } catch (const std::bad_alloc&) {
        failPatchException(aError, PatchError::AllocationFailure, "JSON Patch ran out of memory");
        return false;
    } catch (const std::exception&) {
        failPatchException(aError, PatchError::InternalError,
                           "JSON Patch failed with an internal exception");
        return false;
    } catch (...) {
        failPatchException(aError, PatchError::InternalError,
                           "JSON Patch failed with an unknown exception");
        return false;
    }
}
// Applies Merge Patch while intentionally discarding detailed diagnostics.
bool pjson::applyMergePatch(const pjson& aPatch, const PatchOptions& aOpts) noexcept {
    PatchError error;
    return applyMergePatch(aPatch, error, aOpts);
}
// Applies RFC 7396 atomically by mutating a private deep copy and publishing it
// only after the iterative merge has completed.
bool pjson::applyMergePatch(const pjson& aPatch, PatchError& aError,
                            const PatchOptions& aOpts) noexcept {
    resetPatchError(aError);
    try {
        PatchBudget budget(aOpts);
        if (!measureClone(*this, budget, aError))
            return false;
        pjson scratch(*this, *_allocator);
        if (!applyMergePatchTo(scratch, aPatch, budget, aError)) {
            if (!aError.ok)
                return false;
            return failPatch(aError, PatchError::InternalError,
                             "JSON Merge Patch could not update an object member");
        }
        pjsonImpl::_swapStorage(*this, scratch);
        resetPatchError(aError);
        return true;
    } catch (const std::bad_alloc&) {
        failPatchException(aError, PatchError::AllocationFailure,
                           "JSON Merge Patch ran out of memory");
        return false;
    } catch (const std::exception&) {
        failPatchException(aError, PatchError::InternalError,
                           "JSON Merge Patch failed with an internal exception");
        return false;
    } catch (...) {
        failPatchException(aError, PatchError::InternalError,
                           "JSON Merge Patch failed with an unknown exception");
        return false;
    }
}
/*static*/
