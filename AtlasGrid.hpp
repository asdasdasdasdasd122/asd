#pragma once
#include <array>
#include <cstdint>

namespace textgpu {
// 1024x1024 page, 32x16 cells. Allocation happens only after the stock End.
// Row masks keep the allocator bounded; released regions are reusable.
struct GridRect { unsigned x = 0, y = 0, width = 0, height = 0; };
class AtlasGrid {
    std::array<std::uint64_t, 64> rows_{};
public:
    static constexpr unsigned PAGE_SIZE = 1024;
    void reset() { rows_.fill(0); }
    bool acquire(unsigned width, unsigned height, GridRect& out) {
        if (!width || !height || width > PAGE_SIZE || height > PAGE_SIZE) return false;
        const unsigned columns = (width + 31) / 32, rows = (height + 15) / 16;
        const std::uint64_t base = (std::uint64_t{1} << columns) - 1;
        for (unsigned y = 0; y + rows <= rows_.size(); ++y) {
            std::uint64_t blocked = 0;
            for (unsigned dy = 0; dy < rows; ++dy) blocked |= rows_[y + dy];
            for (unsigned x = 0; x + columns <= 32; ++x) {
                const auto mask = base << x;
                if (blocked & mask) continue;
                for (unsigned dy = 0; dy < rows; ++dy) rows_[y + dy] |= mask;
                out = {x * 32, y * 16, columns * 32, rows * 16};
                return true;
            }
        }
        return false;
    }
    void release(const GridRect& r) {
        if (!r.width || !r.height || r.x % 32 || r.y % 16 ||
            r.width % 32 || r.height % 16 || r.x > PAGE_SIZE || r.y > PAGE_SIZE ||
            r.width > PAGE_SIZE - r.x || r.height > PAGE_SIZE - r.y) return;
        const auto mask = ((std::uint64_t{1} << (r.width / 32)) - 1) << (r.x / 32);
        for (unsigned y = r.y / 16; y < (r.y + r.height) / 16; ++y) rows_[y] &= ~mask;
    }
};
} // namespace textgpu
