// Search.h — "find in everything that is open".
//
// The GUI keeps several documents (and libraries) in tabs; this module answers the
// question "where does this text appear?" without any Qt code, so it can be tested.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "Library.h"
#include "Model.h"

namespace pcbv {

struct SearchHit {
    enum class Kind { Component, Net, Layer, Footprint, Symbol };

    Kind kind = Kind::Component;
    std::string name;    ///< designator / net name / footprint name / …
    std::string detail;  ///< value, pad count, size, …
    int index = -1;      ///< component index, net index or library item index
};

/// Human readable label of a hit kind ("Component", "Net", "Footprint", …).
std::string searchKindName(SearchHit::Kind kind);

/// Searches one document (components, nets, layers) and, if it is a library, its items.
/// Matching is case-insensitive on the name, the detail and the raw parameters.
/// At most `maxHits` hits are returned (a huge board must not stall the dialog).
std::vector<SearchHit> searchDocument(const Document& document, const Library& library,
                                      const std::string& needle, std::size_t maxHits = 300);

/// Number of places a document was searched in (used for the "nothing found" hint).
std::size_t searchableItemCount(const Document& document, const Library& library);

} // namespace pcbv
