#pragma once
#include "CachePolicy.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3dx9.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>

namespace textgpu {
template<class T> class ComOwner {
    T* value_ = nullptr;
public:
    ComOwner() = default;
    ComOwner(const ComOwner&) = delete;
    ComOwner& operator=(const ComOwner&) = delete;
    ~ComOwner() { reset(); }
    T* get() const { return value_; }
    T* operator->() const { return value_; }
    T** put() { reset(); return &value_; }
    T* detach() { T* value = value_; value_ = nullptr; return value; }
    void reset() { if (value_) { value_->Release(); value_ = nullptr; } }
};

struct Texture {
    IDirect3DTexture9* texture = nullptr;
    UINT width = 0, height = 0;
    std::size_t bytes = 0;
    void release() {
        if (texture) texture->Release();
        *this = {};
    }
};

inline void report(const char* operation, HRESULT hr) {
    char message[160]{};
    std::snprintf(message, sizeof(message),
        "[3DTextOptimizer] %s failed: 0x%08lX; using original text.\n",
        operation, static_cast<unsigned long>(hr));
    OutputDebugStringA(message);
}

// The caller is INSIDE the game's device scene, AFTER its sprite batch.
// Switch only render targets. Never call device BeginScene/EndScene here.
// State blocks do not own the render-target/depth-stencil restoration for us.
class TargetScope {
    IDirect3DDevice9* device_;
    ComOwner<IDirect3DStateBlock9> state_;
    std::array<ComOwner<IDirect3DSurface9>, 4> targets_;
    ComOwner<IDirect3DSurface9> depth_;
    D3DVIEWPORT9 viewport_{};
    DWORD targetCount_ = 0;
    bool changed_ = false;
public:
    explicit TargetScope(IDirect3DDevice9* device) : device_(device) {}
    ~TargetScope() { restore(); }
    bool begin(IDirect3DSurface9* surface, UINT width, UINT height, DWORD targetCount) {
        targetCount_ = std::max<DWORD>(1, std::min<DWORD>(4, targetCount));
        if (FAILED(device_->CreateStateBlock(D3DSBT_ALL, state_.put())) || !state_.get() ||
            FAILED(device_->GetViewport(&viewport_))) return false;
        for (DWORD i = 0; i < targetCount_; ++i) {
            const HRESULT hr = device_->GetRenderTarget(i, targets_[i].put());
            if (FAILED(hr) && !(i && hr == D3DERR_NOTFOUND)) return false;
        }
        const HRESULT depthResult = device_->GetDepthStencilSurface(depth_.put());
        if (FAILED(depthResult) && depthResult != D3DERR_NOTFOUND) return false;
        changed_ = true; // every partial setup must be rolled back too
        if (FAILED(device_->SetDepthStencilSurface(nullptr))) return false;
        for (DWORD i = 1; i < targetCount_; ++i)
            if (FAILED(device_->SetRenderTarget(i, nullptr))) return false;
        const D3DVIEWPORT9 viewport{0, 0, width, height, 0.f, 1.f};
        return SUCCEEDED(device_->SetRenderTarget(0, surface)) &&
               SUCCEEDED(device_->SetViewport(&viewport));
    }
    bool restore() {
        if (!changed_) return true;
        changed_ = false;
        bool ok = SUCCEEDED(device_->SetDepthStencilSurface(nullptr));
        for (DWORD i = 0; i < targetCount_; ++i)
            if (FAILED(device_->SetRenderTarget(i, targets_[i].get()))) ok = false;
        if (FAILED(device_->SetDepthStencilSurface(depth_.get()))) ok = false;
        if (FAILED(state_->Apply())) ok = false;
        if (FAILED(device_->SetViewport(&viewport_))) ok = false;
        if (!ok) report("restore render target/state", D3DERR_INVALIDCALL);
        return ok;
    }
};

// One reusable sprite per renderer, never one sprite/depth surface per player.
// Both nametags and labels use the same QI policy; no private proxy offsets.
class Baker {
    ComOwner<IDirect3DDevice9> device_;
    ComOwner<ID3DXSprite> sprite_;
    D3DCAPS9 caps_{};
    static UINT powerOfTwo(UINT value) {
        UINT result = 1;
        while (result < value) result <<= 1;
        return result;
    }
    // Glyph RGB is straight alpha; the render target stores premultiplied RGB.
    // Preserve coverage in A with ONE, not SRCALPHA. Reapply immediately before
    // End submits queued glyphs: the stock font callback shares this device and
    // its final render state need not be the state it was given on entry.
    bool setBakeState(const RECT* clip = nullptr) {
        const struct { D3DRENDERSTATETYPE state; DWORD value; } states[] = {
            {D3DRS_ALPHABLENDENABLE, TRUE},
            {D3DRS_ZENABLE, FALSE}, {D3DRS_ZWRITEENABLE, FALSE},
            {D3DRS_SCISSORTESTENABLE, clip ? 1u : 0u},
            {D3DRS_SRCBLEND, D3DBLEND_SRCALPHA},
            {D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA},
            {D3DRS_BLENDOP, D3DBLENDOP_ADD},
            {D3DRS_SEPARATEALPHABLENDENABLE, TRUE},
            {D3DRS_SRCBLENDALPHA, D3DBLEND_ONE},
            {D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA},
            {D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD}
        };
        bool ok = true;
        for (const auto& state : states)
            if (FAILED(device_->SetRenderState(state.state, state.value))) ok = false;
        if (clip && FAILED(device_->SetScissorRect(clip))) ok = false;
        return ok;
    }
public:
    IDirect3DDevice9* device() const { return device_.get(); }
    bool supportsAtlas() const {
        return device_.get() && caps_.MaxTextureWidth >= 1024 && caps_.MaxTextureHeight >= 1024 &&
            (caps_.PrimitiveMiscCaps & D3DPMISCCAPS_SEPARATEALPHABLEND) &&
            (caps_.RasterCaps & D3DPRASTERCAPS_SCISSORTEST);
    }
    samptext::BakeResult createAtlasPage(Texture& output) {
        using samptext::BakeResult;
        if (!supportsAtlas()) return BakeResult::Unsupported;
        if (output.texture) return BakeResult::RetryLater;
        IDirect3DTexture9* texture = nullptr;
        const HRESULT hr = device_->CreateTexture(1024, 1024, 1, D3DUSAGE_RENDERTARGET,
            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &texture, nullptr);
        if (FAILED(hr) || !texture) {
            if (texture) texture->Release();
            report("CreateTexture (label atlas)", hr);
            return (hr == D3DERR_NOTAVAILABLE || hr == D3DERR_INVALIDCALL)
                ? BakeResult::Unsupported : BakeResult::RetryLater;
        }
        output = {texture, 1024, 1024, 1024u * 1024u * 4u};
        return BakeResult::Ready;
    }
    template<class DrawText>
    samptext::BakeResult bakeRegion(const Texture& page, const RECT& clear,
                                   const RECT& clip, DrawText drawText) {
        using samptext::BakeResult;
        if (!supportsAtlas() || !page.texture) return BakeResult::Unsupported;
        if (clear.left < 0 || clear.top < 0 || clear.right > static_cast<LONG>(page.width) ||
            clear.bottom > static_cast<LONG>(page.height) || clear.right <= clear.left ||
            clear.bottom <= clear.top || clip.left < clear.left || clip.top < clear.top ||
            clip.right > clear.right || clip.bottom > clear.bottom ||
            clip.right <= clip.left || clip.bottom <= clip.top) return BakeResult::Unsupported;
        if (!sprite_.get() && FAILED(D3DXCreateSprite(device_.get(), sprite_.put())))
            return BakeResult::RetryLater;
        ComOwner<IDirect3DSurface9> surface;
        if (FAILED(page.texture->GetSurfaceLevel(0, surface.put()))) return BakeResult::RetryLater;
        TargetScope target(device_.get());
        if (!target.begin(surface.get(), page.width, page.height, caps_.NumSimultaneousRTs))
            return BakeResult::RetryLater;
        // Clear ONLY this allocation. Never erase previously cached labels on the page.
        const D3DRECT rectangle{clear.left, clear.top, clear.right, clear.bottom};
        // Clear is affected by scissor too. Clear the gutter before enabling
        // the narrower text clip, regardless of inherited state/proxy behaviour.
        bool ok = SUCCEEDED(device_->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE)) &&
            SUCCEEDED(device_->Clear(1, &rectangle, D3DCLEAR_TARGET, 0, 1.f, 0));
        bool begun = false;
        if (ok) { begun = SUCCEEDED(sprite_->Begin(D3DXSPRITE_ALPHABLEND)); ok = begun; }
        if (ok) {
            // Stock fonts use DT_NOCLIP; enforce the region on the device itself.
            ok = setBakeState(&clip);
            if (ok) ok = drawText(sprite_.get());
        }
        if (begun) {
            if (!setBakeState(&clip)) ok = false;
            if (FAILED(sprite_->End())) ok = false;
        }
        if (!target.restore()) ok = false;
        return ok ? BakeResult::Ready : BakeResult::RetryLater;
    }
    void reset() { sprite_.reset(); device_.reset(); caps_ = {}; }
    bool ensure(IDirect3DDevice9* borrowed) {
        if (device_.get()) return true;
        if (!borrowed) return false;
        HRESULT hr = borrowed->QueryInterface(IID_IDirect3DDevice9,
            reinterpret_cast<void**>(device_.put()));
        if (FAILED(hr) || !device_.get()) { report("device QueryInterface", hr); reset(); return false; }
        hr = device_->GetDeviceCaps(&caps_);
        if (FAILED(hr)) { report("GetDeviceCaps", hr); reset(); return false; }
        return true;
    }
    template<class DrawText>
    samptext::BakeResult bake(UINT width, UINT height, std::size_t availableBytes,
                              Texture& output, DrawText drawText) {
        using samptext::BakeResult;
        if (!device_.get()) return BakeResult::RetryLater;
        if (!(caps_.PrimitiveMiscCaps & D3DPMISCCAPS_SEPARATEALPHABLEND))
            return BakeResult::Unsupported;
        if (!width || !height) return BakeResult::Unsupported;
        if (caps_.TextureCaps & D3DPTEXTURECAPS_POW2) {
            width = powerOfTwo(width); height = powerOfTwo(height);
        }
        if (caps_.TextureCaps & D3DPTEXTURECAPS_SQUAREONLY)
            width = height = std::max(width, height);
        if (width > caps_.MaxTextureWidth || height > caps_.MaxTextureHeight)
            return BakeResult::Unsupported;
        const std::size_t bytes = static_cast<std::size_t>(width) * height * 4u;
        if (bytes > availableBytes) return BakeResult::RetryLater;
        if (!sprite_.get()) {
            const HRESULT hr = D3DXCreateSprite(device_.get(), sprite_.put());
            if (FAILED(hr)) { report("D3DXCreateSprite", hr); return BakeResult::RetryLater; }
        }
        ComOwner<IDirect3DTexture9> texture;
        ComOwner<IDirect3DSurface9> surface;
        HRESULT hr = device_->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET,
            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, texture.put(), nullptr);
        if (FAILED(hr)) {
            report("CreateTexture", hr);
            return (hr == D3DERR_NOTAVAILABLE || hr == D3DERR_INVALIDCALL) ?
                BakeResult::Unsupported : BakeResult::RetryLater;
        }
        if (FAILED(texture->GetSurfaceLevel(0, surface.put()))) return BakeResult::RetryLater;
        TargetScope target(device_.get());
        if (!target.begin(surface.get(), width, height, caps_.NumSimultaneousRTs))
            return BakeResult::RetryLater;
        // Clear must not depend on scissor state left by the game or a proxy.
        bool ok = SUCCEEDED(device_->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE)) &&
            SUCCEEDED(device_->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.f, 0));
        bool spriteBegun = false;
        if (ok) {
            hr = sprite_->Begin(D3DXSPRITE_ALPHABLEND);
            spriteBegun = SUCCEEDED(hr);
            ok = spriteBegun;
        }
        if (ok) {
            ok = setBakeState();
            if (ok) ok = drawText(sprite_.get());
        }
        if (spriteBegun) {
            if (!setBakeState()) ok = false;
            if (FAILED(sprite_->End())) ok = false;
        }
        if (!target.restore()) ok = false;
        if (!ok) return BakeResult::RetryLater;
        output = {texture.detach(), width, height, bytes};
        return BakeResult::Ready;
    }
};

