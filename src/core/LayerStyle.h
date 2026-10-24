// LayerStyle.h — how PCB layers are grouped and coloured (shared by SVG and Qt output).
//
// Altium layer ids: 1 = Top Layer, 2..31 = Mid Layer 1..30, 32 = Bottom Layer,
// 33/34 = Top/Bottom Overlay, 35/36 = Top/Bottom Paste, 37/38 = Top/Bottom Solder,
// 39..54 = internal planes, 55 = Drill Guide, 56 = Keep-Out, 57..72 = Mechanical 1..16,
// 73 = Drill Drawing, 74 = Multi-Layer.
#pragma once

#include <cstdint>

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

/// 0xRRGGBB colour used for a layer in the board view and in SVG exports.
inline std::uint32_t layerColor(int layer) {
    switch (layerGroup(layer)) {
        case LayerGroup::TopCopper: return 0xE05C5C;
        case LayerGroup::MidCopper: return 0xC08A3E;
        case LayerGroup::BottomCopper: return 0x4C86D9;
        case LayerGroup::TopSilk: return 0xE6E6E6;
        case LayerGroup::BottomSilk: return 0xB68CE0;
        case LayerGroup::Paste: return 0x8A8F98;
        case LayerGroup::SolderMask: return 0x5A6068;
        case LayerGroup::KeepOut: return 0xE0B44C;
        case LayerGroup::Mechanical: return 0x767C88;
        case LayerGroup::MultiLayer: return 0xF2E6A8;
        case LayerGroup::Other: break;
    }
    return 0x6B7280;
}

inline bool isCopper(int layer) {
    const LayerGroup group = layerGroup(layer);
    return group == LayerGroup::TopCopper || group == LayerGroup::MidCopper ||
           group == LayerGroup::BottomCopper || group == LayerGroup::MultiLayer;
}

inline bool isSilk(int layer) {
    return layerGroup(layer) == LayerGroup::TopSilk || layerGroup(layer) == LayerGroup::BottomSilk;
}

} // namespace pcbv
