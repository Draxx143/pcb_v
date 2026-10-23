// AsciiParser.cpp — reader for Altium's *ASCII* exports:
//   * "PCB ASCII File (*.PcbDoc)"          — one "|RECORD=..." line per object
//   * "Advanced Schematic ASCII (*.SchDoc)" — same record numbers as the binary format
//
// ASCII files are what Altium produces with File -> Save As -> "PCB ASCII File".
// They are plain text, so the OLE2/Compound-File reader cannot open them; this
// module handles that variant (a common source of "not a compound file" errors).
#include "AltiumInternal.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "AltiumText.h"

namespace pcbv {
namespace detail {
namespace {

using text::parseBool;
using text::parseInt;
using text::parseMil;
using text::upper;
using text::valueOf;

std::string prettyLayer(const std::string& raw) {
    if (raw.empty()) return raw;
    const int id = text::pcbLayerIdFromName(raw);
    if (id != 0) return text::pcbLayerName(id);
    return raw;
}

/// Reads the vertex list of an ASCII polygon/region: |MAINCONTOURVERTEXCOUNT=n|VX0=..|VY0=..|...
void addOutlineBounds(const std::vector<Property>& props, Bounds& bounds) {
    for (int i = 0; i < 4096; ++i) {
        const Property* x = text::find(props, "VX" + std::to_string(i));
        if (x == nullptr) break;
        const Property* y = text::find(props, "VY" + std::to_string(i));
        const double px = parseMil(x->value);
        const double py = y != nullptr ? parseMil(y->value) : 0.0;
        bounds.add(px, py);
    }
}

} // namespace

std::vector<std::vector<Property>> readAsciiRecords(const std::vector<std::uint8_t>& bytes,
                                                    bool* wasUtf8) {
    const bool utf8 = text::isValidUtf8(bytes.data(), bytes.size());
    if (wasUtf8 != nullptr) *wasUtf8 = utf8;

    std::vector<std::vector<Property>> records;
    std::size_t start = 0;
    auto flush = [&](std::size_t end) {
        // Trim CR/LF/spaces around the record.
        std::size_t begin = start;
        while (begin < end && (bytes[begin] == '\r' || bytes[begin] == '\n' || bytes[begin] == ' ' ||
                               bytes[begin] == '\t' ||
                               bytes[begin] == static_cast<std::uint8_t>(0xEF) ||
                               bytes[begin] == static_cast<std::uint8_t>(0xBB) ||
                               bytes[begin] == static_cast<std::uint8_t>(0xBF)))
            ++begin;
        std::size_t last = end;
        while (last > begin && (bytes[last - 1] == '\r' || bytes[last - 1] == '\n' ||
                                bytes[last - 1] == ' ' || bytes[last - 1] == '\t'))
            --last;
        if (last > begin) {
            std::vector<Property> props =
                text::parseParameters(bytes.data() + begin, last - begin, utf8);
            if (!props.empty()) records.push_back(std::move(props));
        }
    };

    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] == '\r' || bytes[i] == '\n') {
            flush(i);
            start = i + 1;
        }
    }
    flush(bytes.size());
    return records;
}

