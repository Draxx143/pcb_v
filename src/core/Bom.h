// Bom.h — grouped bill of materials (the same parts collapsed into one line).
#pragma once

#include <string>
#include <vector>

#include "Model.h"

namespace pcbv {

/// One line of the grouped BOM: every component that shares value, footprint and
/// part number becomes a single entry with a quantity and a list of designators.
struct BomEntry {
    std::string comment;
    std::string footprint;
    std::string partNumber;
    std::string manufacturer;
    std::string description;
    std::string supplier;
    std::string supplierPartNumber;
    std::string layers;                     ///< "Top" / "Bottom" / "Top + Bottom"
    std::vector<std::string> designators;   ///< naturally sorted (R1, R2, R10)
    int quantity = 0;
    int pins = 0;
    int pads = 0;
    bool dnp = false;                       ///< do not populate / not fitted
    bool throughHole = false;

    /// "R1, R2, R7" — the designator list as shown in the BOM table.
    std::string designatorList() const;
    /// Best available human readable name for the part.
    std::string label() const;
};

/// Groups document.components into BOM lines, ordered by their first designator.
std::vector<BomEntry> buildBom(const Document& document);

/// Grouped BOM as CSV (UTF-8, CRLF line endings for Excel).
std::string bomToCsv(const Document& document);

/// Grouped BOM as a standalone, printable HTML page.
std::string bomToHtml(const Document& document);

/// "12 lines · 50 parts · 3 DNP" — used by the GUI header and the CLI.
std::string bomSummary(const Document& document);

/// Altium has no fixed DNP flag: designers either add a parameter ("DNP", "Fitted",
/// "Do Not Populate", "BOM Ignore", …) or set the comment to "DNP". Both are honoured.
bool isDoNotPopulate(const Component& component);

/// Short description of where a component sits ("Top", "Bottom", "" for schematics).
std::string componentLayers(const Component& component);

} // namespace pcbv
