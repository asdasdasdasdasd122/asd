#pragma once
// Only the two hook types used in this R1 project. Unlike the upstream hook,
// trampolines are executable and the original instructions are verified first.
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <limits>

namespace hookdetail {
inline bool rel32(uintptr_t fromAfterInstruction, uintptr_t to, int32_t& out) {
    const int64_t delta = static_cast<int64_t>(to) - static_cast<int64_t>(fromAfterInstruction);
    if (delta < static_cast<int64_t>(std::numeric_limits<int32_t>::min()) ||
        delta > static_cast<int64_t>(std::numeric_limits<int32_t>::max())) return false;
    out = static_cast<int32_t>(delta);
    return true;
}
inline bool replace(void* destination, const void* expected, const void* replacement, SIZE_T count) {
    if (std::memcmp(destination, expected, count) != 0) return false;
    DWORD previous = 0;
    if (!VirtualProtect(destination, count, PAGE_EXECUTE_READWRITE, &previous)) return false;
    // Recheck after changing page protection: don't overwrite another patch.
    const bool unchanged = (std::memcmp(destination, expected, count) == 0);
    if (unchanged) {
        std::memcpy(destination, replacement, count);
        FlushInstructionCache(GetCurrentProcess(), destination, count);
    }
    DWORD dummy = 0;
    VirtualProtect(destination, count, previous, &dummy);
    return unchanged;
}
}

class rtdhook_call {
    unsigned char original[5]{};
    unsigned char replacement[5]{};
    uintptr_t site = 0, originalTarget = 0;
    bool installed = false, verified = false;
public:
    template<typename T>
    rtdhook_call(uintptr_t address, T* detour, uintptr_t expectedTarget = 0)
        : site(address) {
        if (!site || *reinterpret_cast<unsigned char*>(site) != 0xE8) return;
        std::memcpy(original, reinterpret_cast<void*>(site), 5);
        int32_t relative = 0;
        std::memcpy(&relative, original + 1, 4);
        originalTarget = static_cast<uintptr_t>(static_cast<int64_t>(site + 5) + relative);
        if (expectedTarget && originalTarget != expectedTarget) return;
        int32_t displacement = 0;
        if (!hookdetail::rel32(site + 5, reinterpret_cast<uintptr_t>(detour), displacement)) return;
        replacement[0] = 0xE8;
        std::memcpy(replacement + 1, &displacement, 4);
        verified = true;
    }
    bool ready() const { return verified; }
    bool install() {
        if (!verified || installed) return false;
        installed = hookdetail::replace(reinterpret_cast<void*>(site), original, replacement, 5);
        return installed;
    }
    void uninstall() {
        if (!installed) return;
        hookdetail::replace(reinterpret_cast<void*>(site), replacement, original, 5);
        installed = false;
    }
    ~rtdhook_call() { uninstall(); }
    uintptr_t getHookedFunctionAddress() const { return originalTarget; }
};

class rtdhook {
    uintptr_t site = 0, trampoline = 0;
    unsigned char original[6]{};
    unsigned char replacement[6]{};
    bool installed = false, verified = false;
public:
    template<typename T>
    rtdhook(uintptr_t address, T* detour, size_t prologueSize = 6)
        : site(address) {
        // Exactly the six complete relocation-free prologue bytes in R1
        // CPlayerTags::OnLostDevice. Any different code -> no hook.
        static const unsigned char expected[6] = {0x56,0x8B,0xF1,0x8B,0x46,0x04};
        if (!site || prologueSize != 6 ||
            std::memcmp(reinterpret_cast<void*>(site), expected, 6) != 0) return;
        std::memcpy(original, expected, 6);
        trampoline = reinterpret_cast<uintptr_t>(VirtualAlloc(nullptr, 11,
                                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!trampoline) return;
        int32_t back = 0, forward = 0;
        if (!hookdetail::rel32(trampoline + 11, site + 6, back) ||
            !hookdetail::rel32(site + 5, reinterpret_cast<uintptr_t>(detour), forward)) return;
        std::memcpy(reinterpret_cast<void*>(trampoline), original, 6);
        unsigned char* jump = reinterpret_cast<unsigned char*>(trampoline + 6);
        jump[0] = 0xE9;
        std::memcpy(jump + 1, &back, 4);
        DWORD oldProtection = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(trampoline), 11,
                            PAGE_EXECUTE_READ, &oldProtection)) return;
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(trampoline), 11);
        replacement[0] = 0xE9;
        std::memcpy(replacement + 1, &forward, 4);
        replacement[5] = 0x90;
        verified = true;
    }
    bool ready() const { return verified; }
    bool install() {
        if (!verified || installed) return false;
        installed = hookdetail::replace(reinterpret_cast<void*>(site), original, replacement, 6);
        return installed;
    }
    uintptr_t getTrampoline() const { return installed ? trampoline : 0; }
    void uninstall() {
        if (!installed) return;
        hookdetail::replace(reinterpret_cast<void*>(site), replacement, original, 6);
        installed = false;
    }
    ~rtdhook() {
        uninstall();
        // If another mod still executes the trampoline, freeing is unsafe.
        // Modules are only unloaded on process exit in the supported workflow.
        // Keep its tiny 11-byte allocation until the process terminates.
    }
};
