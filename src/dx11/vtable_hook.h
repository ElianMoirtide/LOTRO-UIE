#pragma once

#include "common/logger.h"

#include <Windows.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>

namespace lotro_uie {

inline std::mutex& GetVtablePatchMutex() {
    static std::mutex m;
    return m;
}

template <typename FnPtr>
struct VtableDispatch {
    struct Entry {
        std::atomic<void**> vtable{nullptr};
        std::atomic<FnPtr> orig{nullptr};
    };

    std::atomic<void**> primaryVtable{nullptr};
    std::atomic<FnPtr> primaryOrig{nullptr};
    std::array<Entry, 64> extra{};
    std::atomic<size_t> extraCount{0};

    FnPtr Resolve(void* comObject) const noexcept {
        if (!comObject) {
            return primaryOrig.load(std::memory_order_acquire);
        }
        void** vtbl = *reinterpret_cast<void***>(comObject);
        if (vtbl == primaryVtable.load(std::memory_order_acquire)) {
            return primaryOrig.load(std::memory_order_acquire);
        }
        const size_t count = extraCount.load(std::memory_order_acquire);
        for (size_t i = 0; i < count; ++i) {
            if (extra[i].vtable.load(std::memory_order_acquire) == vtbl) {
                return extra[i].orig.load(std::memory_order_acquire);
            }
        }
        return primaryOrig.load(std::memory_order_acquire);
    }
};

inline bool IsSameCodeModule(const void* a, const void* b) noexcept {
    if (!a || !b) return false;
    HMODULE modA = nullptr;
    HMODULE modB = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(a), &modA) || !modA) {
        return false;
    }
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(b), &modB) || !modB) {
        return false;
    }
    return modA == modB;
}

