#pragma once
// Portable integration of the decision cache with a renderer abstraction.
// No game addresses or hooks here. An eventual R1 adapter must implement
// IRenderer with VERIFIED text-only rasterisation and respect batch boundaries.
#include "TextCacheCore.hpp"
#include "CachePolicy.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace samptext {
struct Position {
    // Screen position/scale MUST come from the game's normal projection on
    // every draw. No camera position belongs in the text texture cache key.
    float x = 0, y = 0, scale = 1;
    std::uint32_t tint = 0xFFFFFFFFu;
};

// Conservative: TextDraws with previews, sprite fonts, clickable selections,
// or unverified box/outline behavior stay in the original renderer.
enum class Eligibility : std::uint8_t { TextOnlyVerified, OriginalOnly };

struct Request {
    Id id;
    std::string text;     // untouched bytes; preserve inline color tags
    Style style;          // ALL raster-affecting flags must be populated
    Position position;    // updated every frame, NOT part of cache key
    Eligibility eligibility = Eligibility::OriginalOnly;
};

// An implementation must never call the original 3D projection routine while
// baking into an offscreen texture: projection and glyph rasterization are
// separate operations. Classify temporary failures and unsupported content.
struct IRenderer {
    virtual ~IRenderer() = default;
    virtual BakeResult bake(Id id, const Entry& contents) = 0;
    virtual bool draw(Id id, const Entry& contents, Position screen) = 0;
    virtual void release(Id id) = 0;
};

class Pipeline {
public:
    enum class Result { Cached, Original };
    using Original = std::function<void()>;

    explicit Pipeline(IRenderer& renderer, std::size_t maxEntries = 2048,
                      std::size_t maxBytes = 4096)
        : renderer_(renderer), cache_(maxEntries, maxBytes) {}
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    // Called INSIDE the normal game draw loop. NEVER bake here: offscreen
    // render-target switching mid-sprite-batch hid names in the earlier ASI.
    Result drawOrOriginal(const Request& req, const Original& original,
                          std::uint32_t now) {
        if (req.eligibility != Eligibility::TextOnlyVerified) {
            erase(req.id);  // do not retain an obsolete resource for a reused ID
            original();
            return Result::Original;
        }

        const Entry* before = cache_.lookup(req.id);
        if (before && (before->text != req.text || !(before->style == req.style)))
            releaseResource(req.id);

        const auto decision = cache_.observe(req.id, req.text, req.style);
        if (decision == Cache::Decision::TooLong ||
            decision == Cache::Decision::CapacityExceeded) {
            if (decision == Cache::Decision::TooLong) erase(req.id);
            original();
            return Result::Original;
        }

        const Entry* entry = cache_.lookup(req.id);
        if (decision == Cache::Decision::UseCached && entry &&
            resources_.count(req.id) && renderer_.draw(req.id, *entry, req.position))
            return Result::Cached;

        if (decision == Cache::Decision::UseCached) {
            releaseResource(req.id);  // stale graphics handle or failed Draw
            cache_.invalidate(req.id);
            entry = cache_.lookup(req.id);
        }
        // Render the name / label NOW using stock code, then bake AFTER End().
        original();
        if (entry) {
            auto failed = failures_.find(req.id);
            if (failed != failures_.end() && failed->second.generation != entry->generation) {
                failures_.erase(failed);
                failed = failures_.end();
            }
            if (failed == failures_.end() || failed->second.retry.allowed(now))
                schedule(req.id, entry->generation);
        }
        return Result::Original;
    }

    // Call AFTER the stock CLabel::End / text draw batch, never inside Draw().
    // Limit work to avoid a first-frame spike at busy locations.
    std::size_t afterBatch(std::size_t maxBakes, std::uint32_t now) {
        std::size_t attempts = 0;
        while (attempts < maxBakes && !queue_.empty()) {
            const Job job = queue_.front();
            queue_.pop_front();
            auto pending = pending_.find(job.id);
            if (pending == pending_.end() || pending->second != job.generation)
                continue; // old text / old slot / reset: never bake obsolete data
            pending_.erase(pending);
            const Entry* entry = cache_.lookup(job.id);
            if (!entry || entry->generation != job.generation || entry->ready)
                continue;
            ++attempts;
            const auto result = renderer_.bake(job.id, *entry);
            if (result == BakeResult::Ready) {
                failures_.erase(job.id);
                if (cache_.markReady(job.id, job.generation)) {
                    resources_.insert(job.id);
                } else {
                    renderer_.release(job.id);
                }
            } else {
                renderer_.release(job.id);
                auto& failure = failures_[job.id];
                if (failure.generation != job.generation) failure.retry.reset();
                failure.generation = job.generation;
                failure.retry.failed(result, now);
            }
        }
        return attempts;
    }

    // Must be called when the server deletes a 3D label or TextDraw ID.
    void erase(Id id) {
        releaseResource(id);
        cache_.erase(id);
        pending_.erase(id);
        failures_.erase(id);
        queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
            [id](const Job& job) { return job.id == id; }), queue_.end());
    }

    // Device loss: release all textures, discard queued jobs, force re-bake.
    // Call only at a safe Direct3D lifecycle point, not from an arbitrary thread.
    void deviceLost() {
        for (const Id id : resources_) renderer_.release(id);
        resources_.clear();
        queue_.clear();
        pending_.clear();
        failures_.clear();
        cache_.resetDevice();
    }

    void clear() {
        deviceLost();
        cache_.clear();
    }

    std::size_t cachedEntries() const { return cache_.size(); }
    std::size_t queuedJobs() const { return pending_.size(); }
    ~Pipeline() { deviceLost(); }

private:
    struct Job { Id id; std::uint64_t generation; };
    struct Failure { std::uint64_t generation = 0; RetryState retry; };
    void releaseResource(Id id) {
        if (resources_.erase(id)) renderer_.release(id);
    }
    void schedule(Id id, std::uint64_t generation) {
        auto old = pending_.find(id);
        if (old != pending_.end() && old->second == generation) return;
        // Replace rather than accumulate stale jobs when a dynamic textdraw
        // changes several times before the batch ends (bounded queue size).
        if (old != pending_.end())
            queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                [id](const Job& job) { return job.id == id; }), queue_.end());
        pending_[id] = generation;
        queue_.push_back({id, generation});
    }
    IRenderer& renderer_;
    Cache cache_;
    std::unordered_set<Id, IdHash> resources_;
    std::unordered_map<Id, std::uint64_t, IdHash> pending_;
    std::deque<Job> queue_;
    std::unordered_map<Id, Failure, IdHash> failures_;
};
} // namespace samptext
