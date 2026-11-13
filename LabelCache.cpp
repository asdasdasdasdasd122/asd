#include "pch.h"
#if ENABLE_LABEL_CACHE_EXPERIMENTAL
#include "LabelCache.hpp"
#include "LabelEligibility.hpp"
#include "LabelDiagnosticText.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <atomic>

namespace labelcache {
Adapter gLabels;
// 0 = stock 3D-label text, 1 = cache, 2 = explicitly selected hidden-text diagnostic.
// This is a runtime-only switch. It does not edit samp.dll on disk.
namespace {
volatile LONG gMode = 0;           // stock until BOTH hooks install successfully
std::array<std::atomic<std::uint32_t>, 12> gStats{};
volatile LONG gHooksReady = 0;     // only true after BOTH 3D-label hooks install
}
bool cacheEnabled() {
    return InterlockedCompareExchange(&gMode, 0, 0) == 1;
}
void markHooksReady() {
    // Enable caching by default ONLY after both verified label hooks installed.
    // Unknown builds and partial hook failures remain on the untouched renderer.
    InterlockedExchange(&gMode, 1);
    InterlockedExchange(&gHooksReady, 1);
}
int currentStatus() {
    if (!InterlockedCompareExchange(&gHooksReady, 0, 0)) return 0;
    return static_cast<int>(InterlockedCompareExchange(&gMode, 0, 0)) + 1;
}
namespace {
constexpr int PADDING = 4;
constexpr uintptr_t R1_FONT_DRAW = 0x66E00;
using StockFontDraw = void(__thiscall*)(void*, ID3DXSprite*, const char*,
                                       RECT, int, D3DCOLOR, BOOL);
// Read-only on-disk bytes and virtual addresses were checked against the user's
// R1 DLL. Also check nearby instructions; a CALL to the same target is not enough.
}

bool verifyR1Callsites(uintptr_t base) {
    if (!base) return false;

    // The two MOV instructions contain absolute addresses. Windows adjusts
    // those DWORD operands when samp.dll is loaded away from 0x10000000.
    // Compare the relocated targets AND all unaffected opcode/CALL bytes.
    // Do not weaken the signature check or patch an unknown SA-MP build.
    static const unsigned char fontPrefix[] = {0x50, 0x51, 0x8B, 0x0D};
    static const unsigned char fontSuffix[] = {
        0x89, 0x53, 0x0C, 0xE8, 0xFA, 0xF6, 0xFF, 0xFF
    };
    static const unsigned char endPrefix[] = {0x8B, 0x0D};
    static const unsigned char endSuffix[] = {
        0x85, 0xC9, 0x5E, 0x5D, 0x5B, 0x74, 0x05,
        0xE8, 0xE5, 0x5F, 0x06, 0x00
    };

    const auto* font = reinterpret_cast<const unsigned char*>(base + 0x676F6);
    const auto* end = reinterpret_cast<const unsigned char*>(base + 0x15A9);
    std::uint32_t fontAddress = 0, endAddress = 0;
    std::memcpy(&fontAddress, font + 4, sizeof(fontAddress));
    std::memcpy(&endAddress, end + 2, sizeof(endAddress));

    return std::memcmp(font, fontPrefix, sizeof(fontPrefix)) == 0 &&
           fontAddress == static_cast<std::uint32_t>(base + 0x21A0FC) &&
           std::memcmp(font + 8, fontSuffix, sizeof(fontSuffix)) == 0 &&
           std::memcmp(end, endPrefix, sizeof(endPrefix)) == 0 &&
           endAddress == static_cast<std::uint32_t>(base + 0x21A0C0) &&
           std::memcmp(end + 6, endSuffix, sizeof(endSuffix)) == 0;
}
Adapter::Adapter() : pipeline_(new samptext::Pipeline(*this, LIMIT_SLOTS, 512)) {
    slots_.reserve(LIMIT_SLOTS);
}
Adapter::~Adapter() { deviceLost(); }

void Adapter::beginBatch() {
    if (batchActive_) return;
    batchActive_ = true;
    batchMode_ = static_cast<int>(InterlockedCompareExchange(&gMode, 0, 0));
    batchNow_ = GetTickCount();
    stats_.fill(0);
    lastDrawTexture_ = nullptr;
}
void Adapter::publishStats() {
    stats_[7] = static_cast<std::uint32_t>(pipeline_->queuedJobs());
    stats_[8] = static_cast<std::uint32_t>(slots_.size());
    stats_[9] = atlas_.pageCount();
    stats_[10] = static_cast<std::uint32_t>(atlas_.bytes());
    for (std::size_t i = 0; i < stats_.size(); ++i)
        gStats[i].store(stats_[i], std::memory_order_relaxed);
    batchActive_ = false;
    lastDrawTexture_ = nullptr;
}
std::size_t Adapter::keyHash(std::string_view text, D3DCOLOR color, BOOL shadow) {
    auto hash = std::hash<std::string_view>{}(text);
    hash ^= static_cast<std::size_t>(color) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
    hash ^= static_cast<std::size_t>(shadow != 0) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
    return hash;
}

void Adapter::original(void* fonts, ID3DXSprite* sprite, const char* text,
                       RECT rect, int flags, D3DCOLOR color, BOOL shadow) {
    cachedBatch_.finish();
    lastDrawTexture_ = nullptr;
    reinterpret_cast<StockFontDraw>(sampGetBase() + R1_FONT_DRAW)(
        fonts, sprite, text, rect, flags, color, shadow);
}

bool Adapter::eligible(const char* text, int flags, const RECT& rect) const {
    if (!text || !device_ || !stockSprite_ || !fonts_ || flags != 0x101 ||
        rect.right <= rect.left || rect.bottom <= rect.top) return false;
    // Up to 512 raw CP1251 bytes, including color tags and empty LF lines.
    // R1-tagged / LF-separated strings: preserve EVERY byte for stock
    // measurement and stock rasterization. Malformed cases stay vanilla.
    return eligibleCp1251Label(text);
}

void Adapter::drawText(void* fonts, ID3DXSprite* sprite, const char* text,
                       RECT rect, int flags, D3DCOLOR color, BOOL shadow) {
    beginBatch();
    ++stats_[0];
    const auto fallback = [&]() {
        ++stats_[2];
        original(fonts, sprite, text, rect, flags, color, shadow);
    };
    if (batchMode_ == 2) return; // explicitly selected diagnostic, never the default
    if (batchMode_ != 1 || !fonts || !sprite || !text) { fallback(); return; }
    auto* label = *reinterpret_cast<void**>(sampGetBase() + 0x21A0C0);
    auto* dev = label ? *reinterpret_cast<IDirect3DDevice9**>(label) : nullptr;
    if (dev != device_ || fonts != fonts_) {
        if (dev == device_) cachedBatch_.finish();
        deviceLost();
        device_ = dev;
        fonts_ = fonts;
    }
    stockSprite_ = sprite;
    if (!eligible(text, flags, rect)) { fallback(); return; }
    const std::string_view view(text); // bounded by the eligibility check above
    const BOOL normalizedShadow = shadow ? TRUE : FALSE;
    const auto hash = keyHash(view, color, normalizedShadow);
    const auto range = slots_.equal_range(hash);
    auto it = range.first;
    for (; it != range.second; ++it) {
        const auto& slot = it->second;
        if (slot.color == color && slot.shadow == normalizedShadow &&
            std::string_view(slot.text) == view) break;
    }
    if (it == range.second) {
        std::uint16_t index = 0;
        if (!ids_.acquire(index)) { ++stats_[3]; fallback(); return; }
        const samptext::Id id{samptext::Kind::Label3D, index};
        it = slots_.emplace(hash, Slot{id, batchNow_, std::string(view), color, normalizedShadow});
    }
    auto& slot = it->second;
    slot.lastUsed = batchNow_;
    const samptext::Position position{static_cast<float>(rect.left), static_cast<float>(rect.top), 1.f};
    // Slot keys are immutable until erased. A ready atlas entry is already the
    // exact text/color/shadow match: no Request string, std::function or cache
    // map lookups are needed on the steady-state path.
    if (textures_[slot.id.slot].atlas.valid()) {
        if (drawSlot(slot.id.slot, position)) { ++stats_[1]; return; }
        pipeline_->erase(slot.id); // failed draw: invalidate before scheduling a retry
    }
    samptext::Style style{};
    style.color = color;
    style.flags = static_cast<std::uint32_t>(flags) | (shadow ? 0x10000u : 0u);
    const samptext::Request request{slot.id, slot.text, style, position,
                                   samptext::Eligibility::TextOnlyVerified};
    if (pipeline_->drawOrOriginal(request, fallback, batchNow_) == samptext::Pipeline::Result::Cached)
        ++stats_[1];
}

samptext::BakeResult Adapter::bake(samptext::Id id, const samptext::Entry& entry) {
    using samptext::BakeResult;
    ++stats_[4];
    const auto fail = [&](BakeResult result) { ++stats_[6]; return result; };
    if (id.kind != samptext::Kind::Label3D || id.slot >= LIMIT_SLOTS ||
        !device_ || !fonts_ || entry.text.empty() || textures_[id.slot].atlas.valid() ||
        !baker_.ensure(device_)) return fail(BakeResult::RetryLater);
    const std::string bakedText = diagnosticText(entry.text);
    const SIZE measured = sampGetMeasuredTextSize(bakedText.c_str());
    if (measured.cx <= 0 || measured.cy <= 0 || measured.cx > 504 || measured.cy > 176)
        return fail(BakeResult::Unsupported);
    const UINT width = static_cast<UINT>(measured.cx + PADDING * 2);
    const UINT height = static_cast<UINT>(measured.cy + PADDING * 2);
    const LONG originX = static_cast<LONG>(PADDING + measured.cx / 2);
    textgpu::AtlasSlot region;
    const auto result = atlas_.bake(baker_, width, height, region,
        [&](ID3DXSprite* sprite, const RECT& source) {
            // Translate the SAME stock centered text into its atlas region.
            const LONG x = source.left + originX, y = source.top + PADDING;
            RECT rect{x, y, x + 1, y + 1};
            const bool shadow = (entry.style.flags & 0x10000u) != 0;
            original(fonts_, sprite, bakedText.c_str(), rect, 0x101,
                     entry.style.color, shadow ? TRUE : FALSE);
            return true;
        });
    if (result != BakeResult::Ready) return fail(result);
    textures_[id.slot] = {region, originX};
    ++stats_[5];
    return BakeResult::Ready;
}

bool Adapter::drawSlot(std::uint16_t slot, samptext::Position pos) {
    if (slot >= LIMIT_SLOTS || !stockSprite_) return false;
    const auto& item = textures_[slot];
    auto* texture = atlas_.texture(item.atlas);
    if (!texture) return false;
    const D3DXVECTOR3 position{pos.x - static_cast<float>(item.originX), pos.y - PADDING, 0.0f};
    if (!cachedBatch_.draw(baker_.device(), stockSprite_, texture, position, &item.atlas.source))
        return false;
    if (texture != lastDrawTexture_) { ++stats_[11]; lastDrawTexture_ = texture; }
    return true;
}
bool Adapter::draw(samptext::Id id, const samptext::Entry&, samptext::Position pos) {
    return id.kind == samptext::Kind::Label3D && drawSlot(id.slot, pos);
}
void Adapter::release(samptext::Id id) {
    if (id.kind != samptext::Kind::Label3D || id.slot >= LIMIT_SLOTS) return;
    atlas_.release(textures_[id.slot].atlas);
    textures_[id.slot] = {};
}

void Adapter::sweep(DWORD now) {
    if (static_cast<DWORD>(now - lastSweep_) < 3000u) return;
    lastSweep_ = now;
    for (auto it = slots_.begin(); it != slots_.end();) {
        if (static_cast<DWORD>(now - it->second.lastUsed) > 15000u) {
            pipeline_->erase(it->second.id);
            ids_.release(it->second.id.slot);
            it = slots_.erase(it);
        } else ++it;
    }
}

void Adapter::beforeStockEnd() { cachedBatch_.finish(); }

void Adapter::afterStockEnd() {
    beginBatch(); // also publish zero draws when the stock loop is empty
    if (batchMode_ != 1) {
        if (device_ || !slots_.empty()) deviceLost();
        publishStats();
        return;
    }
    const auto* label = *reinterpret_cast<void**>(sampGetBase() + 0x21A0C0);
    auto* current = label ? *reinterpret_cast<IDirect3DDevice9* const*>(label) : nullptr;
    if (current != device_ || sampGetFonts() != fonts_) {
        deviceLost();
        publishStats();
        return;
    }
    sweep(batchNow_);
    if (device_ && fonts_) pipeline_->afterBatch(2, batchNow_);
    stockSprite_ = nullptr;
    publishStats();
}
void Adapter::deviceLost() {
    cachedBatch_.discard();
    if (pipeline_) pipeline_->clear();
    for (auto& texture : textures_) texture = {};
    atlas_.reset();
    slots_.clear();
    ids_.reset();
    baker_.reset();
    lastSweep_ = 0;
    lastDrawTexture_ = nullptr;
    stockSprite_ = nullptr;
    device_ = nullptr;
    fonts_ = nullptr;
}
} // namespace labelcache

