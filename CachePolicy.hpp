#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace samptext {
enum class BakeResult { Ready, RetryLater, Unsupported };

// Unsigned subtraction also works across GetTickCount's 49-day wrap.
struct RetryState {
    std::uint32_t lastAttempt = 0;
    std::uint32_t delayMs = 0;
    bool unsupported = false;
    bool allowed(std::uint32_t now) const {
        return !unsupported && (!delayMs ||
            static_cast<std::uint32_t>(now - lastAttempt) >= delayMs);
    }
    void reset() { *this = {}; }
    void failed(BakeResult result, std::uint32_t now) {
        unsupported = result == BakeResult::Unsupported;
        lastAttempt = now;
        delayMs = !delayMs ? 250u : (delayMs < 2500u ? delayMs * 2u : 5000u);
    }
};

// A bounded scan with a persistent cursor; hot/failed low IDs cannot starve
// higher IDs. Only work actually attempted consumes the per-frame budget.
class FairCursor {
    std::size_t next_ = 0;
public:
    void reset() { next_ = 0; }
    template<class Eligible, class Work>
    std::size_t run(std::size_t count, std::size_t budget, Eligible eligible, Work work) {
        std::size_t attempts = 0;
        for (std::size_t scanned = 0; scanned < count && attempts < budget; ++scanned) {
            const auto id = next_ % count;
            next_ = (id + 1) % count;
            if (eligible(id)) { work(id); ++attempts; }
        }
        return attempts;
    }
};

template<std::size_t Capacity>
class SlotIds {
    static_assert(Capacity > 0 && Capacity <= 65536, "16-bit slot capacity");
    std::array<std::uint16_t, Capacity> free_{};
    std::array<bool, Capacity> used_{};
    std::size_t available_ = 0;
public:
    SlotIds() { reset(); }
    void reset() {
        used_.fill(false);
        for (std::size_t i = 0; i < Capacity; ++i)
            free_[i] = static_cast<std::uint16_t>(Capacity - 1 - i);
        available_ = Capacity;
    }
    bool acquire(std::uint16_t& id) {
        if (!available_) return false;
        id = free_[--available_];
        used_[id] = true;
        return true;
    }
    void release(std::uint16_t id) {
        if (id >= Capacity || !used_[id]) return;
        used_[id] = false;
        free_[available_++] = id;
    }
};
} // namespace samptext
