// Netlist.cpp — see Netlist.h.
#include "Netlist.h"

#include <algorithm>
#include <map>
#include <sstream>

#include "AltiumText.h"
#include "Export.h"

namespace pcbv {
namespace {

std::string escapeCsv(const std::string& value) {
    if (value.find_first_of(",\"\r\n") == std::string::npos) return value;
    std::string out = "\"";
    for (char c : value) {
        if (c == '"') out += '"';
        out += c;
    }
    out += '"';
    return out;
}

std::string layerNameOf(const Pad& pad) {
    if (!pad.layerName.empty()) return pad.layerName;
    const int layer = pad.holeSize > 0.0 ? 74 : (pad.layer == 0 ? 1 : pad.layer);
    return text::pcbLayerName(layer);
}

/// Unconnected pads are grouped together instead of being dropped: a floating pad is
/// exactly what a netlist should reveal.
const char* kNoNet = "(no net)";

} // namespace

const char* noNetName() { return kNoNet; }

std::string NetConnection::labelList(std::size_t limit) const {
    std::string text;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (limit > 0 && i >= limit) {
            text += ", …";
            break;
        }
        if (i > 0) text += ", ";
        text += nodes[i].label;
    }
    return text;
}

std::vector<NetConnection> buildNetlist(const Document& document) {
    std::map<std::string, NetConnection> byNet;

    for (const auto& pad : document.geometry.pads) {
        const std::string net = pad.net.empty() ? std::string(kNoNet) : pad.net;
        NetConnection& connection = byNet[net];
        if (connection.name.empty()) connection.name = net;

        NetNode node;
        node.pad = pad.designator;
        node.x = pad.x;
        node.y = pad.y;
        node.throughHole = pad.holeSize > 0.0;
        node.layer = layerNameOf(pad);
        node.componentIndex = pad.componentIndex;
        if (pad.componentIndex >= 0 &&
            pad.componentIndex < static_cast<int>(document.components.size())) {
            node.designator =
                document.components[static_cast<std::size_t>(pad.componentIndex)].designator;
        }
        if (node.designator.empty()) node.designator = "free";
        node.label = node.pad.empty() ? node.designator : node.designator + "-" + node.pad;
        connection.nodes.push_back(std::move(node));
    }

    std::vector<NetConnection> netlist;
    netlist.reserve(byNet.size());
    for (auto& entry : byNet) {
        std::sort(entry.second.nodes.begin(), entry.second.nodes.end(),
                  [](const NetNode& a, const NetNode& b) {
                      if (a.designator != b.designator) return text::naturalLess(a.designator, b.designator);
                      return text::naturalLess(a.pad, b.pad);
                  });
        netlist.push_back(std::move(entry.second));
    }

    // The "no net" bucket always sorts last; named nets sort naturally.
    std::sort(netlist.begin(), netlist.end(), [](const NetConnection& a, const NetConnection& b) {
        const bool aNoNet = a.name == kNoNet;
        const bool bNoNet = b.name == kNoNet;
        if (aNoNet != bNoNet) return bNoNet;
        return text::naturalLess(a.name, b.name);
    });
    return netlist;
}

int danglingNetCount(const std::vector<NetConnection>& netlist) {
    int count = 0;
    for (const auto& net : netlist) {
        if (net.nodes.size() == 1 && net.name != kNoNet) ++count;
    }
    return count;
}

std::string netlistToCsv(const Document& document) {
    std::ostringstream out;
    out << "Net,Component,Pad,Label,Layer,X (mm),Y (mm),ThroughHole,File\r\n";
    for (const auto& net : buildNetlist(document)) {
        for (const auto& node : net.nodes) {
            out << escapeCsv(net.name) << ',' << escapeCsv(node.designator) << ','
                << escapeCsv(node.pad) << ',' << escapeCsv(node.label) << ','
                << escapeCsv(node.layer) << ',' << formatMm(node.x) << ',' << formatMm(node.y)
                << ',' << (node.throughHole ? "yes" : "") << ','
                << escapeCsv(document.fileName) << "\r\n";
        }
    }
    return out.str();
}

std::string netlistToText(const Document& document) {
    const std::vector<NetConnection> netlist = buildNetlist(document);
    if (netlist.empty()) return std::string("no pads in this document\n");

    std::size_t pads = 0;
    std::size_t unconnected = 0;
    std::size_t named = 0;
    for (const auto& net : netlist) {
        pads += net.nodes.size();
        if (net.name == kNoNet) unconnected = net.nodes.size();
        else ++named;
    }

    std::ostringstream out;
    out << document.fileName << "  —  " << named << " nets, " << pads << " pads";
    if (unconnected > 0) out << "  (" << unconnected << " unconnected)";
    const int dangling = danglingNetCount(netlist);
    if (dangling > 0) out << "  (" << dangling << " net(s) with a single pad)";
    out << "\n";

    for (const auto& net : netlist) {
        out << "\n" << net.name << "  (" << net.nodes.size()
            << (net.nodes.size() == 1 ? " pad" : " pads") << ")\n";
        for (const auto& node : net.nodes) {
            std::string line = "    ";
            line += node.label;
            while (line.size() < 20) line += ' ';
            line += node.layer;
            while (line.size() < 42) line += ' ';
            line += formatMm(node.x) + ", " + formatMm(node.y) + " mm";
            out << line << "\n";
        }
    }
    return out.str();
}

} // namespace pcbv
