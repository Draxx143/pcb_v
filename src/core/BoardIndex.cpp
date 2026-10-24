// BoardIndex.cpp — see BoardIndex.h.
#include "BoardIndex.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>

#include "AltiumText.h"
#include "LayerStyle.h"

namespace pcbv {
namespace {

std::string connectionLabel(const Pad& pad, const Document& document) {
    std::string designator;
    if (pad.componentIndex >= 0 &&
        pad.componentIndex < static_cast<int>(document.components.size())) {
        designator = document.components[static_cast<std::size_t>(pad.componentIndex)].designator;
    }
    if (designator.empty()) designator = "free";
    if (pad.designator.empty()) return designator;
    return designator + "-" + pad.designator;
}

struct LayerCounter {
    std::map<int, int> count;

    void add(int layer) {
        if (layer > 0) ++count[layer];
    }

    void addMany(int layer, int amount) {
        if (layer > 0) count[layer] += amount;
    }
};

} // namespace

void finalizeBoard(Document& document) {
    std::map<std::string, std::size_t> byName;
    for (std::size_t i = 0; i < document.nets.size(); ++i) {
        byName[document.nets[i].name] = i;
    }

    // --- nets ---------------------------------------------------------------
    std::map<std::string, NetSummary> summaries;
    auto summaryFor = [&](const std::string& name, int index) -> NetSummary& {
        auto it = summaries.find(name);
        if (it != summaries.end()) return it->second;
        NetSummary summary;
        summary.name = name;
        summary.index = index;
        return summaries.emplace(name, summary).first->second;
    };

    for (const auto& net : document.nets) summaryFor(net.name, net.index);

    for (const auto& pad : document.geometry.pads) {
        if (pad.net.empty()) continue;
        NetSummary& summary = summaryFor(pad.net, pad.netIndex);
        ++summary.pads;
        summary.connections.push_back(connectionLabel(pad, document));
    }

    for (const auto& track : document.geometry.tracks) {
        if (track.net.empty()) continue;
        NetSummary& summary = summaryFor(track.net, -1);
        ++summary.tracks;
        summary.trackLength += std::hypot(track.x2 - track.x1, track.y2 - track.y1);
    }

    for (const auto& via : document.geometry.vias) {
        if (via.net.empty()) continue;
        ++summaryFor(via.net, -1).vias;
    }

    for (const auto& region : document.geometry.regions) {
        if (region.net.empty()) continue;
        // A pour belongs to its net but is not routed copper.
        summaryFor(region.net, -1);
    }

    document.netSummaries.clear();
    document.netSummaries.reserve(summaries.size());
    for (auto& entry : summaries) {
        NetSummary& summary = entry.second;
        std::sort(summary.connections.begin(), summary.connections.end(),
                  [](const std::string& a, const std::string& b) {
                      return text::naturalLess(a, b);
                  });
        summary.connections.erase(
            std::unique(summary.connections.begin(), summary.connections.end()),
            summary.connections.end());
        // Nets declared in the file but without any copper are leftovers of deleted
        // routing; listing them only adds noise.
        if (summary.pads == 0 && summary.tracks == 0 && summary.vias == 0) continue;
        if (!summary.name.empty()) document.netSummaries.push_back(std::move(summary));
    }
    std::sort(document.netSummaries.begin(), document.netSummaries.end(),
              [](const NetSummary& a, const NetSummary& b) {
                  return text::naturalLess(a.name, b.name);
              });

    // --- layer usage --------------------------------------------------------
    LayerCounter counter;
    for (const auto& track : document.geometry.tracks) counter.add(track.layer);
    for (const auto& arc : document.geometry.arcs) counter.add(arc.layer);
    for (const auto& fill : document.geometry.fills) counter.add(fill.layer);
    for (const auto& text : document.geometry.texts) counter.add(text.layer);
    for (const auto& region : document.geometry.regions) counter.add(region.layer);
    for (const auto& pad : document.geometry.pads) {
        // SMD pads live on their own layer, through-hole pads span every copper layer.
        if (pad.holeSize > 0.0) {
            counter.addMany(1, 1);
            counter.addMany(32, 1);
        } else {
            counter.add(pad.layer);
        }
    }
    for (const auto& via : document.geometry.vias) {
        (void)via;  // vias are counted as multi-layer in the panel
    }
    if (!document.geometry.vias.empty()) counter.addMany(74, static_cast<int>(document.geometry.vias.size()));

    document.layerUsage.clear();
    for (const auto& entry : counter.count) {
        LayerUsage usage;
        usage.layer = entry.first;
        usage.name = text::pcbLayerName(entry.first);
        usage.primitives = entry.second;
        usage.copper = isCopper(entry.first);
        document.layerUsage.push_back(usage);
    }
    std::sort(document.layerUsage.begin(), document.layerUsage.end(),
              [](const LayerUsage& a, const LayerUsage& b) {
                  const int groupA = static_cast<int>(layerGroup(a.layer));
                  const int groupB = static_cast<int>(layerGroup(b.layer));
                  if (groupA != groupB) return groupA < groupB;
                  return a.layer < b.layer;
              });
}

std::string copperStackup(const Document& document) {
    int signal = 0;
    int planes = 0;
    for (const auto& usage : document.layerUsage) {
        if (usage.layer == 1 || usage.layer == 32 || (usage.layer >= 2 && usage.layer <= 31)) {
            ++signal;
        } else if (usage.layer >= 39 && usage.layer <= 54) {
            ++planes;
        }
    }
    if (signal == 0) return std::string();
    std::string text = std::to_string(signal) + (signal == 1 ? " copper layer" : " copper layers");
    if (planes > 0) text += " + " + std::to_string(planes) + " internal plane(s)";
    return text;
}

} // namespace pcbv
