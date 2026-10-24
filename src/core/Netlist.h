// Netlist.h — the connectivity table: every pad and the net that ties it to the others.
#pragma once

#include <string>
#include <vector>

#include "Model.h"

namespace pcbv {

/// One end of a connection: a pad (or a free pad with no component).
struct NetNode {
    std::string designator;   ///< component designator, "free" for a loose pad
    std::string pad;          ///< pad name inside the footprint
    std::string label;        ///< "R1-2" / "TP3-1" / "free-1"
    std::string layer;        ///< layer the pad is drawn on
    double x = 0.0;           ///< mil
    double y = 0.0;
    bool throughHole = false;
    int componentIndex = -1;  ///< index into Document::components, -1 for a free pad
};

/// A net and everything that is soldered to it.
struct NetConnection {
    std::string name;
    std::vector<NetNode> nodes;

    /// "R1-2, C5-1, …" — the joined labels.
    std::string labelList(std::size_t limit = 0) const;
};

/// The bucket name used for pads that carry no net ("(no net)").
const char* noNetName();

/// Builds the netlist of a board (empty for schematics). Nets are sorted naturally,
/// the nodes of a net are sorted by designator and then by pad name.
std::vector<NetConnection> buildNetlist(const Document& document);

/// Nets with a single pad: almost always routing that was never finished.
int danglingNetCount(const std::vector<NetConnection>& netlist);

/// Flat CSV: Net,Component,Pad,Layer,X (mm),Y (mm),ThroughHole,File.
std::string netlistToCsv(const Document& document);

/// Grouped text form used by `pcbv-cli --netlist`.
std::string netlistToText(const Document& document);

} // namespace pcbv
