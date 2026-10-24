// LayerStyle.h — how PCB layers are grouped and coloured (shared by SVG and Qt output).
//
// Altium layer ids: 1 = Top Layer, 2..31 = Mid Layer 1..30, 32 = Bottom Layer,
// 33/34 = Top/Bottom Overlay, 35/36 = Top/Bottom Paste, 37/38 = Top/Bottom Solder,
// 39..54 = internal planes, 55 = Drill Guide, 56 = Keep-Out, 57..72 = Mechanical 1..16,
// 73 = Drill Drawing, 74 = Multi-Layer.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace pcbv {

enum class LayerGroup {
    TopCopper,
    MidCopper,
    BottomCopper,
    TopSilk,
    BottomSilk,
    Paste,
    SolderMask,
    KeepOut,
    Mechanical,
    MultiLayer,
    Other
};

inline LayerGroup layerGroup(int layer) {
    if (layer == 1) return LayerGroup::TopCopper;
    if (layer == 32) return LayerGroup::BottomCopper;
    if (layer >= 2 && layer <= 31) return LayerGroup::MidCopper;
    if (layer == 33) return LayerGroup::TopSilk;
    if (layer == 34) return LayerGroup::BottomSilk;
    if (layer == 35 || layer == 36) return LayerGroup::Paste;
    if (layer == 37 || layer == 38) return LayerGroup::SolderMask;
    if (layer == 56) return LayerGroup::KeepOut;
    if (layer == 74) return LayerGroup::MultiLayer;
    if (layer >= 55 && layer <= 73) return LayerGroup::Mechanical;
    return LayerGroup::Other;
}

/// The colour language of the board drawing. Both the 2D view (SVG and Qt) and the 3D
/// preview take their colours from here, so a board looks the same everywhere.
enum class BoardTheme {
    EasyEda,   ///< dark canvas, blue copper, gold pads, magenta edge (web CAD look)
    Classic    ///< the warm palette the viewer shipped with
};

/// Every colour the drawing needs, as 0xRRGGBB.
struct ThemeColours {
    // 2D
    std::uint32_t canvas = 0x0d1015;      ///< page behind the board
    std::uint32_t boardFill = 0x0b0f14;   ///< the board itself (substrate under the mask)
    std::uint32_t boardEdge = 0xff3df5;   ///< board outline
    std::uint32_t grid = 0x232830;        ///< dot grid
    std::uint32_t copperTop = 0x2f5bff;
    std::uint32_t copperMid = 0xb0823e;
    std::uint32_t copperBottom = 0xe2453c;
    std::uint32_t copperMulti = 0xf2e6a8; ///< through-hole pads / vias (multi-layer)
    std::uint32_t pad = 0xc8a02a;         ///< bare metal pad
    std::uint32_t padEdge = 0x7d6418;     ///< the darker rim of a pad
    std::uint32_t padHole = 0x090b0e;     ///< drilled hole
    std::uint32_t silk = 0xf2f2f2;
    std::uint32_t silkBottom = 0x9fc7ff;
    std::uint32_t designator = 0xffffff;
    std::uint32_t paste = 0x8a8f98;
    std::uint32_t solderMask = 0x5a6068;
    std::uint32_t keepOut = 0xe0b44c;
    std::uint32_t mechanical = 0x767c88;
    std::uint32_t other = 0x6b7280;

    // 3D
    std::uint32_t mask = 0x1b3c7a;        ///< solder mask on the top of the board
    std::uint32_t substrate = 0xd6cfb4;   ///< the laminate seen on the board edge
    std::uint32_t copperUnderMask = 0x2a5bb8;  ///< copper showing through the mask
    std::uint32_t barrel = 0xd9ae35;      ///< plated barrel of a through hole
    std::uint32_t hole = 0x08090b;
    std::uint32_t body = 0x24272c;
    std::uint32_t bodySide = 0x1b1e22;
    std::uint32_t metal = 0xc3cad2;
    std::uint32_t threeTop = 0x8f8f92;    ///< studio background, top
    std::uint32_t threeBottom = 0x6f7074; ///< studio background, bottom
};

inline ThemeColours easyEdaColours() {
    ThemeColours colours;
    return colours;
}

inline ThemeColours classicColours() {
    ThemeColours colours;
    colours.canvas = 0x10141a;
    colours.boardFill = 0x0b0f14;
    colours.boardEdge = 0x97a1ae;
    colours.grid = 0x1b2027;
    colours.copperTop = 0xe05c5c;
    colours.copperMid = 0xc08a3e;
    colours.copperBottom = 0x4c86d9;
    colours.copperMulti = 0xf2e6a8;
    colours.pad = 0xf2e6a8;
    colours.padEdge = 0x8c8054;
    colours.padHole = 0x10141a;
    colours.silk = 0xe6e6e6;
    colours.silkBottom = 0xb68ce0;
    colours.designator = 0xf5d76e;
    colours.mask = 0x16684a;
    colours.substrate = 0xa8996f;
    colours.copperUnderMask = 0x2f8f63;
    colours.barrel = 0xd8b26a;
    colours.hole = 0x0b0d10;
    colours.threeTop = 0x11161c;
    colours.threeBottom = 0x0b0f13;
    return colours;
}

inline const ThemeColours& themeColours(BoardTheme theme) {
    static const ThemeColours easy = easyEdaColours();
    static const ThemeColours classic = classicColours();
    return theme == BoardTheme::Classic ? classic : easy;
}

/// The theme in force. A viewer setting, not part of a document: the GUI switches it from
/// the View menu and the CLI reads `PCBV_THEME=classic` from the environment.
inline BoardTheme& currentTheme() {
    static BoardTheme theme = [] {
        const char* configured = std::getenv("PCBV_THEME");
        if (configured != nullptr &&
            (std::strcmp(configured, "classic") == 0 || std::strcmp(configured, "Classic") == 0)) {
            return BoardTheme::Classic;
        }
        return BoardTheme::EasyEda;
    }();
    return theme;
}

inline void setBoardTheme(BoardTheme theme) { currentTheme() = theme; }

inline const ThemeColours& themeColours() { return themeColours(currentTheme()); }

/// 0xRRGGBB colour used for a layer in the board view and in SVG exports.
inline std::uint32_t layerColour(int layer, const ThemeColours& colours) {
    switch (layerGroup(layer)) {
        case LayerGroup::TopCopper: return colours.copperTop;
        case LayerGroup::MidCopper: return colours.copperMid;
        case LayerGroup::BottomCopper: return colours.copperBottom;
        case LayerGroup::TopSilk: return colours.silk;
        case LayerGroup::BottomSilk: return colours.silkBottom;
        case LayerGroup::Paste: return colours.paste;
        case LayerGroup::SolderMask: return colours.solderMask;
        case LayerGroup::KeepOut: return colours.keepOut;
        case LayerGroup::Mechanical: return colours.mechanical;
        case LayerGroup::MultiLayer: return colours.copperMulti;
        case LayerGroup::Other: break;
    }
    return colours.other;
}

/// The same thing for the theme in force.
inline std::uint32_t layerColor(int layer) { return layerColour(layer, themeColours()); }

inline bool isCopper(int layer) {
    const LayerGroup group = layerGroup(layer);
    return group == LayerGroup::TopCopper || group == LayerGroup::MidCopper ||
           group == LayerGroup::BottomCopper || group == LayerGroup::MultiLayer;
}

inline bool isSilk(int layer) {
    return layerGroup(layer) == LayerGroup::TopSilk || layerGroup(layer) == LayerGroup::BottomSilk;
}

} // namespace pcbv
