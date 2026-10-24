// Model.h — Document model shared by the CLI and the Qt GUI (no Qt dependencies here).
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "CompoundFile.h"

namespace pcbv {

enum class DocType {
    Unknown,
    PcbDoc,  ///< Altium PCB document
    SchDoc,  ///< Altium schematic document
    PcbLib,  ///< Altium PCB footprint library (not parsed yet)
    SchLib   ///< Altium schematic library (not parsed yet)
};

std::string docTypeName(DocType type);

/// Physical container the document was read from.
enum class Container {
    Unknown,
    CompoundBinary, ///< Microsoft OLE2 / Compound File (Altium's native format)
    AsciiText       ///< Altium's ASCII export ("PCB ASCII File", "Schematic Capture Ascii File")
};

std::string containerName(Container container);

/// A schematic pin (SchDoc only).
struct Pin {
    std::string name;         // e.g. "VCC"
    std::string designator;   // e.g. "1"
    std::string electrical;   // e.g. "Input", "Power"
    int ownerPart = 0;
};

/// A PCB pad (PcbDoc only).
struct Pad {
    std::string designator;   // e.g. "A1"
    std::string net;          // net name (resolved) or empty
    int layer = 0;            // Altium layer id
    std::string layerName;    // human readable layer
    int componentIndex = -1;  // -1 = free pad
    int netIndex = -1;
    double x = 0.0;           // mil
    double y = 0.0;
    double width = 0.0;       // mil
    double height = 0.0;      // mil
    double holeSize = 0.0;    // mil, 0 = SMD
    double rotation = 0.0;    // degrees
    bool plated = false;
};

/// A 2D point in mil (PCB coordinates: X to the right, Y up).
struct Point {
    double x = 0.0;
    double y = 0.0;
};

/// A straight copper/silk segment (PcbDoc "Track").
struct TrackFeature {
    double x1 = 0.0, y1 = 0.0;
    double x2 = 0.0, y2 = 0.0;
    double width = 0.0;   // mil
    int layer = 0;        // Altium layer id
    std::string net;
};

/// A circular arc (PcbDoc "Arc"): centre, radius, start/end angle in degrees.
struct ArcFeature {
    double cx = 0.0, cy = 0.0;
    double radius = 0.0;
    double startAngle = 0.0;
    double endAngle = 360.0;
    double width = 0.0;   // mil
    int layer = 0;
};

/// A plated through hole (PcbDoc "Via").
struct ViaFeature {
    double x = 0.0, y = 0.0;
    double diameter = 0.0;  // mil
    double hole = 0.0;      // mil
    std::string net;
};

/// A filled polygon (PcbDoc "Region", also used for the board shape).
struct RegionFeature {
    std::vector<Point> outline;
    int layer = 0;
    bool boardOutline = false;  ///< true for the board shape / cut-outs
    bool pour = false;          ///< copper pour (Polygon record); fill not computed
    std::string net;
};

/// A rotated filled rectangle (PcbDoc "Fill").
struct FillFeature {
    double x1 = 0.0, y1 = 0.0;
    double x2 = 0.0, y2 = 0.0;
    double rotation = 0.0;
    int layer = 0;
};

/// A silkscreen / documentation string (PcbDoc "Text").
struct TextFeature {
    double x = 0.0, y = 0.0;
    double height = 0.0;    // mil
    double rotation = 0.0;
    int layer = 0;
    std::string text;
    std::uint32_t color = 0;  // 0xRRGGBB override (0 = use the layer colour)
    int anchor = 0;           // 0 = centred, 1 = left aligned, 2 = right aligned
};

/// Every drawable primitive of a PCB document (used by the board view / SVG export).
struct PcbGeometry {
    std::vector<TrackFeature> tracks;
    std::vector<ArcFeature> arcs;
    std::vector<ViaFeature> vias;
    std::vector<RegionFeature> regions;
    std::vector<FillFeature> fills;
    std::vector<TextFeature> texts;
    std::vector<Pad> pads;
    std::vector<Point> outline;   ///< board contour (ASCII exports and binary Board6 when available)

