#include "Export.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <vector>

namespace pcbv {
namespace {

std::string num(double value, int decimals) {
    if (!std::isfinite(value)) return "";
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(decimals) << value;
    std::string out = ss.str();
    // Strip trailing zeros but keep one decimal.
    if (out.find('.') != std::string::npos) {
        while (out.size() > 1 && out.back() == '0' && out[out.size() - 2] != '.') out.pop_back();
    }
    return out;
}

std::string escapeCsv(const std::string& value) {
    const bool needsQuotes = value.find_first_of(",\"\r\n") != std::string::npos;
    if (!needsQuotes) return value;
    std::string out = "\"";
    for (char c : value) {
        if (c == '"') out += "\"\"";
        else out += c;
    }
    out += "\"";
    return out;
}

std::string jsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (unsigned char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[(c >> 4) & 0xF];
                    out += hex[c & 0xF];
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string htmlEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

int quantity(const Document& doc) {
    return static_cast<int>(doc.components.size());
}

} // namespace

std::string formatMil(double mil) { return num(mil, 4); }
std::string formatMm(double mil) { return num(mil * 0.0254, 4); }
std::string formatQty(int value) { return std::to_string(value); }

std::string toSummaryText(const Document& doc) {
    std::ostringstream out;
    out << "File        : " << doc.fileName << "\n";
    out << "Path        : " << doc.path << "\n";
    out << "Type        : " << docTypeName(doc.type) << "\n";
    out << "Container   : " << containerName(doc.container) << "\n";
    out << "Format      : " << doc.formatVersion;
    if (!doc.boardVersion.empty()) out << "  (board format " << doc.boardVersion << ")";
    out << "\n";
    if (!doc.lastSaved.empty()) out << "Last saved  : " << doc.lastSaved << "\n";
    out << "File size   : " << (static_cast<double>(doc.fileSize) / (1024.0 * 1024.0)) << " MB ("
        << doc.fileSize << " bytes)\n";
    if (doc.container == Container::CompoundBinary) {
        out << "Streams     : " << doc.structure.size() << " root entries\n";
    } else if (!doc.recordTypes.empty()) {
        out << "Records     :";
        for (std::size_t i = 0; i < doc.recordTypes.size() && i < 8; ++i) {
            out << (i == 0 ? " " : ", ") << doc.recordTypes[i].first << "=" << doc.recordTypes[i].second;
        }
        out << "\n";
    }
    out << "\n";

    out << "Components  : " << quantity(doc) << "\n";

    if (doc.type == DocType::PcbDoc) {
        out << "Nets        : " << doc.pcb.nets << "\n";
        out << "Pads        : " << doc.pcb.pads << "\n";
        out << "Tracks      : " << doc.pcb.tracks << "\n";
        out << "Vias        : " << doc.pcb.vias << "\n";
        out << "Arcs        : " << doc.pcb.arcs << "\n";
        out << "Regions     : " << doc.pcb.regions << "\n";
        out << "Component bodies: " << doc.pcb.componentBodies << "\n";
        out << "Board outline: " << (doc.geometry.outline.size() >= 3
                                        ? std::to_string(doc.geometry.outline.size()) + " vertices"
                                        : std::string("not stored in this file"))
            << "\n";
        out << "Drawable    : " << doc.geometry.tracks.size() << " tracks, "
            << doc.geometry.pads.size() << " pads, " << doc.geometry.vias.size() << " vias, "
            << doc.geometry.arcs.size() << " arcs, " << doc.geometry.fills.size()
            << " fills, " << doc.geometry.regions.size() << " regions\n";
        if (doc.bounds.valid) {
            out << "Board extent: " << formatMm(doc.bounds.width()) << " x "
                << formatMm(doc.bounds.height()) << " mm  (" << formatMil(doc.bounds.width())
                << " x " << formatMil(doc.bounds.height()) << " mil)\n";
        }
    } else if (doc.type == DocType::SchDoc) {
        out << "Pins        : " << doc.sch.pins << "\n";
        out << "Wires       : " << doc.sch.wires << "\n";
        out << "Net labels  : " << doc.sch.netLabels << "\n";
        out << "Ports       : " << doc.sch.ports << "\n";
        out << "Power ports : " << doc.sch.powerPorts << "\n";
        out << "Junctions   : " << doc.sch.junctions << "\n";
    }

    if (!doc.warnings.empty()) {
        out << "\nWarnings:\n";
        for (const auto& w : doc.warnings) out << "  - " << w << "\n";
    }
    return out.str();
}

std::string toCsv(const Document& doc) {
    const bool pcb = doc.type == DocType::PcbDoc;
    std::ostringstream out;
    out << "Designator,Comment,Name,Footprint,Manufacturer,PartNumber,Supplier,SupplierPartNumber,"
           "Description,Layer,Pins,Pads,ThroughHolePads,X (mil),Y (mil),X (mm),Y (mm),Rotation (deg),"
           "SourceLibrary,Datasheet,UniqueId,File\r\n";

    for (const auto& c : doc.components) {
        out << escapeCsv(c.designator) << ',' << escapeCsv(c.comment) << ',' << escapeCsv(c.name) << ','
            << escapeCsv(c.footprint) << ',' << escapeCsv(c.manufacturer) << ','
            << escapeCsv(c.partNumber) << ',' << escapeCsv(c.supplier) << ','
            << escapeCsv(c.supplierPartNumber) << ',' << escapeCsv(c.description) << ','
            << escapeCsv(c.layer) << ',' << c.pinCount << ',' << c.padCount << ','
            << c.throughHolePads << ',' << formatMil(c.x) << ',' << formatMil(c.y) << ','
            << formatMm(c.x) << ',' << formatMm(c.y) << ',' << num(c.rotation, 3) << ','
            << escapeCsv(pcb ? c.sourceFootprintLibrary : c.sourceLibrary) << ','
            << escapeCsv(c.datasheet) << ',' << escapeCsv(c.uniqueId) << ','
            << escapeCsv(doc.fileName) << "\r\n";
    }
    return out.str();
}

std::string toJson(const Document& doc) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"file\": \"" << jsonEscape(doc.fileName) << "\",\n";
    out << "  \"path\": \"" << jsonEscape(doc.path) << "\",\n";
    out << "  \"type\": \"" << jsonEscape(docTypeName(doc.type)) << "\",\n";
    out << "  \"container\": \"" << jsonEscape(containerName(doc.container)) << "\",\n";
    out << "  \"format\": \"" << jsonEscape(doc.formatVersion) << "\",\n";
    out << "  \"fileSize\": " << doc.fileSize << ",\n";
    out << "  \"componentCount\": " << quantity(doc) << ",\n";

