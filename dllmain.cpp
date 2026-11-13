#include "pch.h"
#include "OptimizedNametags.h"
#if ENABLE_LABEL_CACHE_EXPERIMENTAL
#include "LabelCache.hpp"
#endif

namespace {
rtdhook_call* mainloopHook = nullptr;
bool triedInstallation = false;
bool nametagHooksActive = false;
bool chatNoticeSent = false;
DWORD chatReadyAt = 0;
void* observedChat = nullptr;
#if ENABLE_LABEL_CACHE_EXPERIMENTAL
rtdhook_call* labelFontHook = nullptr;
rtdhook_call* labelEndHook = nullptr;
#endif
void log(const char* message) { OutputDebugStringA(message); }

// Fail closed for unknown GTA and for altered SA-MP text/shadow routines.
// These bytes were checked against the GTA executable supplied by the user.
bool verifiedGtaCallsite() {
    if (reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)) != 0x00400000)
        return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(0x00400000);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        dos->e_lfanew > 0x1000) return false;
    const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS*>(0x00400000 + dos->e_lfanew);
    if (pe->Signature != IMAGE_NT_SIGNATURE ||
        pe->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        pe->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        pe->OptionalHeader.SizeOfImage <= 0x562000 ||
        pe->OptionalHeader.AddressOfEntryPoint != 0x424570) return false;
    static const unsigned char callAndNextTwo[] = {
        0xE8, 0xA3, 0x31, 0x02, 0x00, // 0x53E968 -> 0x561B10
        0xE8, 0xDE, 0x89, 0x1E, 0x00,
        0xE8, 0x89, 0xAE, 0x1D, 0x00
    };
    static const unsigned char targetPrologue[] = {
        0x8B, 0x0D, 0x28, 0xCB, 0xB7, 0x00,
        0x83, 0xEC, 0x0C, 0x85, 0xC9
    };
    return std::memcmp(reinterpret_cast<const void*>(0x53E968), callAndNextTwo,
                       sizeof(callAndNextTwo)) == 0 &&
           std::memcmp(reinterpret_cast<const void*>(0x561B10), targetPrologue,
                       sizeof(targetPrologue)) == 0;
}

bool verifiedOriginalShadow(uintptr_t base) {
    // 0/1/2-pass Shadow Lite variants are intentionally NOT accepted.
    // This module generates the original 4-sided outline into a cache.
    static const unsigned char enabled[] = {0x6A, 0x01};
    static const unsigned char first[] = {
        0x8B,0x54,0x24,0x24,0x8B,0x46,0x0C,0x68,0x00,0x00,0x00,0xFF,0x55
    };
    static const unsigned char second[] = {
        0x8B,0x44,0x24,0x24,0x8B,0x54,0x24,0x20,0x68,0x00,0x00,0x00,0xFF
    };
    return std::memcmp(reinterpret_cast<const void*>(base + 0x68849), enabled,
                       sizeof(enabled)) == 0 &&
           std::memcmp(reinterpret_cast<const void*>(base + 0x66EBC), first,
                       sizeof(first)) == 0 &&
           std::memcmp(reinterpret_cast<const void*>(base + 0x66EE1), second,
                       sizeof(second)) == 0;
}

// Show the notice on the game thread only after the verified R1 hooks and
// chat are ready. CChat::AddMessage is 0x645A0, pChat is 0x21A0E4 in R1.
// Do not call SA-MP UI functions from DllMain or a worker thread.
void showLoadNoticeWhenReady() {
    if (chatNoticeSent || sampGetVersion() != SAMPVER::SAMP_037_R1) return;
#if ENABLE_LABEL_CACHE_EXPERIMENTAL
    if (!nametagHooksActive || labelcache::currentStatus() != 2) return;
#else
    // The message identifies the combined nametag + 3D-label plugin.
    return;
#endif
    const uintptr_t base = sampGetBase();
    void* chat = *reinterpret_cast<void**>(base + 0x21A0E4);
    if (chat != observedChat) {
        observedChat = chat;
        chatReadyAt = chat ? GetTickCount() : 0;
        return;
    }
    if (!chat || GetTickCount() - chatReadyAt < 1000) return;

    // R1 uses Windows-1251, not UTF-8, for its chat strings.
    // Exact text: [SAMP3DTextOptimizer] Загружен. Автор: Yuko
    static const char notice[] =
        "{69d1d1}[OptimizedText] {dddddd}"
        "\xC7\xE0\xE3\xF0\xF3\xE6\xE5\xED\x2E\x20"
        "\xC0\xE2\xF2\xEE\xF0\x3A\x20"
        "{69d1d1}Yuko";
    chatNoticeSent = true;
    reinterpret_cast<void(__thiscall*)(void*, D3DCOLOR, const char*)>(
        base + 0x645A0)(chat, 0xFFFFFFFFu, notice);
    log("[SAMP3DTextOptimizer] Startup message added to chat.\n");
}

