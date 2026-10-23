// AltiumParser.h — reads Altium .PcbDoc and .SchDoc files and extracts the
// component (BOM) information, nets, pads and document statistics.
#pragma once

#include <string>

#include "Model.h"

namespace pcbv {

struct ParseOutcome {
    bool ok = false;
    std::string error;
    Document document;
};

/// Detects the document type from the file extension (case-insensitive).
DocType detectType(const std::string& path);

/// Reads an Altium document. Never throws; inspect ParseOutcome::ok / error.
ParseOutcome parseFile(const std::string& path);

} // namespace pcbv
