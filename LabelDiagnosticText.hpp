#pragma once
// v8 VISUAL DIAGNOSTIC only. This label is part of an already baked texture,
// NOT a server data mutation and NOT an additional per-frame sprite draw.
#include <string>

#ifndef LABEL_CACHE_SHOW_MARKER
#define LABEL_CACHE_SHOW_MARKER 1
#endif

namespace labelcache {
inline std::string diagnosticText(const std::string& serverText) {
#if LABEL_CACHE_SHOW_MARKER
    return std::string("[Optimized] ") + serverText;
#else
    return serverText;
#endif
}
} // namespace labelcache