void __cdecl mainloop() {
    if (!triedInstallation && sampGetVersion() == SAMPVER::SAMP_037_R1) {
        triedInstallation = true;
        const uintptr_t base = sampGetBase();
        // Verify BOTH R1 draw and final sprite-End CALLs, as well as the shadow.
        static const unsigned char expectedDraw[] = {0xE8,0x25,0x77,0xFF,0xFF};
        static const unsigned char expectedEnd[]  = {0xE8,0xFD,0x76,0xFF,0xFF};
        const bool correctCalls =
            std::memcmp(reinterpret_cast<void*>(base + 0x70F96), expectedDraw, 5) == 0 &&
            std::memcmp(reinterpret_cast<void*>(base + 0x70FAE), expectedEnd, 5) == 0;
        if (!correctCalls || !verifiedOriginalShadow(base)) {
            log("[Nametags R1] Incompatible R1 call/shadow patch; vanilla renderer preserved.\n");
        } else {
            auto* lost = new rtdhook(base + 0x68F70, &CPlayerTags__OnLostDevice, 6);
            auto* draw = new rtdhook_call(base + 0x70F96, &CPlayerTags__Draw_Naked,
                                         base + 0x686C0);
            auto* end = new rtdhook_call(base + 0x70FAE, &CPlayerTags__End,
                                        base + 0x686B0);
            // Store hook pointers before installation: callbacks can never see null hooks.
            gInstance.mHooks.CPlayerTags__OnLostDevice = lost;
            gInstance.mHooks.CPlayerTags__End = end;
            gInstance.mHooks.CPlayerTags__Draw = draw;
            if (lost->ready() && draw->ready() && end->ready() &&
                lost->install() && end->install() && draw->install()) {
                nametagHooksActive = true;
                log("[Nametags R1] Deferred nametag hooks installed.\n");
            } else {
                // Undo in reverse order and never keep dangling hook pointers.
                draw->uninstall();
                end->uninstall();
                lost->uninstall();
                gInstance.mHooks.CPlayerTags__Draw = nullptr;
                gInstance.mHooks.CPlayerTags__End = nullptr;
                gInstance.mHooks.CPlayerTags__OnLostDevice = nullptr;
                delete draw;
                delete end;
                delete lost;
                log("[Nametags R1] Signature/install failed; vanilla code preserved.\n");
            }
        }
    }
#if ENABLE_LABEL_CACHE_EXPERIMENTAL
    // Separate fail-closed transaction: a label hook failure does not
    // disable or uninstall the working v4 nametag hooks. The R1 callsite at
    // +0x67701 is AFTER the stock CLabel visibility/projection code; +0x15B6
    // ends the sprite batch before we bake any offscreen label textures.
    static bool labelAttempted = false;
    if (!labelAttempted && sampGetVersion() == SAMPVER::SAMP_037_R1) {
        labelAttempted = true;
        const uintptr_t base = sampGetBase();
        if (nametagHooksActive && labelcache::verifyR1Callsites(base) &&
            verifiedOriginalShadow(base)) {
            auto* end = new rtdhook_call(base + 0x15B6, &CLabel__PoolEnd,
                                         base + 0x675A0);
            auto* font = new rtdhook_call(base + 0x67701,
                        &CLabel__FinalFontDraw, base + 0x66E00);
            if (end->ready() && font->ready() &&
                end->install() && font->install()) {
                labelEndHook = end;
                labelFontHook = font;
                labelcache::markHooksReady();
                log("[Labels R1 v8] Hooks active; optimized by default.\n");
            } else {
                font->uninstall();
                end->uninstall();
                delete font;
                delete end;
                log("[Labels R1] Verification/install failed: labels remain stock.\n");
            }
        } else {
            log("[Labels R1] Unexpected code bytes: labels remain stock.\n");
        }
    }
#endif
    showLoadNoticeWhenReady();
    if (mainloopHook && mainloopHook->getHookedFunctionAddress())
        reinterpret_cast<void(__cdecl*)()>(mainloopHook->getHookedFunctionAddress())();
}
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        // Match a 15-byte instruction window and its exact target prologue,
        // rather than patching every executable that happens to have a CALL.
        if (!verifiedGtaCallsite()) {
            log("[Nametags R1] Unknown GTA executable; no hooks installed.\n");
            return TRUE;
        }
        mainloopHook = new rtdhook_call(0x53E968, &mainloop, 0x561B10);
        if (!mainloopHook->ready() || !mainloopHook->install()) {
            delete mainloopHook;
            mainloopHook = nullptr;
            log("[Nametags R1] GTA main-loop CALL unavailable; no hooks.\n");
        }
    }
    return TRUE;
}
