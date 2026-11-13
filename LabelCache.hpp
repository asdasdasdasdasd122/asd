#pragma once
// EXPERIMENTAL R1 adapter. Default ON only after verified hooks succeed.
// Never edits samp.dll on disk.
// The hook is at the FINAL CFonts draw call, after the stock visibility and
// projection checks. The second hook is at the stock CLabel::End callsite.
#include "TextPipeline.hpp"
#include "LabelAtlas.hpp"
#include <windows.h>
#include <d3dx9.h>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <string>
#include <string_view>
#include <array>

namespace labelcache {
// State is deliberately single-threaded: SA-MP's D3D9 render thread only.
// Do not install this adapter simultaneously with another 3D label renderer.
class Adapter final : public samptext::IRenderer {
public:
    Adapter();
    ~Adapter() override;
    void drawText(void* fonts, ID3DXSprite* stockSprite, const char* text,
                  RECT rect, int flags, D3DCOLOR color, BOOL shadow);
    void afterStockEnd();
    void beforeStockEnd();
    void deviceLost();
    samptext::BakeResult bake(samptext::Id, const samptext::Entry&) override;
    bool draw(samptext::Id, const samptext::Entry&, samptext::Position) override;
    void release(samptext::Id) override;
private:
    static constexpr std::size_t LIMIT_SLOTS = 512;
    struct Texture {
        textgpu::AtlasSlot atlas;
        LONG originX = 0; // Original R1 flags=0x101 centers text around rect.left.
    };
    struct Slot {
        samptext::Id id;
        DWORD lastUsed;
        std::string text;
        D3DCOLOR color;
        BOOL shadow;
    };
    // Hash collisions are resolved by comparing ALL original bytes and style.
    // Lookup uses string_view; only a new key allocates an owned string.
    using Slots = std::unordered_multimap<std::size_t, Slot>;
    static std::size_t keyHash(std::string_view text, D3DCOLOR color, BOOL shadow);
    bool drawSlot(std::uint16_t slot, samptext::Position pos);
    void beginBatch();
    void publishStats();
    bool eligible(const char* text, int flags, const RECT& rect) const;
    void original(void* fonts, ID3DXSprite* stockSprite, const char* text,
                  RECT rect, int flags, D3DCOLOR color, BOOL shadow);
    void sweep(DWORD now);
    IDirect3DDevice9* device_ = nullptr; // non-owning
    ID3DXSprite* stockSprite_ = nullptr;  // non-owning, only used inside Draw
    void* fonts_ = nullptr;               // non-owning CFonts instance
    textgpu::Baker baker_;
    textgpu::CachedBatch cachedBatch_;
    textgpu::LabelAtlas atlas_;
    std::array<Texture, LIMIT_SLOTS> textures_{};
    Slots slots_;
    std::unique_ptr<samptext::Pipeline> pipeline_;
    samptext::SlotIds<LIMIT_SLOTS> ids_;
    DWORD lastSweep_ = 0;
    bool batchActive_ = false;
    int batchMode_ = 1;
    DWORD batchNow_ = 0;
    std::array<std::uint32_t, 12> stats_{};
    IDirect3DTexture9* lastDrawTexture_ = nullptr; // comparison only, batch-local
};
extern Adapter gLabels;
bool cacheEnabled();
void markHooksReady();
int currentStatus();
// Verify callsites BEFORE constructing hooks; do not bypass version check.
bool verifyR1Callsites(uintptr_t base);
} // namespace labelcache

// Original CFonts__DrawLittleText is __thiscall with 36 stack argument bytes.
// __fastcall uses ECX for 'fonts' and EDX as an unused register argument;
// all 36 bytes of arguments remain on the stack, matching the original RET 24h.
void __fastcall CLabel__FinalFontDraw(void* fonts, void* unusedEDX,
    ID3DXSprite* sprite, const char* text, RECT rect, int flags,
    D3DCOLOR color, BOOL shadow);
void __fastcall CLabel__PoolEnd(void* label);
