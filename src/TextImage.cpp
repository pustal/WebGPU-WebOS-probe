/**
 * @file TextImage.cpp
 * @brief Implementation of TextImage (see TextImage.h).
 */
#include "TextImage.h"

#include "FontData.h"

#include <algorithm>

namespace probe
{
namespace
{

constexpr int kScale       = 2;                      ///< font pixels -> image pixels
constexpr int kLineHeight  = (kGlyphSize + 3) * kScale;
constexpr int kMargin      = 24;
constexpr int kLabelColumn = 30;                     ///< characters reserved for "TAG label"

} // namespace

TextImage::TextImage(int width, int height)
    : width_(width), height_(height), pixels_(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 0)
{
}

void TextImage::Fill(int x, int y, int w, int h, Colour colour)
{
    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = std::min(width_, x + w);
    const int y1 = std::min(height_, y + h);
    for (int row = y0; row < y1; ++row) {
        std::uint8_t *pixel = &pixels_[(static_cast<std::size_t>(row) * static_cast<std::size_t>(width_) +
                                        static_cast<std::size_t>(x0)) * 4];
        for (int col = x0; col < x1; ++col, pixel += 4) {
            pixel[0] = colour.r;
            pixel[1] = colour.g;
            pixel[2] = colour.b;
            pixel[3] = colour.a;
        }
    }
}

void TextImage::DrawText(int x, int y, const std::string &text, Colour colour, int scale)
{
    const int advance = kGlyphSize * scale;
    for (char c : text) {
        if (x + advance > width_ - kMargin)
            break;
        int index = static_cast<unsigned char>(c) - static_cast<unsigned char>(kFirstGlyph);
        if (index < 0 || index >= kGlyphCount)
            index = '?' - kFirstGlyph;
        const auto &glyph = kGlyphs[static_cast<std::size_t>(index)];
        for (int row = 0; row < kGlyphSize; ++row) {
            for (int col = 0; col < kGlyphSize; ++col) {
                // Bit 0 is the leftmost pixel of a row (see FontData.h).
                if (glyph[static_cast<std::size_t>(row)] & (1u << col))
                    Fill(x + col * scale, y + row * scale, scale, scale, colour);
            }
        }
        x += advance;
    }
}

void TextImage::Render(const std::string &title, const Report &report, const std::string &footer)
{
    constexpr Colour kPanel   = { 8, 12, 32, 235 };
    constexpr Colour kTitle   = { 255, 209, 61, 255 };
    constexpr Colour kText    = { 235, 235, 245, 255 };
    constexpr Colour kSection = { 89, 171, 255, 255 };
    constexpr Colour kPass    = { 89, 204, 120, 255 };
    constexpr Colour kWarn    = { 255, 191, 64, 255 };
    constexpr Colour kFail    = { 255, 107, 89, 255 };
    constexpr Colour kDim     = { 153, 161, 191, 255 };

    std::fill(pixels_.begin(), pixels_.end(), std::uint8_t(0));
    Fill(0, 0, width_, height_, kPanel);

    int y = kMargin;
    DrawText(kMargin, y, title, kTitle, kScale + 1);
    y += kLineHeight * 2;

    for (const Finding &finding : report.Findings()) {
        if (y > height_ - kMargin - kLineHeight * 2)
            break;
        if (finding.label.empty()) {
            y += kLineHeight / 2;
            DrawText(kMargin, y, finding.value, kSection, kScale);
            y += kLineHeight;
            continue;
        }
        Colour tagColour = kDim;
        switch (finding.status) {
            case Status::Pass: tagColour = kPass; break;
            case Status::Warn: tagColour = kWarn; break;
            case Status::Fail: tagColour = kFail; break;
            case Status::Info: break;
        }
        DrawText(kMargin, y, StatusTag(finding.status), tagColour, kScale);
        DrawText(kMargin + 6 * kGlyphSize * kScale, y, finding.label, kText, kScale);
        DrawText(kMargin + kLabelColumn * kGlyphSize * kScale, y, finding.value, kText, kScale);
        y += kLineHeight;
    }

    DrawText(kMargin, height_ - kMargin - kLineHeight, footer, kDim, kScale);
}

} // namespace probe
