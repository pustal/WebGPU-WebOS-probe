/**
 * @file FontData.h
 * @brief Embedded 8x8 bitmap font (printable ASCII), so the probe can show
 *        text without any font files or libraries.
 */
#ifndef WGPUPROBE_FONT_DATA_H
#define WGPUPROBE_FONT_DATA_H

#include <array>
#include <cstdint>

namespace probe
{

constexpr char kFirstGlyph = ' ';
constexpr int kGlyphCount  = 95; // ' ' (0x20) .. '~' (0x7E)
constexpr int kGlyphSize   = 8;  // pixels, square

extern const std::array<std::array<std::uint8_t, kGlyphSize>, kGlyphCount> kGlyphs;

} // namespace probe

#endif // WGPUPROBE_FONT_DATA_H