// MSVC x86 __cdecl normally exports a leading underscore. These aliases give
// MoonLoader stable, undecorated GetProcAddress names (no .def file required).
#if defined(_M_IX86)
#pragma comment(linker, "/EXPORT:LabelStress_SetMode=_LabelStress_SetMode")
#pragma comment(linker, "/EXPORT:LabelStress_GetStatus=_LabelStress_GetStatus")
#pragma comment(linker, "/EXPORT:LabelStress_GetStat=_LabelStress_GetStat")
#endif
extern "C" int __cdecl LabelStress_SetMode(int mode) {
    if ((mode < 0 || mode > 2) || !labelcache::currentStatus())
        return 0; // cannot benchmark a label cache whose hooks did not install
    // No device mutation, target changes, or resource release in this setter.
    InterlockedExchange(&labelcache::gMode, mode);
    return 1;
}
extern "C" int __cdecl LabelStress_GetStatus() {
    // 0 = unavailable; 1 = original; 2 = cached; 3 = diagnostic hidden text.
    return labelcache::currentStatus();
}

// Read-only, thread-safe counters from the last completed batch. Independent
// fields may straddle two batches if read concurrently with the render thread.
extern "C" int __cdecl LabelStress_GetStat(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= labelcache::gStats.size()) return -1;
    return static_cast<int>(labelcache::gStats[index].load(std::memory_order_relaxed));
}

void __fastcall CLabel__FinalFontDraw(void* fonts, void*, ID3DXSprite* sprite,
        const char* text, RECT rect, int flags, D3DCOLOR color, BOOL shadow) {
    labelcache::gLabels.drawText(fonts, sprite, text, rect, flags, color, shadow);
}
void __fastcall CLabel__PoolEnd(void* label) {
    labelcache::gLabels.beforeStockEnd();
    reinterpret_cast<void(__thiscall*)(void*)>(sampGetBase() + 0x675A0)(label);
    labelcache::gLabels.afterStockEnd();
}

#endif // ENABLE_LABEL_CACHE_EXPERIMENTAL