template <typename FnPtr>
bool WriteVtableSlot(void** vtable, size_t slotIndex, void* newAddr) {
    if (!vtable || !newAddr) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    bool needProtect = true;
    DWORD requestedProtect = PAGE_READWRITE;
    if (VirtualQuery(&vtable[slotIndex], &mbi, sizeof(mbi)) == sizeof(mbi)) {
        constexpr DWORD kWritableMask =
            PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        constexpr DWORD kExecMask =
            PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if ((mbi.Protect & kWritableMask) != 0) {
            needProtect = false;
        } else if ((mbi.Protect & kExecMask) != 0) {
            requestedProtect = PAGE_EXECUTE_READWRITE;
        }
    }

    DWORD oldProtect = 0;
    if (needProtect) {
        if (!VirtualProtect(&vtable[slotIndex], sizeof(void*), requestedProtect, &oldProtect) &&
            !VirtualProtect(&vtable[slotIndex], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            UIE_LOG_ERROR("VirtualProtect failed on vtable=%p slot=%zu (err=%lu)",
                          static_cast<void*>(vtable), slotIndex, GetLastError());
            return false;
        }
    }

    vtable[slotIndex] = newAddr;

    if (needProtect) {
        DWORD unused = 0;
        VirtualProtect(&vtable[slotIndex], sizeof(void*), oldProtect, &unused);
    }
    FlushInstructionCache(GetCurrentProcess(), &vtable[slotIndex], sizeof(void*));
    return true;
}

template <typename FnPtr>
bool PatchVtableSlot(
    void* comObject,
    size_t slotIndex,
    FnPtr hookFn,
    VtableDispatch<FnPtr>& dispatch,
    bool allowOrigUpdate = false
) {
    if (!comObject || !hookFn) {
        return false;
    }

    std::lock_guard<std::mutex> lock(GetVtablePatchMutex());
    void** vtable = *reinterpret_cast<void***>(comObject);
    if (!vtable) {
        return false;
    }

    void* const hookAddr = reinterpret_cast<void*>(hookFn);
    if (vtable[slotIndex] == hookAddr) {
        return true;
    }

    const FnPtr orig = reinterpret_cast<FnPtr>(vtable[slotIndex]);
    void** const curPrimary = dispatch.primaryVtable.load(std::memory_order_relaxed);
    if (curPrimary == nullptr) {
        dispatch.primaryOrig.store(orig, std::memory_order_release);
        dispatch.primaryVtable.store(vtable, std::memory_order_release);
    } else if (curPrimary == vtable) {
        const FnPtr saved = dispatch.primaryOrig.load(std::memory_order_relaxed);
        const bool safeSystemReset = allowOrigUpdate &&
            (orig == saved || IsSameCodeModule(reinterpret_cast<const void*>(saved), reinterpret_cast<const void*>(orig)));
        if (!safeSystemReset) {
            // An external hook (e.g. Steam Overlay) wrapped this slot after us; do not overwrite orig or steal it back,
            // otherwise calling orig -> externalHook -> hookFn creates an infinite recursion loop.
            return true;
        }
        if (orig) {
            dispatch.primaryOrig.store(orig, std::memory_order_release);
        }
    } else {
        const size_t count = dispatch.extraCount.load(std::memory_order_relaxed);
        bool found = false;
        for (size_t i = 0; i < count; ++i) {
            if (dispatch.extra[i].vtable.load(std::memory_order_relaxed) == vtable) {
                const FnPtr saved = dispatch.extra[i].orig.load(std::memory_order_relaxed);
                const bool safeSystemReset = allowOrigUpdate &&
                    (orig == saved || IsSameCodeModule(reinterpret_cast<const void*>(saved), reinterpret_cast<const void*>(orig)));
                if (!safeSystemReset) {
                    return true;
                }
                if (orig) {
                    dispatch.extra[i].orig.store(orig, std::memory_order_release);
                }
                found = true;
                break;
            }
        }
        if (!found) {
            if (count < dispatch.extra.size()) {
                dispatch.extra[count].orig.store(orig, std::memory_order_relaxed);
                dispatch.extra[count].vtable.store(vtable, std::memory_order_relaxed);
                dispatch.extraCount.store(count + 1, std::memory_order_release);
            } else {
                UIE_LOG_ERROR("VtableDispatch capacity exceeded (vtable=%p, slot=%zu)",
                              static_cast<void*>(vtable), slotIndex);
                return false;
            }
        }
    }

    return WriteVtableSlot<FnPtr>(vtable, slotIndex, hookAddr);
}

template <typename FnPtr>
class ScopedPrimaryVtableRestore {
public:
    ScopedPrimaryVtableRestore(size_t slotIndex, FnPtr hookFn, VtableDispatch<FnPtr>& dispatch)
        : slotIndex_(slotIndex), hookFn_(hookFn), dispatch_(dispatch) {
        std::lock_guard<std::mutex> lock(GetVtablePatchMutex());
        void** vtbl = dispatch_.primaryVtable.load(std::memory_order_acquire);
        const FnPtr orig = dispatch_.primaryOrig.load(std::memory_order_acquire);
        if (vtbl && orig && vtbl[slotIndex_] == reinterpret_cast<void*>(hookFn_)) {
            if (WriteVtableSlot<FnPtr>(vtbl, slotIndex_, reinterpret_cast<void*>(orig))) {
                vtbl_ = vtbl;
                expectedOrig_ = orig;
            }
        }
    }

    ~ScopedPrimaryVtableRestore() {
        if (!vtbl_ || !hookFn_) return;
        std::lock_guard<std::mutex> lock(GetVtablePatchMutex());
        void* const current = vtbl_[slotIndex_];
        if (current != reinterpret_cast<void*>(hookFn_)) {
            if (current && current != reinterpret_cast<void*>(expectedOrig_)) {
                // Safe to adopt: hookFn_ was not installed in vtbl_ while this guard was active,
                // so current chains onto expectedOrig_ (or another system stub) without looping back to hookFn_.
                dispatch_.primaryOrig.store(reinterpret_cast<FnPtr>(current), std::memory_order_release);
            }
            WriteVtableSlot<FnPtr>(vtbl_, slotIndex_, reinterpret_cast<void*>(hookFn_));
        }
    }

    ScopedPrimaryVtableRestore(const ScopedPrimaryVtableRestore&) = delete;
    ScopedPrimaryVtableRestore& operator=(const ScopedPrimaryVtableRestore&) = delete;

private:
    void** vtbl_ = nullptr;
    size_t slotIndex_ = 0;
    FnPtr hookFn_ = nullptr;
    FnPtr expectedOrig_ = nullptr;
    VtableDispatch<FnPtr>& dispatch_;
};

} // namespace lotro_uie
