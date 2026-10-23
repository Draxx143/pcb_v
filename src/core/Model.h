// Model.h — Document model shared by the CLI and the Qt GUI (no Qt dependencies here).
#pragma once

#include <cstdint>
#include <string>
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

/// A single named property (schematic parameter, PCB component parameter, ...).
struct Property {
    std::string name;
    std::string value;
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
    int componentIndex = -1;     // index inside Components6 (PCB)
    std::vector<Pin> pins;
    std::vector<Pad> pads;
    std::vector<Property> properties; // every raw parameter, in file order
};

struct Net {
    std::string name;
    int index = -1;
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
    std::string formatVersion;   // "PCB 6.0 Binary File" / "Protel ... Version 5.0"
    std::string boardVersion;    // VERSION= from Board6 ("5.01")
    std::string lastSaved;       // DATE + TIME from Board6 when available
    std::uint64_t fileSize = 0;

    std::vector<Component> components;
    std::vector<Net> nets;
    std::vector<FileEntry> structure;  // OLE container tree

    PcbCounts pcb;
    SchCounts sch;
    Bounds bounds;

    std::vector<std::string> warnings;

    /// Convenience: units used for coordinates in this document.
    std::string unitLabel() const { return "mil"; }
};

} // namespace pcbv