void parseAsciiPcb(const std::vector<std::uint8_t>& bytes, Document& document) {
    const std::vector<std::vector<Property>> records = readAsciiRecords(bytes);
    if (records.empty()) {
        document.warnings.push_back("ASCII file contains no |RECORD=| lines.");
        return;
    }

    document.container = Container::AsciiText;
    document.formatVersion = "PCB ASCII File (Altium ASCII export)";

    std::map<int, std::string> netNames;
    std::map<int, int> componentIndexById;  // ASCII "ID" -> position in document.components
    std::map<std::string, int> recordCounts;
    std::vector<Property> boardProps;
    std::vector<const std::vector<Property>*> padRecords;

    // ---- first pass: nets, components, board header ----
    for (const auto& props : records) {
        const std::string type = valueOf(props, "RECORD");
        if (type.empty()) continue;
        ++recordCounts[type];

        if (text::equalsNoCase(type, "Net")) {
            Net net;
            net.index = parseInt(valueOf(props, "ID"), static_cast<int>(document.nets.size()));
            net.name = valueOf(props, "NAME");
            if (net.name.empty()) net.name = valueOf(props, "NETNAME");
            netNames[net.index] = net.name;
            document.nets.push_back(net);
        } else if (text::equalsNoCase(type, "Board")) {
            // Altium splits the board object over several "RECORD=Board" lines; keep them all.
            for (const auto& property : props) {
                if (text::upper(property.name) == "RECORD") continue;
                boardProps.push_back(property);
            }
        } else if (text::equalsNoCase(type, "Component")) {
            Component component;
            component.componentIndex = parseInt(valueOf(props, "ID"), -1);
            component.designator = valueOf(props, "SOURCEDESIGNATOR");
            if (component.designator.empty()) component.designator = valueOf(props, "DESIGNATOR");
            component.footprint = valueOf(props, "PATTERN");
            component.name = valueOf(props, "SOURCELIBREFERENCE");
            component.comment = valueOf(props, "COMMENT");
            component.description = valueOf(props, "DESCRIPTION");
            if (component.description.empty()) component.description = valueOf(props, "SOURCEDESCRIPTION");
            component.sourceLibrary = valueOf(props, "SOURCECOMPONENTLIBRARY");
            component.sourceFootprintLibrary = valueOf(props, "SOURCEFOOTPRINTLIBRARY");
            component.uniqueId = valueOf(props, "SOURCEUNIQUEID");
            component.layer = prettyLayer(valueOf(props, "LAYER"));
            component.x = parseMil(valueOf(props, "X"));
            component.y = parseMil(valueOf(props, "Y"));
            component.rotation = text::parseNumber(valueOf(props, "ROTATION"));
            component.height = parseMil(valueOf(props, "HEIGHT"));
            for (const auto& property : props) component.properties.push_back(property);

            componentIndexById[component.componentIndex] =
                static_cast<int>(document.components.size());
            document.components.push_back(std::move(component));
        } else if (text::equalsNoCase(type, "Pad")) {
            padRecords.push_back(&props);
        }
    }

    // Board header values (VERSION / DATE / TIME).
    if (!boardProps.empty()) {
        document.boardVersion = valueOf(boardProps, "VERSION");
        const std::string date = valueOf(boardProps, "DATE");
        const std::string time = valueOf(boardProps, "TIME");
        if (!date.empty() || !time.empty()) document.lastSaved = text::trim(date + " " + time);
        // Board outline (KINDn/VXn/VYn) gives a reliable board extent.
        addOutlineBounds(boardProps, document.bounds);
    }

    // ---- pads ----
    for (const std::vector<Property>* props : padRecords) {
        const int netId = parseInt(valueOf(*props, "NET"), -1);
        Pad pad;
        pad.designator = valueOf(*props, "NAME");
        if (pad.designator.empty()) pad.designator = valueOf(*props, "DESIGNATOR");
        pad.layer = text::pcbLayerIdFromName(valueOf(*props, "LAYER"));
        pad.layerName = prettyLayer(valueOf(*props, "LAYER"));
        pad.netIndex = netId;
        const auto netIt = netNames.find(netId);
        if (netIt != netNames.end()) pad.net = netIt->second;
        pad.x = parseMil(valueOf(*props, "X"));
        pad.y = parseMil(valueOf(*props, "Y"));
        pad.width = parseMil(valueOf(*props, "XSIZE"));
        pad.height = parseMil(valueOf(*props, "YSIZE"));
        pad.holeSize = parseMil(valueOf(*props, "HOLESIZE"));
        pad.rotation = text::parseNumber(valueOf(*props, "ROTATION"));
        pad.plated = parseBool(valueOf(*props, "PLATED"), false);

        const int ownerId = parseInt(valueOf(*props, "COMPONENT"), -1);
        pad.componentIndex = ownerId;
        const auto ownerIt = componentIndexById.find(ownerId);
        if (ownerId >= 0 && ownerIt != componentIndexById.end()) {
            Component& owner = document.components[static_cast<std::size_t>(ownerIt->second)];
            owner.pads.push_back(pad);
            ++owner.padCount;
            if (pad.holeSize > 0.0) ++owner.throughHolePads;
        }
        document.bounds.add(pad.x, pad.y);
        ++document.pcb.pads;
    }

    // ---- remaining primitives: counts and extents ----
    for (const auto& props : records) {
        const std::string type = valueOf(props, "RECORD");
        if (text::equalsNoCase(type, "Track")) {
            ++document.pcb.tracks;
            const double x1 = parseMil(valueOf(props, "X1"));
            const double y1 = parseMil(valueOf(props, "Y1"));
            const double x2 = parseMil(valueOf(props, "X2"));
            const double y2 = parseMil(valueOf(props, "Y2"));
            document.bounds.add(x1, y1);
            document.bounds.add(x2, y2);
        } else if (text::equalsNoCase(type, "Arc")) {
            ++document.pcb.arcs;
            const double cx = parseMil(valueOf(props, "LOCATION.X"));
            const double cy = parseMil(valueOf(props, "LOCATION.Y"));
            const double radius = parseMil(valueOf(props, "RADIUS"));
            document.bounds.add(cx - radius, cy - radius);
            document.bounds.add(cx + radius, cy + radius);
        } else if (text::equalsNoCase(type, "Via")) {
            ++document.pcb.vias;
            document.bounds.add(parseMil(valueOf(props, "X")), parseMil(valueOf(props, "Y")));
        } else if (text::equalsNoCase(type, "Region")) {
            ++document.pcb.regions;
            addOutlineBounds(props, document.bounds);
        } else if (text::equalsNoCase(type, "Polygon")) {
            ++document.pcb.polygons;
            addOutlineBounds(props, document.bounds);
        } else if (text::equalsNoCase(type, "Text")) {
            ++document.pcb.texts;
        } else if (text::equalsNoCase(type, "Fill")) {
            ++document.pcb.fills;
            document.bounds.add(parseMil(valueOf(props, "X1")), parseMil(valueOf(props, "Y1")));
            document.bounds.add(parseMil(valueOf(props, "X2")), parseMil(valueOf(props, "Y2")));
        }
    }

    document.pcb.nets = static_cast<int>(document.nets.size());

    // Histogram for the "document structure" panel / --inspect.
    std::vector<std::pair<std::string, int>> histogram(recordCounts.begin(), recordCounts.end());
    std::sort(histogram.begin(), histogram.end(),
              [](const std::pair<std::string, int>& a, const std::pair<std::string, int>& b) {
                  if (a.second != b.second) return a.second > b.second;
                  return a.first < b.first;
              });
    document.recordTypes = histogram;

    sortComponentPads(document);

    if (document.components.empty())
        document.warnings.push_back("No RECORD=Component entries were found in this ASCII PCB file.");
    document.warnings.push_back(
        "ASCII PCB export: Altium does not write the component value (comment) or supplier fields; "
        "open the binary .PcbDoc or the .SchDoc to see them.");
}

