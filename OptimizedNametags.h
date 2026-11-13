#pragma once
#pragma comment(lib, "d3dx9.lib")
#include "D3DTextCache.hpp"

struct CVector { float x, y, z; };

class OptimizedNametags {
public:
    static constexpr int MAX_PLAYERS = 1004;
    static constexpr int MAX_PLAYER_NAME = 24;
    static constexpr DWORD UNUSED_CACHE_MS = 15000;
    static constexpr std::size_t LIMIT_BYTES = 32u * 1024u * 1024u;

    struct NameTag {
        textgpu::Texture gpu;
        ID3DXFont* lastAuxFont = nullptr; // detect AFK-font availability change
        DWORD lastUsed = 0;
        D3DCOLOR color = 0;
        float center = 0.f;
        bool redraw = true, isAfk = false;
        char name[MAX_PLAYER_NAME + 7 + 1]{};
        // A missing/changed name is drawn by the ORIGINAL renderer immediately.
        // Only the text/color are queued: never store transient game string pointers.
        bool pending = false;
        char pendingName[MAX_PLAYER_NAME + 7 + 1]{};
        D3DCOLOR pendingColor = 0;
        bool pendingAfk = false;
        ID3DXFont* pendingAuxFont = nullptr; // compared only; never dereferenced
        samptext::RetryState retry;
    };

    NameTag mNametags[MAX_PLAYERS];
    struct {
        rtdhook* CPlayerTags__OnLostDevice = nullptr;
        rtdhook_call* CPlayerTags__Draw = nullptr;
        rtdhook_call* CPlayerTags__End = nullptr;
    } mHooks;
    IDirect3DDevice9* mD3DDevice = nullptr; // Borrowed game's device, possibly a proxy.
    void* mFonts = nullptr;
    textgpu::Baker mBaker;
    textgpu::CachedBatch mCachedBatch;
    samptext::FairCursor mBakeCursor;
    std::size_t mUsedBytes = 0;
    DWORD mLastSweep = 0;

    bool shouldRedrawNametag(const NameTag& nt, D3DCOLOR color, bool isAfk,
                             ID3DXFont* auxFont, const char* name) const;
    bool syncDevice(); // detects replacement; clears resources/queued work first
    void releaseElements(NameTag& nt);
    void sweepUnused(DWORD now);
    void onLostDevice();
    void preparePendingAfterEnd();
    samptext::BakeResult bake(NameTag& nt);
    void queue(NameTag& nt, const char* text, D3DCOLOR color, bool afk, ID3DXFont* auxFont);
};

#define DX_SAFE_RELEASE(p) do { if ((p) != nullptr) { (p)->Release(); (p) = nullptr; } } while (0)

extern OptimizedNametags gInstance;

void __fastcall CPlayerTags__Draw(uintptr_t self, int id, CVector* playerPos,
    const char* text, D3DCOLOR color, float distance, bool drawStatus, int status);
void CPlayerTags__Draw_Naked();
void __fastcall CPlayerTags__End(void* self);
void __fastcall CPlayerTags__OnLostDevice(void* self);