// Stock font glyphs use straight alpha; our textures use premultiplied alpha.
// Flush BEFORE each transition so already queued glyphs keep their own blend.
// Consecutive cached texts stay in a single batch, without a flush per label.
// A shared device may be changed between hook calls. Bind our blend before
// every Draw (which may submit internally) AND before the final Flush.
class CachedBatch {
    IDirect3DDevice9* device_ = nullptr;
    ID3DXSprite* sprite_ = nullptr;
    std::array<DWORD, 5> saved_{};
    inline static constexpr D3DRENDERSTATETYPE states_[5] = {
        D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP,
        D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_ALPHABLENDENABLE
    };
    static bool setBlend(IDirect3DDevice9* device) {
        const DWORD values[5] = {D3DBLEND_ONE, D3DBLEND_INVSRCALPHA,
                                D3DBLENDOP_ADD, FALSE, TRUE};
        bool ok = true;
        for (std::size_t i = 0; i < 5; ++i)
            if (FAILED(device->SetRenderState(states_[i], values[i]))) ok = false;
        return ok;
    }
public:
    void discard() { device_ = nullptr; sprite_ = nullptr; }
    bool finish() {
        if (!sprite_) return true;
        bool ok = setBlend(device_);
        if (FAILED(sprite_->Flush())) ok = false;
        for (std::size_t i = 0; i < saved_.size(); ++i)
            if (FAILED(device_->SetRenderState(states_[i], saved_[i]))) ok = false;
        discard();
        return ok;
    }
    bool draw(IDirect3DDevice9* device, ID3DXSprite* sprite,
              IDirect3DTexture9* texture, const D3DXVECTOR3& position,
              const RECT* source = nullptr) {
        if (!device || !sprite || !texture) return false;
        if (sprite_ && (sprite_ != sprite || device_ != device) && !finish()) return false;
        if (!sprite_) {
            if (FAILED(sprite->Flush())) return false;
            for (std::size_t i = 0; i < saved_.size(); ++i)
                if (FAILED(device->GetRenderState(states_[i], &saved_[i]))) return false;
            if (!setBlend(device)) {
                for (std::size_t i = 0; i < saved_.size(); ++i)
                    device->SetRenderState(states_[i], saved_[i]);
                return false;
            }
            device_ = device; sprite_ = sprite;
        } else if (!setBlend(device)) {
            // Submit earlier cached sprites with their blend, then restore
            // stock state before the caller falls back to the original font.
            finish();
            return false;
        }
        return SUCCEEDED(sprite->Draw(texture, source, nullptr, &position, 0xFFFFFFFF));
    }
};
} // namespace textgpu