void parseAsciiSch(const std::vector<std::uint8_t>& bytes, Document& document) {
    const std::vector<std::vector<Property>> records = readAsciiRecords(bytes);
    document.container = Container::AsciiText;
    document.formatVersion = "Schematic ASCII File (Altium ASCII export)";
    if (records.empty()) {
        document.warnings.push_back("ASCII file contains no |RECORD=| lines.");
        return;
    }

    std::map<std::string, int> recordCounts;
    std::vector<SchRecord> schRecords;
    schRecords.reserve(records.size());

    int objectIndex = 0;
    for (std::size_t i = 0; i < records.size(); ++i) {
        const std::vector<Property>& props = records[i];
        SchRecord record;
        record.props = props;
        const std::string typeText = valueOf(props, "RECORD");
        record.type = typeText.empty() ? -1 : parseInt(typeText, -1);
        if (!typeText.empty()) ++recordCounts[typeText];

        // The first line of an ASCII schematic is the |HEADER=...| file header.
        const bool isFileHeader = (record.type < 0 && i == 0);
        record.index = isFileHeader ? -1 : objectIndex++;

        if (isFileHeader) {
            const std::string header = valueOf(props, "HEADER");
            if (!header.empty()) document.formatVersion = header;
        }
        schRecords.push_back(std::move(record));
    }

    buildSchDocument(schRecords, document);
    document.container = Container::AsciiText;

    std::vector<std::pair<std::string, int>> histogram(recordCounts.begin(), recordCounts.end());
    std::sort(histogram.begin(), histogram.end(),
              [](const std::pair<std::string, int>& a, const std::pair<std::string, int>& b) {
                  if (a.second != b.second) return a.second > b.second;
                  return a.first < b.first;
              });
    document.recordTypes = histogram;

    if (document.components.empty())
        document.warnings.push_back("No RECORD=1 (component) entries were found in this ASCII schematic.");
}

} // namespace detail
} // namespace pcbv