    if (doc.type == DocType::PcbDoc) {
        out << "  \"pcb\": {\"nets\": " << doc.pcb.nets << ", \"pads\": " << doc.pcb.pads
            << ", \"tracks\": " << doc.pcb.tracks << ", \"vias\": " << doc.pcb.vias
            << ", \"arcs\": " << doc.pcb.arcs << ", \"regions\": " << doc.pcb.regions << "},\n";
    } else {
        out << "  \"schematic\": {\"pins\": " << doc.sch.pins << ", \"wires\": " << doc.sch.wires
            << ", \"netLabels\": " << doc.sch.netLabels << ", \"ports\": " << doc.sch.ports << "},\n";
    }

    out << "  \"components\": [\n";
    for (std::size_t i = 0; i < doc.components.size(); ++i) {
        const Component& c = doc.components[i];
        out << "    {\n";
        out << "      \"designator\": \"" << jsonEscape(c.designator) << "\",\n";
        out << "      \"comment\": \"" << jsonEscape(c.comment) << "\",\n";
        out << "      \"name\": \"" << jsonEscape(c.name) << "\",\n";
        out << "      \"footprint\": \"" << jsonEscape(c.footprint) << "\",\n";
        out << "      \"manufacturer\": \"" << jsonEscape(c.manufacturer) << "\",\n";
        out << "      \"partNumber\": \"" << jsonEscape(c.partNumber) << "\",\n";
        out << "      \"supplier\": \"" << jsonEscape(c.supplier) << "\",\n";
        out << "      \"supplierPartNumber\": \"" << jsonEscape(c.supplierPartNumber) << "\",\n";
        out << "      \"description\": \"" << jsonEscape(c.description) << "\",\n";
        out << "      \"layer\": \"" << jsonEscape(c.layer) << "\",\n";
        out << "      \"pinCount\": " << c.pinCount << ",\n";
        out << "      \"padCount\": " << c.padCount << ",\n";
        out << "      \"throughHolePads\": " << c.throughHolePads << ",\n";
        out << "      \"xMil\": " << num(c.x, 4) << ",\n";
        out << "      \"yMil\": " << num(c.y, 4) << ",\n";
        out << "      \"rotation\": " << num(c.rotation, 4) << ",\n";
        out << "      \"sourceLibrary\": \"" << jsonEscape(c.sourceLibrary) << "\",\n";
        out << "      \"datasheet\": \"" << jsonEscape(c.datasheet) << "\",\n";
        out << "      \"uniqueId\": \"" << jsonEscape(c.uniqueId) << "\",\n";

        out << "      \"parameters\": [";
        for (std::size_t p = 0; p < c.properties.size(); ++p) {
            if (p > 0) out << ", ";
            out << "{\"name\": \"" << jsonEscape(c.properties[p].name) << "\", \"value\": \""
                << jsonEscape(c.properties[p].value) << "\"}";
        }
        out << "],\n";

        out << "      \"pins\": [";
        for (std::size_t p = 0; p < c.pins.size(); ++p) {
            const Pin& pin = c.pins[p];
            if (p > 0) out << ", ";
            out << "{\"name\": \"" << jsonEscape(pin.name) << "\", \"designator\": \""
                << jsonEscape(pin.designator) << "\", \"electrical\": \"" << jsonEscape(pin.electrical)
                << "\", \"part\": " << pin.ownerPart << "}";
        }
        out << "],\n";

        out << "      \"pads\": [";
        for (std::size_t p = 0; p < c.pads.size(); ++p) {
            const Pad& pad = c.pads[p];
            if (p > 0) out << ", ";
            out << "{\"designator\": \"" << jsonEscape(pad.designator) << "\", \"net\": \""
                << jsonEscape(pad.net) << "\", \"layer\": \"" << jsonEscape(pad.layerName) << "\", \"xMil\": "
                << num(pad.x, 4) << ", \"yMil\": " << num(pad.y, 4) << ", \"widthMil\": " << num(pad.width, 4)
                << ", \"heightMil\": " << num(pad.height, 4) << ", \"holeMil\": " << num(pad.holeSize, 4)
                << ", \"rotation\": " << num(pad.rotation, 3) << ", \"plated\": "
                << (pad.plated ? "true" : "false") << "}";
        }
        out << "]\n";
        out << "    }" << (i + 1 < doc.components.size() ? ",\n" : "\n");
    }
    out << "  ],\n";

