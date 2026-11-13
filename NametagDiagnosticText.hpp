#pragma once
// Diagnostic ONLY: prefix is baked into cached nametag texture on a miss.
// It is NOT added to the player's server-provided name, cache key, or fallback.
#include <string>
#ifndef NAMETAG_CACHE_SHOW_MARKER
#define NAMETAG_CACHE_SHOW_MARKER 0
#endif
namespace nametagcache {
inline std::string diagnosticText(const char* playerText) {
#if NAMETAG_CACHE_SHOW_MARKER
    return std::string("[Optimized] ") + playerText;
#else
    return playerText;
#endif
}
} // namespace nametagcache
