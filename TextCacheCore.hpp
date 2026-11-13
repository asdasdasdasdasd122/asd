#pragma once
// Portable *decision layer* for future SA-MP R1 3D-label/TextDraw renderer.
// Deliberately no game hooks, D3D objects, player-slot writes or network access.
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>

namespace samptext {
enum class Kind : std::uint8_t { Label3D, TextDraw };
struct Id {
    Kind kind;
    std::uint16_t slot;
    bool operator==(const Id& o) const noexcept { return kind == o.kind && slot == o.slot; }
};
struct IdHash {
    std::size_t operator()(Id i) const noexcept {
        return (static_cast<std::size_t>(i.slot) << 1) | static_cast<std::size_t>(i.kind);
    }
};
// Include only attributes that affect the resulting texture. Position, distance,
// camera angle, world position, attachment coordinates must NOT invalidate it.
// Encode SA-MP inline color markup in `text` unchanged; do not strip or normalize.
struct Style {
    std::uint32_t color = 0xFFFFFFFFu;
    std::uint32_t background = 0;
    std::uint32_t fontRevision = 0; // bump after changing/recreating font resources
    std::uint32_t widthBits = 0;    // exact float bit pattern for wrap/box width
    std::uint32_t heightBits = 0;
    std::uint32_t flags = 0;        // outline, shadow, alignment, box etc.
    bool operator==(const Style& o) const noexcept {
        return color == o.color && background == o.background &&
            fontRevision == o.fontRevision && widthBits == o.widthBits &&
            heightBits == o.heightBits && flags == o.flags;
    }
};
struct Entry {
    std::string text;
    Style style;
    std::uint64_t generation = 0;
    bool ready = false; // integration sets true ONLY after successful GPU bake
};
class Cache {
public:
    enum class Decision { NeedsBake, UseCached, TooLong, CapacityExceeded };
    // Actual pool limits require verification for each R1 subsystem.
    explicit Cache(std::size_t capacity = 2048, std::size_t maxTextBytes = 4096)
        : capacity_(capacity), maxTextBytes_(maxTextBytes) {}
    Decision observe(Id id, const std::string& text, Style style) {
        if (text.size() > maxTextBytes_) return Decision::TooLong;
        auto it = entries_.find(id);
        if (it == entries_.end()) {
            if (entries_.size() >= capacity_) return Decision::CapacityExceeded;
            Entry fresh;
            fresh.text = text;
            fresh.style = style;
            fresh.generation = epoch_++;
            entries_.emplace(id, std::move(fresh));
            return Decision::NeedsBake;
        }
        auto& old = it->second;
        if (old.text != text || !(old.style == style)) {
            old.text = text;
            old.style = style;
            old.ready = false;
            old.generation = epoch_++;
        }
        return old.ready ? Decision::UseCached : Decision::NeedsBake;
    }
    // Prevent completion of an outdated queued bake from marking a newer value ready.
    bool markReady(Id id, std::uint64_t generation) {
        auto it = entries_.find(id);
        if (it == entries_.end() || it->second.generation != generation) return false;
        it->second.ready = true;
        return true;
    }
    const Entry* lookup(Id id) const {
        auto it = entries_.find(id);
        return it == entries_.end() ? nullptr : &it->second;
    }
    // Drawing an existing texture can fail after a device transition. Do not
    // keep reporting it ready; the renderer adapter releases its resource.
    bool invalidate(Id id) {
        auto it = entries_.find(id);
        if (it == entries_.end()) return false;
        it->second.ready = false;
        it->second.generation = epoch_++;
        return true;
    }
    // On SA-MP delete events release matching GPU resource BEFORE erase.
    bool erase(Id id) { return entries_.erase(id) != 0; }
    void clear() { entries_.clear(); } // keep epoch monotonic across ID reuse
    // After device loss, integration must release GPU handles and call resetDevice().
    void resetDevice() {
        for (auto& item : entries_) {
            item.second.ready = false;
            item.second.generation = epoch_++;
        }
    }
    std::size_t size() const noexcept { return entries_.size(); }
private:
    std::unordered_map<Id, Entry, IdHash> entries_;
    std::size_t capacity_;
    std::size_t maxTextBytes_;
    std::uint64_t epoch_ = 1;
};
} // namespace samptext
