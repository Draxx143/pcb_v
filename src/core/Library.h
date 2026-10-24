// Library.h — Altium component libraries (.PcbLib footprints / .SchLib symbols).
//
// A library is read into its own model instead of a board: every footprint keeps the
// primitives that belong to it, so the GUI can draw one footprint on its own and the
// CLI can list names, pad counts and dimensions without a board around them.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "CompoundFile.h"
#include "Model.h"

namespace pcbv {

/// One footprint of a PCB library (the library equivalent of a Component).
struct LibraryFootprint {
    std::string name;         // footprint / pattern name
    std::string description;
    std::string itemGuid;
    double height = 0.0;      // mil (HEIGHT parameter)
    Bounds bounds;            // extent of the primitives, in mil
    PcbGeometry geometry;     // pads, tracks, arcs, vias, fills, regions
    std::vector<Property> parameters;
    int padCount = 0;
    int trackCount = 0;
    int arcCount = 0;
    int viaCount = 0;
    int fillCount = 0;
    int regionCount = 0;
    int textCount = 0;
    int bodyCount = 0;
};

/// One symbol of a schematic library.
struct LibrarySymbol {
    std::string name;              // lib reference
    std::string description;
    std::string designatorPrefix;  // default designator, e.g. "U?"
    int partCount = 1;             // sub-parts (multi-part symbols)
    std::vector<Pin> pins;
    std::vector<Property> parameters;
};

/// A whole library file, whichever of the two kinds it is.
struct Library {
    DocType type = DocType::Unknown;      // PcbLib or SchLib
    Container container = Container::Unknown;
    std::string fileName;
    std::string formatVersion;
    std::vector<LibraryFootprint> footprints;
    std::vector<LibrarySymbol> symbols;
    std::vector<FileEntry> structure;     // OLE tree (binary libraries only)
    std::vector<std::string> warnings;

    bool isFootprintLibrary() const { return type == DocType::PcbLib; }
    std::size_t itemCount() const { return footprints.size() + symbols.size(); }
};

/// Reads a binary .PcbLib / .SchLib from an already opened compound file.
/// Returns false (and fills `error`) when the container does not look like a library.
bool readBinaryLibrary(CompoundFile& file, Library& library, std::string& error);

/// Reads an Altium ASCII library export (same record syntax as the ASCII documents).
void readAsciiLibrary(const std::vector<std::uint8_t>& bytes, Library& library);

/// Fills a Document from a library: one Component per footprint/symbol, plus counters,
/// so the existing tables, filters and exports work unchanged.
void buildLibraryDocument(Document& document, Library& library);

/// A stand-alone document holding exactly one footprint, for the board view.
Document footprintDocument(const LibraryFootprint& footprint, const std::string& fileName);

/// One-line summary, e.g. "PcbLib · 12 footprints · 0 symbols".
std::string librarySummary(const Library& library);

/// Human readable listing (one line per item), used by the CLI.
std::string libraryToText(const Library& library);
/// The same listing as CSV (Excel friendly).
std::string libraryToCsv(const Library& library);

} // namespace pcbv