    bool empty() const {
        return tracks.empty() && arcs.empty() && vias.empty() && regions.empty() &&
               fills.empty() && texts.empty() && pads.empty() && outline.size() < 3;
    }
    std::size_t primitiveCount() const {
        return tracks.size() + arcs.size() + vias.size() + regions.size() + fills.size() + texts.size();
    }
};

/// A single named property (schematic parameter, PCB component parameter, ...).
struct Property {
    std::string name;
    std::string value;
};

/// A real 3D model of a part: the STEP file Altium embeds in the footprint library.
struct EmbeddedModel {
    std::string name;       ///< file name inside the library ("QFN-24.step")
    std::string id;         ///< the library's model id ("{GUID}")
    std::string stepText;   ///< decoded ISO-10303-21 text ("" when it is another format)
    double rotationX = 0.0; ///< degrees, from the library's model record
    double rotationY = 0.0;
    double rotationZ = 0.0;
    double dz = 0.0;        ///< mm, model offset along the board normal
};

/// A component / part, from either the schematic or the PCB.
struct Component {
    std::string designator;      // "R12"
    std::string comment;         // value shown on the board ("10k")
    std::string name;            // library reference / footprint pattern
    std::string footprint;       // PCB footprint name
    std::string description;
    std::string manufacturer;
    std::string partNumber;      // MPN / orderable part number
    std::string supplier;        // distributor
    std::string supplierPartNumber;
    std::string datasheet;       // URL if present
    std::string layer;           // PCB layer ("TOP", "BOTTOM", ...)
    std::string sourceLibrary;   // source schematic / footprint library
    std::string sourceFootprintLibrary;
    std::string uniqueId;
    double x = 0.0;              // mil (PCB position)
    double y = 0.0;
    double rotation = 0.0;       // degrees
    double height = 0.0;         // mil
    int pinCount = 0;            // schematic pins
    int padCount = 0;            // PCB pads
    int throughHolePads = 0;
    int partCount = 1;           // sub-parts for multi-part symbols
    int modelIndex = -1;         // index into Document::models (-1 = no 3D model)
    int componentIndex = -1;     // index inside Components6 (PCB)
    std::vector<Pin> pins;
    std::vector<Pad> pads;
    std::vector<Property> properties; // every raw parameter, in file order
};

struct Net {
    std::string name;
    int index = -1;
};

/// Aggregated view of one net: what is connected and how much copper carries it.
struct NetSummary {
    std::string name;
    int index = -1;
    int pads = 0;
    int tracks = 0;
    int vias = 0;
    double trackLength = 0.0;              ///< mil, sum of segment lengths
    std::vector<std::string> connections;  ///< "R1-2" style pad references, sorted
};

/// How much geometry sits on one layer (drives the Layers panel).
struct LayerUsage {
    int layer = 0;
    std::string name;
    int primitives = 0;
    bool copper = false;
};

/// Primitive counters for a PCB document.
struct PcbCounts {
    int tracks = 0;
    int arcs = 0;
    int vias = 0;
    int pads = 0;
    int regions = 0;
    int boardRegions = 0;
    int polygons = 0;
    int texts = 0;
    int fills = 0;
    int componentBodies = 0;
    int nets = 0;
    int rules = 0;
    int classes = 0;
};

/// Primitive counters for a schematic document.
struct SchCounts {
    int wires = 0;
    int buses = 0;
    int netLabels = 0;
    int ports = 0;
    int powerPorts = 0;
    int junctions = 0;
    int texts = 0;
    int pins = 0;
    int components = 0;
};

/// Bounding box of all placed geometry, in mil.
struct Bounds {
    bool valid = false;
    double minX = 0.0, minY = 0.0, maxX = 0.0, maxY = 0.0;
    void add(double x, double y);
    double width() const { return valid ? maxX - minX : 0.0; }
    double height() const { return valid ? maxY - minY : 0.0; }
};

/// Result of reading one Altium document.
struct Document {
    std::string path;
    std::string fileName;
    DocType type = DocType::Unknown;
    Container container = Container::Unknown;
    std::string formatVersion;   // "PCB 6.0 Binary File" / "Protel ... Version 5.0"
    std::string signature;       // printable preview of the first bytes (diagnostics)
    std::string boardVersion;    // VERSION= from Board6 ("5.01")
    std::string lastSaved;       // DATE + TIME from Board6 when available
    std::uint64_t fileSize = 0;

    std::vector<Component> components;
    /// Real 3D models attached to this document (from a footprint library); a component
    /// points into this list with Component::modelIndex.
    std::vector<EmbeddedModel> models;
    std::vector<Net> nets;
    std::vector<FileEntry> structure;  // OLE container tree (binary documents only)

    /// Record-type histogram (ASCII exports): {"Pad", 102}, {"Track", 465}, ...
    std::vector<std::pair<std::string, int>> recordTypes;

    PcbGeometry geometry;   ///< drawable primitives (empty for schematics)

    /// Net / layer indexes (PCB documents; computed once after parsing).
    std::vector<NetSummary> netSummaries;
    std::vector<LayerUsage> layerUsage;

    PcbCounts pcb;
    SchCounts sch;
    Bounds bounds;

    std::vector<std::string> warnings;

    /// Convenience: units used for coordinates in this document.
    std::string unitLabel() const { return "mil"; }
};

} // namespace pcbv