    out << "  \"nets\": [";
    for (std::size_t i = 0; i < doc.nets.size(); ++i) {
        if (i > 0) out << ", ";
        out << "\"" << jsonEscape(doc.nets[i].name) << "\"";
    }
    out << "]\n";
    out << "}\n";
    return out.str();
}

std::string toHtml(const Document& doc) {
    std::ostringstream out;
    const bool pcb = doc.type == DocType::PcbDoc;
    out << "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n";
    out << "<title>PCB Viewer — " << htmlEscape(doc.fileName) << "</title>\n";
    out << "<style>\n"
           "body{font-family:Segoe UI,Roboto,DejaVu Sans,sans-serif;background:#16181d;color:#e6e6e6;margin:0;padding:24px}\n"
           "h1{font-size:22px;margin:0 0 4px}h2{font-size:15px;text-transform:uppercase;letter-spacing:.08em;color:#8ab4f8;margin:28px 0 10px}\n"
           ".meta{color:#9aa0a6;font-size:13px;margin-bottom:18px}\n"
           ".cards{display:flex;flex-wrap:wrap;gap:10px;margin-bottom:8px}\n"
           ".card{background:#1f232b;border:1px solid #2c313a;border-radius:8px;padding:10px 14px;min-width:110px}\n"
           ".card .v{font-size:20px;font-weight:600}.card .k{font-size:11px;color:#9aa0a6;text-transform:uppercase;letter-spacing:.06em}\n"
           "table{width:100%;border-collapse:collapse;font-size:12.5px;background:#1b1f26;border-radius:8px;overflow:hidden}\n"
           "th{background:#242a33;text-align:left;padding:8px;font-weight:600;color:#cfd8dc;position:sticky;top:0}\n"
           "td{padding:7px 8px;border-top:1px solid #262b34;vertical-align:top}\n"
           "tr:hover td{background:#222833}\n"
           ".mono{font-family:ui-monospace,Consolas,monospace;color:#b0bec5}\n"
           "footer{margin-top:26px;color:#6b7280;font-size:11px}\n"
           "</style>\n</head>\n<body>\n";
    out << "<h1>" << htmlEscape(doc.fileName) << "</h1>\n";
    out << "<div class=\"meta\">" << htmlEscape(docTypeName(doc.type)) << " &middot; "
        << htmlEscape(containerName(doc.container)) << " &middot; " << htmlEscape(doc.formatVersion)
        << " &middot; " << doc.components.size() << " components</div>\n";

    out << "<div class=\"cards\">\n";
    auto card = [&out](const std::string& key, const std::string& value) {
        out << "<div class=\"card\"><div class=\"v\">" << htmlEscape(value) << "</div><div class=\"k\">"
            << htmlEscape(key) << "</div></div>\n";
    };
    card("Components", std::to_string(doc.components.size()));
    if (pcb) {
        card("Nets", std::to_string(doc.pcb.nets));
        card("Pads", std::to_string(doc.pcb.pads));
        card("Tracks", std::to_string(doc.pcb.tracks));
        card("Vias", std::to_string(doc.pcb.vias));
        if (doc.bounds.valid) card("Board (mm)", formatMm(doc.bounds.width()) + " x " + formatMm(doc.bounds.height()));
    } else {
        card("Pins", std::to_string(doc.sch.pins));
        card("Wires", std::to_string(doc.sch.wires));
        card("Net labels", std::to_string(doc.sch.netLabels));
    }
    out << "</div>\n";

    out << "<h2>Component list</h2>\n<table>\n<thead><tr>"
           "<th>#</th><th>Designator</th><th>Comment / Value</th><th>Name</th><th>Footprint</th>"
           "<th>Manufacturer</th><th>Part number</th><th>Layer</th><th>Pins</th><th>Pads</th>"
           "<th>Position (mm)</th></tr></thead>\n<tbody>\n";
    int row = 1;
    for (const auto& c : doc.components) {
        out << "<tr><td class=\"mono\">" << row++ << "</td><td><b>" << htmlEscape(c.designator)
            << "</b></td><td>" << htmlEscape(c.comment) << "</td><td>" << htmlEscape(c.name)
            << "</td><td>" << htmlEscape(c.footprint) << "</td><td>" << htmlEscape(c.manufacturer)
            << "</td><td class=\"mono\">" << htmlEscape(c.partNumber) << "</td><td>" << htmlEscape(c.layer)
            << "</td><td class=\"mono\">" << (c.pinCount ? std::to_string(c.pinCount) : "")
            << "</td><td class=\"mono\">" << (c.padCount ? std::to_string(c.padCount) : "")
            << "</td><td class=\"mono\">" << formatMm(c.x) << ", " << formatMm(c.y) << "</td></tr>\n";
    }
    out << "</tbody>\n</table>\n";

    out << "<footer>Generated by PCB Viewer (pcbv) &middot; Altium .PcbDoc / .SchDoc reader</footer>\n";
    out << "</body>\n</html>\n";
    return out.str();
}

} // namespace pcbv
