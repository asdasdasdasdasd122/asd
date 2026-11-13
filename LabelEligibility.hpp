#pragma once
// R1 3D-label text eligibility, deliberately conservative.
// This is a byte-level CP1251 / ASCII validator, not a Unicode decoder.
// Whitelisted: one to eight lines (including blank lines) separated by LF, and complete
// {RRGGBB} tags matching the six-hex-digit syntax recognized by stock R1.
// All bytes, including tags/newlines, MUST be passed unchanged to BOTH the
// original font measurement and the original text renderer when baking.
// Unsupported markup/control characters stay on the original draw path.
#include <cstddef>

namespace labelcache {
inline bool hexDigit(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
           (c >= 'a' && c <= 'f');
}

inline bool eligibleCp1251Label(const char* text, std::size_t maxBytes = 512,
                                std::size_t maxLines = 8,
                                std::size_t maxVisiblePerLine = 80) {
    if (!text || !*text || maxBytes == 0 || maxLines == 0 ||
        maxVisiblePerLine == 0) return false;
    std::size_t i = 0, lines = 1, glyphs = 0;
    bool hasVisibleGlyph = false; // Blank lines are valid, but all-tags/empty labels are not.
    while (i <= maxBytes) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == 0) return i != 0 && hasVisibleGlyph;
        if (i == maxBytes) return false;
        if (c == '\n') {
            if (++lines > maxLines) return false;
            glyphs = 0;
            ++i;
            continue;
        }
        if (c == '{') {
            // Exact R1 markup: '{', six ASCII hex digits, '}'.
            // Test each byte in order; never scan past the first NUL.
            if (maxBytes - i < 8) return false;
            for (std::size_t j = 1; j <= 6; ++j) {
                const unsigned char h = static_cast<unsigned char>(text[i + j]);
                if (!h || !hexDigit(h)) return false;
            }
            if (text[i + 7] != '}') return false;
            i += 8;
            continue;
        }
        // Reject invalid closing braces, CR/CRLF, tabs, controls, unknown
        // single-byte codepage characters. Preserve raw CP1251 bytes.
        // CP1251: 0xA8/0xB8 = Ё/ё; 0xAB/0xBB = «/»;
        // 0xB9 = № (number sign), seen in server 3D labels.
        // Preserve original bytes for stock R1 measurement and texture baking.
        if (c == '}' || !((c >= 0x20 && c <= 0x7e) || c == 0xA8 ||
                          c == 0xB8 || c == 0xAB || c == 0xBB ||
                          c == 0xB9 || c >= 0xC0)) return false;
        if (++glyphs > maxVisiblePerLine) return false;
        hasVisibleGlyph = true;
        ++i;
    }
    return false;
}

// Retain the v6 API for code that explicitly needs single-line plain text.
inline bool plainCp1251SingleLine(const char* text, std::size_t limit = 80) {
    if (!eligibleCp1251Label(text, limit, 1, limit)) return false;
    for (const char* p = text; *p; ++p)
        if (*p == '{' || *p == '\n') return false;
    return true;
}
} // namespace labelcache
