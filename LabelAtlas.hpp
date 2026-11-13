#pragma once
#include "AtlasGrid.hpp"
#include "D3DTextCache.hpp"

namespace textgpu {
struct AtlasSlot {
    int page = -1;
    GridRect allocation;
    RECT source{};
    bool valid() const { return page >= 0; }
};

// Shared pages preserve draw order; there is no alpha-unsafe texture sorting.
// Two 1024x1024 ARGB pages keep the previous 8 MiB label pixel-data budget.
class LabelAtlas {
    struct Page { Texture gpu; AtlasGrid grid; };
    std::array<Page, 2> pages_{};
public:
    LabelAtlas() = default;
    LabelAtlas(const LabelAtlas&) = delete;
    LabelAtlas& operator=(const LabelAtlas&) = delete;
    ~LabelAtlas() { reset(); }
    void reset() {
        for (auto& page : pages_) { page.gpu.release(); page.grid.reset(); }
    }
    unsigned pageCount() const {
        unsigned count = 0;
        for (const auto& page : pages_) if (page.gpu.texture) ++count;
        return count;
    }
    std::size_t bytes() const { return pageCount() * 1024u * 1024u * 4u; }
    IDirect3DTexture9* texture(const AtlasSlot& slot) const {
        return slot.valid() && static_cast<std::size_t>(slot.page) < pages_.size()
            ? pages_[slot.page].gpu.texture : nullptr;
    }
    void release(AtlasSlot& slot) {
        if (slot.valid() && static_cast<std::size_t>(slot.page) < pages_.size())
            pages_[slot.page].grid.release(slot.allocation);
        slot = {};
    }
    template<class DrawText>
    samptext::BakeResult bake(Baker& baker, UINT width, UINT height,
                              AtlasSlot& output, DrawText drawText) {
        using samptext::BakeResult;
        if (output.valid()) return BakeResult::RetryLater;
        if (!width || !height || width > 1020 || height > 1020 ||
            !baker.supportsAtlas()) return BakeResult::Unsupported;
        for (std::size_t index = 0; index < pages_.size(); ++index) {
            auto& page = pages_[index];
            GridRect area;
            // 2-pixel transparent gutter on every side prevents sampling a neighbour.
            if (!page.grid.acquire(width + 4, height + 4, area)) continue;
            if (!page.gpu.texture) {
                const auto result = baker.createAtlasPage(page.gpu);
                if (result != BakeResult::Ready) { page.grid.release(area); return result; }
            }
            const RECT clear{static_cast<LONG>(area.x), static_cast<LONG>(area.y),
                static_cast<LONG>(area.x + area.width), static_cast<LONG>(area.y + area.height)};
            const RECT source{clear.left + 2, clear.top + 2,
                clear.left + 2 + static_cast<LONG>(width), clear.top + 2 + static_cast<LONG>(height)};
            const auto result = baker.bakeRegion(page.gpu, clear, source,
                [&](ID3DXSprite* sprite) { return drawText(sprite, source); });
            if (result != BakeResult::Ready) { page.grid.release(area); return result; }
            output = {static_cast<int>(index), area, source};
            return BakeResult::Ready;
        }
        return BakeResult::RetryLater; // bounded memory: ordinary draw remains visible
    }
};
} // namespace textgpu
