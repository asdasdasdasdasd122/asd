#include "pch.h"
#include "OptimizedNametags.h"
#include "NametagDiagnosticText.hpp"
#if ENABLE_LABEL_CACHE_EXPERIMENTAL
#include "LabelCache.hpp"
#endif
#include <cstring>

OptimizedNametags gInstance;

namespace {
using OriginalDraw = void(__thiscall*)(uintptr_t, CVector*, const char*,
                                       D3DCOLOR, float, bool, int);
void drawOriginal(uintptr_t self, CVector* pos, const char* text,
                  D3DCOLOR color, float distance, bool drawStatus, int status) {
    // Flush cached premultiplied textures before the stock straight-alpha font.
    gInstance.mCachedBatch.finish();
    reinterpret_cast<OriginalDraw>(sampGetBase() + 0x686C0)(
        self, pos, text, color, distance, drawStatus, status);
}
bool validName(const char* name) {
    if (!name || !*name) return false;
    for (size_t i = 0; i < sizeof(OptimizedNametags::NameTag::name); ++i)
        if (name[i] == '\0') return true;
    return false;
}
}

bool OptimizedNametags::shouldRedrawNametag(const NameTag& nt, D3DCOLOR color,
        bool isAfk, ID3DXFont* auxFont, const char* name) const {
    return !nt.gpu.texture || nt.redraw || nt.isAfk != isAfk || nt.color != color ||
           (isAfk && nt.lastAuxFont != auxFont) || std::strcmp(nt.name, name) != 0;
}

void OptimizedNametags::releaseElements(NameTag& nt) {
    mUsedBytes -= nt.gpu.bytes;
    nt.gpu.release();
    nt.lastAuxFont = nullptr;
    nt.redraw = true;
}

void OptimizedNametags::sweepUnused(DWORD now) {
    if (static_cast<DWORD>(now - mLastSweep) < 5000) return;
    mLastSweep = now;
    for (auto& nt : mNametags) {
        if (static_cast<DWORD>(now - nt.lastUsed) > UNUSED_CACHE_MS) {
            releaseElements(nt);
            nt.pending = false; // cancel stale failures even if no texture exists
            nt.retry.reset();
        }
    }
}

bool OptimizedNametags::syncDevice() {
    auto* current = *reinterpret_cast<IDirect3DDevice9**>(0xC97C28);
    void* fonts = sampGetFonts();
    if (current != mD3DDevice || fonts != mFonts) {
        if (current == mD3DDevice) mCachedBatch.finish();
        onLostDevice();
        mD3DDevice = current;
        mFonts = fonts;
    }
    return current != nullptr && fonts != nullptr;
}

void OptimizedNametags::onLostDevice() {
    mCachedBatch.discard(); // a lost device cannot flush an old sprite batch
    for (auto& nt : mNametags) {
        releaseElements(nt);
        nt.pending = false;
        nt.pendingAuxFont = nullptr;
        nt.retry.reset();
    }
    mBaker.reset(); // textures must be released before the owned QI reference
    mBakeCursor.reset();
    mD3DDevice = nullptr;
    mFonts = nullptr;
}

void OptimizedNametags::queue(NameTag& nt, const char* text, D3DCOLOR color,
                              bool afk, ID3DXFont* auxFont) {
    if (!nt.pending || nt.pendingColor != color || nt.pendingAfk != afk ||
        nt.pendingAuxFont != auxFont || std::strcmp(nt.pendingName, text) != 0)
        nt.retry.reset();
    nt.pendingColor = color;
    nt.pendingAfk = afk;
    nt.pendingAuxFont = auxFont;
    std::memcpy(nt.pendingName, text, std::strlen(text) + 1);
    nt.pending = true;
}

samptext::BakeResult OptimizedNametags::bake(NameTag& nt) {
    using samptext::BakeResult;
    if (!nt.pending || !mD3DDevice || !mFonts || !mBaker.ensure(mD3DDevice))
        return BakeResult::RetryLater;
    // An obsolete key is never drawn, including one rejected by the size check.
    releaseElements(nt);
    const SIZE originalSize = sampGetMeasuredTextSize(nt.pendingName);
#if NAMETAG_CACHE_SHOW_MARKER
    const std::string bakedText = nametagcache::diagnosticText(nt.pendingName);
    const SIZE bakedSize = sampGetMeasuredTextSize(bakedText.c_str());
    if (bakedSize.cx < originalSize.cx || bakedSize.cy < originalSize.cy)
        return BakeResult::Unsupported;
    const LONG markerWidth = bakedSize.cx - originalSize.cx;
    const char* renderText = bakedText.c_str();
#else
    const SIZE bakedSize = originalSize;
    constexpr LONG markerWidth = 0;
    const char* renderText = nt.pendingName;
#endif
    if (bakedSize.cx <= 0 || bakedSize.cy <= 0 || bakedSize.cx > 974 || bakedSize.cy > 203)
        return BakeResult::Unsupported;
    const UINT width = static_cast<UINT>(bakedSize.cx) + 50u;
    const UINT height = static_cast<UINT>(bakedSize.cy) + 53u;
    ID3DXFont* auxFont = nt.pendingAfk ? sampGetAuxFont() : nullptr;
    const auto result = mBaker.bake(width, height, LIMIT_BYTES - mUsedBytes, nt.gpu,
        [&](ID3DXSprite* sprite) {
            RECT rect{1, 1, static_cast<LONG>(width), static_cast<LONG>(height)};
            sampDrawText(sprite, renderText, rect, nt.pendingColor, TRUE);
            if (auxFont) {
                rect.top += originalSize.cy + 4;
                rect.left = markerWidth + static_cast<LONG>(
                    (1.f + static_cast<float>(originalSize.cx) / 2.f) / 2.6f);
                if (!auxFont->DrawTextA(sprite, "C", 1, &rect,
                    DT_NOCLIP | DT_LEFT, D3DCOLOR_XRGB(0, 0, 0))) return false;
                rect.left += 3;
                rect.top -= 1;
                if (!auxFont->DrawTextA(sprite, "E", 1, &rect,
                    DT_NOCLIP | DT_LEFT, 0xFFFFFFFF)) return false;
            }
            return true;
        });
    if (result != BakeResult::Ready) return result;
    mUsedBytes += nt.gpu.bytes;
    nt.center = 1.f + static_cast<float>(markerWidth) + static_cast<float>(originalSize.cx) / 2.f;
    nt.lastAuxFont = auxFont; // comparison only, actual drawing always re-reads owner
    nt.color = nt.pendingColor;
    nt.isAfk = nt.pendingAfk;
    std::memcpy(nt.name, nt.pendingName, std::strlen(nt.pendingName) + 1);
    nt.redraw = false;
    nt.pending = false;
    nt.retry.reset();
    return BakeResult::Ready;
}

