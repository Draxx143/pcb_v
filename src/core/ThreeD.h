// ThreeD.h — a small software renderer for the 3D board preview.
//
// The preview is not meant to be a photorealistic model, but it does have to read as a
// real board: a solder-mask coloured slab, copper under the mask, bare metal pads with
// holes, white silkscreen and extruded component bodies, all shaded by a single light and
// drawn with a depth buffer so nothing bleeds through anything else.
//
// Rendering into a plain pixel buffer keeps it testable (the CLI writes a BMP, the smoke
// test inspects the pixels) and keeps Qt out of the geometry code.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "BoardRender.h"
#include "Export.h"
#include "Model.h"

namespace pcbv {

/// Camera + look of the 3D preview.
struct ThreeDView {
    double yawDeg = 32.0;         ///< rotation around the board normal
    double elevationDeg = 38.0;   ///< 90 = straight down on the board, 0 = edge on
    double zoom = 1.0;            ///< 1 = fit the view
    int width = 1200;             ///< output size in pixels
    int height = 800;
    int supersample = 2;          ///< internal anti-aliasing factor (1..3)
    std::uint32_t maskColor = 0;   ///< solder mask override (0 = take it from the theme)
    bool shadows = true;          ///< soft contact shadows under the parts
    bool labels = true;           ///< report where the designators would be drawn
};

/// A text label the view should draw on top of the image (designators, silkscreen).
struct ThreeDLabel {
    double x = 0.0;        ///< image pixels
    double y = 0.0;
    double size = 12.0;    ///< pixel height
    std::string text;
    std::uint32_t color = 0xf2f5f8;
};

/// One rendered frame: 0xRRGGBB pixels, row major, top row first.
struct ThreeDImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> pixels;
    std::vector<ThreeDLabel> labels;

    bool valid() const { return width > 0 && height > 0 && pixels.size() == static_cast<std::size_t>(width) * height; }
    std::uint32_t at(int x, int y) const {
        if (x < 0 || y < 0 || x >= width || y >= height) return 0;
        return pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
    }
};

/// Renders the board of `doc`. Layers hidden in `options` are left out of the 3D view too.
ThreeDImage render3D(const Document& doc, const BoardRenderOptions& options, const ThreeDView& view);

/// 24-bit BMP (bottom-up rows, no compression) — the CLI export and the smoke test use it.
std::string threeDToBmp(const ThreeDImage& image);

} // namespace pcbv
