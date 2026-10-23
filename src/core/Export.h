// Export.h — text, CSV, JSON and HTML renderings of a parsed document.
#pragma once

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

/// Formatting helpers shared with the GUI.
std::string formatMil(double mil);        ///< "1234.56" (mil)
std::string formatMm(double mil);         ///< "31.357" (mm)
std::string formatQty(int value);

} // namespace pcbv
