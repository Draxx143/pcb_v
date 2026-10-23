// AltiumParser.h — reads Altium .PcbDoc and .SchDoc files and extracts the
// component (BOM) information, nets, pads and document statistics.
#pragma once

#include <string>

#include "Model.h"

namespace pcbv {

struct ParseOutcome {
    bool ok = false;
    std::string error;
    /// What the container sniffer recognised: "OLE2 compound file (binary Altium)",
    /// "Altium ASCII export (plain text records)", "ZIP archive", "unrecognised", ...
    std::string detected;
    /// Printable preview of the first bytes of the file (helps diagnosing bad files).
    std::string signature;
    Document document;
};

/// Detects the document type from the file extension (case-insensitive).
DocType detectType(const std::string& path);

/// Reads an Altium document. Never throws; inspect ParseOutcome::ok / error.
ParseOutcome parseFile(const std::string& path);

} // namespace pcbv
