// Export.h — text, CSV, JSON and HTML renderings of a parsed document.
#pragma once

#include <set>
#include <string>

#include "Model.h"

namespace pcbv {

/// Human readable summary (used by `--summary` and the GUI "Document" panel).
std::string toSummaryText(const Document& doc);

/// Full BOM as CSV (UTF-8, comma separated, CRLF line endings for Excel).
std::string toCsv(const Document& doc);

/// Machine readable JSON export.
std::string toJson(const Document& doc);

/// Self contained HTML report (open in any browser / print to PDF).
std::string toHtml(const Document& doc);

/// Which layers and extras the board drawing includes.
struct BoardRenderOptions {
    /// Altium layer ids that are switched off in the Layers panel.
    std::set<int> hiddenLayers;
    bool silk = true;        ///< silkscreen text and silk tracks (also honoured per layer)
    bool pads = true;
    bool designators = true;

    bool isHidden(int layer) const { return hiddenLayers.find(layer) != hiddenLayers.end(); }
};

/// Layers that are hidden the first time a board is opened: manufacturing-only
/// layers (paste, solder mask, mechanical, keep-out, drill) would only add noise.
std::set<int> defaultHiddenLayers();

/// Standalone SVG drawing of the PCB geometry (empty string for schematics).
std::string toSvg(const Document& doc, const BoardRenderOptions& options = BoardRenderOptions());

/// Formatting helpers shared with the GUI.
std::string formatMil(double mil);        ///< "1234.56" (mil)
std::string formatMm(double mil);         ///< "31.357" (mm)
std::string formatQty(int value);

} // namespace pcbv
