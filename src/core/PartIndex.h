// PartIndex.h — the project's part database, stored as one readable JSON file.
//
// A row of `libraries/index.json` ties a part number to the four files that describe it:
// the schematic symbol, the PCB footprint, the 3D model and the datasheet. The viewer uses
// it for searching ("0603 10k resistor"), for checking that every referenced file really
// exists, and later for the schematic/PCB cross-check.
//
// Format (version 1):
//
//   { "format": "pcbv-part-index", "version": 1,
//     "parts": [ { "id": "R-0603-10k", "mpn": "RC0603FR-0710KL", "manufacturer": "Yageo",
//                  "value": "10k", "package": "0603", "description": "10 kΩ 1 % resistor",
//                  "datasheet": "libraries/datasheets/…pdf",
//                  "symbol": "libraries/symbols/Device_RC.kicad_sym",
//                  "footprint": "libraries/footprints/R_0603_1608Metric.kicad_mod",
//                  "model": "libraries/models3d/R_0603_1608Metric.step",
//                  "tags": ["resistor", "smd"] } ] }
//
// Paths are relative to the folder that holds the index file.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace pcbv {

/// One part of the index.
struct PartEntry {
    std::string id;
    std::string mpn;           ///< manufacturer part number (orderable)
    std::string manufacturer;
    std::string value;         ///< 10k, 100nF, LM317 …
    std::string package;       ///< 0603, SOT-23, TO-252 …
    std::string description;
    std::string datasheet;     ///< local PDF or URL
    std::string symbol;        ///< .kicad_sym or .SchLib
    std::string footprint;     ///< .kicad_mod or .PcbLib
    std::string model;         ///< .step / .wrl
    std::vector<std::string> tags;

    /// The text a search looks at, lower-cased, one field per line.
    std::string searchText() const;
};

/// A loaded `index.json`.
struct PartIndex {
    std::string path;          ///< file the index was read from
    std::string format;
    int version = 0;
    std::vector<PartEntry> parts;
    std::vector<std::string> warnings;

    /// Reads `path`; on failure `error` explains why.
    bool load(const std::string& path, std::string& error);

    /// Folder relative paths are resolved against.
    std::string folder() const;
};

/// One search result.
struct PartMatch {
    const PartEntry* part = nullptr;
    int score = 0;
    std::vector<std::string> tokens;   ///< query words that matched
};

/// Finds parts for a natural query. Every word must match somewhere (AND), and the score
/// prefers an exact part number, then the package, the value and the tags.
std::vector<PartMatch> searchParts(const PartIndex& index, const std::string& query,
                                   std::size_t limit = 25);

/// Result of checking that every referenced file exists.
struct PartIndexCheck {
    std::size_t parts = 0;
    std::size_t missing = 0;
    std::vector<std::string> messages;   ///< one line per missing file (or per bad row)
    bool ok() const { return missing == 0; }
};

/// Verifies the index against the disk (paths are resolved next to the index file).
PartIndexCheck checkPartIndex(const PartIndex& index);

/// Human readable listing of the whole index.
std::string partIndexToText(const PartIndex& index);

/// Human readable search results (used by the CLI).
std::string partMatchesToText(const PartIndex& index, const std::vector<PartMatch>& matches,
                              const std::string& query);

} // namespace pcbv