void OptimizedNametags::preparePendingAfterEnd() {
    const DWORD now = GetTickCount();
    sweepUnused(now);
    mBakeCursor.run(MAX_PLAYERS, 4,
        [&](std::size_t id) {
            const auto& nt = mNametags[id];
            return nt.pending && nt.retry.allowed(now) &&
                static_cast<DWORD>(now - nt.lastUsed) <= UNUSED_CACHE_MS;
        },
        [&](std::size_t id) {
            auto& nt = mNametags[id];
            const auto result = bake(nt);
            if (result != samptext::BakeResult::Ready) nt.retry.failed(result, now);
        });
}

void __fastcall CPlayerTags__Draw(uintptr_t self, int id, CVector* playerPos,
        const char* text, D3DCOLOR color, float distance, bool drawStatus, int status) {
    if (id < 0 || id >= OptimizedNametags::MAX_PLAYERS || !playerPos || !validName(text)) {
        if (id >= 0 && id < OptimizedNametags::MAX_PLAYERS)
            gInstance.mNametags[id].pending = false;
        drawOriginal(self, playerPos, text, color, distance, drawStatus, status);
        return;
    }
    auto& instance = gInstance;
    const bool ready = instance.syncDevice();
    auto& nt = instance.mNametags[id];
    nt.lastUsed = GetTickCount();
    const bool isAfk = drawStatus && status == 2;
    ID3DXFont* auxFont = ready && isAfk ? sampGetAuxFont() : nullptr;
    auto* stockSprite = *reinterpret_cast<ID3DXSprite**>(self + 8);
    if (ready && stockSprite && !instance.shouldRedrawNametag(nt, color, isAfk, auxFont, text)) {
        D3DXVECTOR3 tagPos{playerPos->x, playerPos->y,
            playerPos->z + 0.21f + distance * 0.05f};
        D3DVIEWPORT9 viewport{};
        if (SUCCEEDED(instance.mD3DDevice->GetViewport(&viewport))) {
            D3DXVECTOR3 projected{};
            D3DXMATRIX identity{};
            D3DXMatrixIdentity(&identity);
            D3DXVec3Project(&projected, &tagPos, &viewport,
                sampGetProjectionMatrix(), sampGetViewMatrix(), &identity);
            projected.x = static_cast<float>(static_cast<LONG>(projected.x - nt.center));
            projected.y = static_cast<float>(static_cast<LONG>(projected.y));
            if (instance.mCachedBatch.draw(instance.mBaker.device(), stockSprite, nt.gpu.texture, projected)) {
                nt.pending = false;
                nt.retry.reset();
                return;
            }
        }
        nt.redraw = true; // a failed GPU draw also obeys the retry policy
    }
    drawOriginal(self, playerPos, text, color, distance, drawStatus, status);
    if (ready && stockSprite) instance.queue(nt, text, color, isAfk, auxFont);
}

__declspec(naked) void CPlayerTags__Draw_Naked() {
    __asm {
        mov edx, edi // verified R1 callsite; EDI is the slot/player ID
        jmp CPlayerTags__Draw
    }
}

void __fastcall CPlayerTags__End(void* self) {
    gInstance.mCachedBatch.finish();
    reinterpret_cast<void(__thiscall*)(void*)>(sampGetBase() + 0x686B0)(self);
    gInstance.syncDevice();
    gInstance.preparePendingAfterEnd();
}

void __fastcall CPlayerTags__OnLostDevice(void* self) {
#if ENABLE_LABEL_CACHE_EXPERIMENTAL
    labelcache::gLabels.deviceLost();
#endif
    gInstance.onLostDevice();
    auto* hook = gInstance.mHooks.CPlayerTags__OnLostDevice;
    if (hook && hook->getTrampoline())
        reinterpret_cast<void(__thiscall*)(void*)>(hook->getTrampoline())(self);
}
