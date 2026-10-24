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
    std::vector<EmbeddedModel> models;   ///< real 3D models (STEP), when the library has them
};

/// One drawing primitive of a schematic symbol (Altium RECORD 6/8/10/12/13/14/28).
struct SymbolShape {
    enum class Kind { Line, Polyline, Rectangle, RoundRectangle, Arc, Ellipse, Text };

    Kind kind = Kind::Line;
    std::vector<Point> points;     // mil: line/polyline vertices, rectangle corners
    double radius = 0.0;           // arc / ellipse, mil
    double secondaryRadius = 0.0;  // ellipse second radius, mil
    double startAngle = 0.0;       // arc, degrees
    double endAngle = 360.0;
    double width = 0.0;            // stroke width, mil (0 = hairline)
    double rotation = 0.0;         // text / round rectangle, degrees
    int fill = 0;                  // 0 = none, 1 = outline, 2 = filled (Altium FILLED)
    std::string text;              // text shapes
    double textHeight = 0.0;       // mil
    int color = 0;                 // 0 = default (layer colour)
    int ownerPart = 1;             // sub-part the shape belongs to (0/-1 = shared)
};

/// A pin with its drawing geometry (schematic symbols).
struct SymbolPin {
    std::string designator;  // "1", "A3", …
    std::string name;        // "GND", …
    std::string electrical;  // "Power", "Input", …
    double x = 0.0;          // connection point, mil
    double y = 0.0;
    double length = 0.0;     // mil
    int rotation = 0;        // 0 = right, 90 = up, 180 = left, 270 = down
    int ownerPart = 1;
    bool hidden = false;
    bool showName = true;
    bool showDesignator = true;
};

/// One symbol of a schematic library.
struct LibrarySymbol {
    std::string name;              // lib reference
    std::string description;
    std::string designatorPrefix;  // default designator, e.g. "U?"
    int partCount = 1;             // sub-parts (multi-part symbols)
    std::vector<Pin> pins;               // name/designator/electrical (list view)
    std::vector<SymbolPin> pinGeometry;  // the same pins with coordinates (drawing)
    std::vector<SymbolShape> shapes;     // the symbol's graphics
    std::vector<Property> parameters;

    bool hasDrawing() const { return !shapes.empty() || !pinGeometry.empty(); }
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

/// The same for one schematic symbol: its graphics are mapped onto the board drawing
/// model (lines/polylines -> tracks, rectangles/ellipses -> regions, text -> labels),
/// so the board view, the SVG export and printing work without a second renderer.
Document symbolDocument(const LibrarySymbol& symbol, const std::string& fileName);

/// Record-level helpers, exposed so the smoke test can feed a record stream directly
/// (a real .PcbLib is a compound file, which the test cannot build byte by byte).
namespace detail {
/// Reads one PcbLib pad record at `pos` and advances past it. A pad record always ends
/// with a per-layer sub-record whose size must be consumed even when it is zero.
bool parsePadRecord(const std::vector<std::uint8_t>& data, std::size_t& pos, Pad& pad);
} // namespace detail

/// Copies the real 3D models of the matching footprints onto the document's components.
/// A component is matched to a footprint by its pattern name (exact after normalising case
/// and punctuation, then a unique partial match). Returns how many components got a model.
std::size_t attachModels(Document& document, const Library& library);

/// Reads a library file and attaches whatever 3D models it has to `document`.
std::size_t attachModelsFromFile(Document& document, const std::string& libraryPath,
                                 std::string* error = nullptr);

/// One-line summary, e.g. "PcbLib · 12 footprints · 0 symbols".
std::string librarySummary(const Library& library);

/// Human readable listing (one line per item), used by the CLI.
std::string libraryToText(const Library& library);
/// The same listing as CSV (Excel friendly).
std::string libraryToCsv(const Library& library);

} // namespace pcbv
