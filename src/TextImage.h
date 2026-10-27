/**
 * @file TextImage.h
 * @brief Draws the report into an RGBA8 image on the CPU, so it can be shown by
 *        WebGPU (as a texture) or, if WebGPU never came up, by SDL's renderer.
 */
#ifndef WGPUPROBE_TEXT_IMAGE_H
#define WGPUPROBE_TEXT_IMAGE_H

#include "Report.h"

#include <cstdint>
#include <string>
#include <vector>

namespace probe
{

class TextImage
{
public:
    /** Creates a transparent image of @p width x @p height pixels. */
    TextImage(int width, int height);

    /**
     * Redraws the image: a translucent panel with @p title, the report and a
     * one-line @p footer (for example the key help or "Running...").
     */
    void Render(const std::string &title, const Report &report, const std::string &footer);

    int Width() const { return width_; }
    int Height() const { return height_; }
    /** Bytes per row (RGBA8, tightly packed). */
    int Pitch() const { return width_ * 4; }
    const std::vector<std::uint8_t> &Pixels() const { return pixels_; }

private:
    struct Colour {
        std::uint8_t r, g, b, a;
    };

    void Fill(int x, int y, int w, int h, Colour colour);
    /** Draws @p text at (@p x, @p y) with each font pixel @p scale pixels wide; clips at the right edge. */
    void DrawText(int x, int y, const std::string &text, Colour colour, int scale);

    int width_;
    int height_;
    std::vector<std::uint8_t> pixels_;
};

} // namespace probe

#endif // WGPUPROBE_TEXT_IMAGE_H
