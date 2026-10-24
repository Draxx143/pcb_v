#include "Search.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

#include "AltiumText.h"

namespace pcbv {
namespace {

std::string lowered(const std::string& value) {
    std::string out = value;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool contains(const std::string& haystack, const std::string& needleLower) {
    if (needleLower.empty()) return false;
    return lowered(haystack).find(needleLower) != std::string::npos;
}

std::string mm(double mil) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(2);
    out << mil * 0.0254;
    return out.str();
}

/// True when the needle appears in any of the component's raw parameters.
bool matchesParameters(const std::vector<Property>& parameters, const std::string& needleLower) {
    for (const Property& property : parameters) {
        if (contains(property.name, needleLower) || contains(property.value, needleLower)) return true;
    }
    return false;
}

} // namespace

std::string searchKindName(SearchHit::Kind kind) {
    switch (kind) {
        case SearchHit::Kind::Component: return "Component";
        case SearchHit::Kind::Net: return "Net";
        case SearchHit::Kind::Layer: return "Layer";
        case SearchHit::Kind::Footprint: return "Footprint";
        case SearchHit::Kind::Symbol: return "Symbol";
    }
    return "Item";
}

std::size_t searchableItemCount(const Document& document, const Library& library) {
    return document.components.size() + document.netSummaries.size() + document.layerUsage.size() +
           library.footprints.size() + library.symbols.size();
}

std::vector<SearchHit> searchDocument(const Document& document, const Library& library,
                                      const std::string& needle, std::size_t maxHits) {
    std::vector<SearchHit> hits;
    const std::string needleLower = lowered(text::trim(needle));
    if (needleLower.empty() || maxHits == 0) return hits;

    auto add = [&hits, maxHits](SearchHit hit) {
        if (hits.size() >= maxHits) return;
        hits.push_back(std::move(hit));
    };

    // --- components (documents and library items both land here) -------------
    for (std::size_t i = 0; i < document.components.size() && hits.size() < maxHits; ++i) {
        const Component& c = document.components[i];
        const bool match = contains(c.designator, needleLower) || contains(c.comment, needleLower) ||
                           contains(c.footprint, needleLower) || contains(c.description, needleLower) ||
                           contains(c.manufacturer, needleLower) ||
                           contains(c.partNumber, needleLower) ||
                           contains(c.layer, needleLower) ||
                           matchesParameters(c.properties, needleLower);
        if (!match) continue;

        std::ostringstream detail;
        if (!c.comment.empty()) detail << c.comment;
        if (!c.footprint.empty()) detail << (detail.tellp() > 0 ? " · " : "") << c.footprint;
        if (c.padCount > 0) detail << (detail.tellp() > 0 ? " · " : "") << c.padCount << " pads";
        if (c.pinCount > 0) detail << (detail.tellp() > 0 ? " · " : "") << c.pinCount << " pins";
        add(SearchHit{SearchHit::Kind::Component, c.designator, detail.str(),
                      static_cast<int>(i)});
    }

    // --- nets ----------------------------------------------------------------
    // A net can exist without carrying copper yet (Altium keeps unused nets), so the net
    // list is searched as well as the per-net summary.
    {
        std::set<std::string> seen;
        for (const Net& net : document.nets) {
            if (net.name.empty() || !seen.insert(net.name).second) continue;
            if (!contains(net.name, needleLower)) continue;
            const auto summary = std::find_if(
                document.netSummaries.begin(), document.netSummaries.end(),
                [&net](const NetSummary& candidate) { return candidate.name == net.name; });
            std::ostringstream detail;
            if (summary != document.netSummaries.end()) {
                detail << summary->pads << " pads · " << summary->tracks << " tracks";
                if (summary->vias > 0) detail << " · " << summary->vias << " vias";
                if (summary->trackLength > 0.0) detail << " · " << mm(summary->trackLength) << " mm";
            } else {
                detail << "no connections";
            }
            add(SearchHit{SearchHit::Kind::Net, net.name, detail.str(), net.index});
        }
        for (const NetSummary& summary : document.netSummaries) {
            if (summary.name.empty() || !seen.insert(summary.name).second) continue;
            if (!contains(summary.name, needleLower)) continue;
            std::ostringstream detail;
            detail << summary.pads << " pads · " << summary.tracks << " tracks";
            if (summary.vias > 0) detail << " · " << summary.vias << " vias";
            if (summary.trackLength > 0.0) detail << " · " << mm(summary.trackLength) << " mm";
            add(SearchHit{SearchHit::Kind::Net, summary.name, detail.str(), summary.index});
        }
    }

    // --- layers --------------------------------------------------------------
    for (std::size_t i = 0; i < document.layerUsage.size() && hits.size() < maxHits; ++i) {
        const LayerUsage& layer = document.layerUsage[i];
        if (!contains(layer.name, needleLower)) continue;
        add(SearchHit{SearchHit::Kind::Layer, layer.name,
                      std::to_string(layer.primitives) + " primitives",
                      layer.layer});
    }

    // --- library items -------------------------------------------------------
    for (std::size_t i = 0; i < library.footprints.size() && hits.size() < maxHits; ++i) {
        const LibraryFootprint& footprint = library.footprints[i];
        if (!contains(footprint.name, needleLower) && !contains(footprint.description, needleLower) &&
            !matchesParameters(footprint.parameters, needleLower)) {
            continue;
        }
        std::ostringstream detail;
        detail << footprint.padCount << " pads";
        if (footprint.bounds.valid)
            detail << " · " << mm(footprint.bounds.width()) << " × " << mm(footprint.bounds.height())
                   << " mm";
        add(SearchHit{SearchHit::Kind::Footprint, footprint.name, detail.str(),
                      static_cast<int>(i)});
    }

    for (std::size_t i = 0; i < library.symbols.size() && hits.size() < maxHits; ++i) {
        const LibrarySymbol& symbol = library.symbols[i];
        if (!contains(symbol.name, needleLower) && !contains(symbol.description, needleLower) &&
            !matchesParameters(symbol.parameters, needleLower)) {
            continue;
        }
        std::ostringstream detail;
        detail << symbol.pins.size() << " pins";
        if (symbol.partCount > 1) detail << " · " << symbol.partCount << " parts";
        add(SearchHit{SearchHit::Kind::Symbol, symbol.name, detail.str(), static_cast<int>(i)});
    }

    return hits;
}

} // namespace pcbv
