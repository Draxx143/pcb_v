// Bundle.cpp — reader / writer for the ".pcbvlib" text bundle (see Bundle.h).
#include "Bundle.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <map>
#include <sstream>

namespace pcbv {
namespace {

constexpr const char* kHeader = "|PCBV-LIB|";

/// `|` and a newline cannot appear inside a value, so they are percent-escaped.
std::string escapeValue(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c == '|') out += "%7C";
        else if (c == '\n' || c == '\r') out += ' ';
        else out += c;
    }
    return out;
}

std::string unescapeValue(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && text.compare(i + 1, 2, "7C") == 0) {
            out += '|';
            i += 2;
        } else {
            out += text[i];
        }
    }
    return out;
}

/// A length such as "10mil", "2.54mm" or "10" (mil).
double parseLength(const std::string& text) {
    if (text.empty()) return 0.0;
    const bool mm = text.size() > 2 && text.compare(text.size() - 2, 2, "mm") == 0;
    const bool mil = text.size() > 3 && text.compare(text.size() - 3, 3, "mil") == 0;
    const std::size_t end = mm ? text.size() - 2 : (mil ? text.size() - 3 : text.size());
    std::string number = text.substr(0, end);
    const double value = std::atof(number.c_str());
    return mm ? value / 0.0254 : value;
}

struct Record {
    std::string name;
    std::map<std::string, std::string> fields;

    std::string text(const std::string& key) const {
        const auto it = fields.find(key);
        return it == fields.end() ? std::string() : unescapeValue(it->second);
    }
    double number(const std::string& key) const { return parseLength(text(key)); }
    int integer(const std::string& key, int fallback = 0) const {
        const auto it = fields.find(key);
        if (it == fields.end() || it->second.empty()) return fallback;
        return std::atoi(it->second.c_str());
    }
    bool has(const std::string& key) const { return fields.find(key) != fields.end(); }
};

bool parseRecord(const std::string& line, Record& record) {
    if (line.size() < 3 || line.front() != '|') return false;
    record = Record();
    std::size_t pos = 1;
    while (pos < line.size()) {
        const std::size_t end = line.find('|', pos);
        const std::string chunk =
            line.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (!chunk.empty()) {
            const std::size_t equals = chunk.find('=');
            if (equals == std::string::npos) record.name = chunk;   // the bare token names it
            else record.fields[chunk.substr(0, equals)] = chunk.substr(equals + 1);
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return !record.name.empty();
}

SymbolShape::Kind shapeKind(const std::string& text) {
    if (text == "POLYLINE") return SymbolShape::Kind::Polyline;
    if (text == "RECT" || text == "RECTANGLE") return SymbolShape::Kind::Rectangle;
    if (text == "ROUNDRECT" || text == "ROUND_RECT") return SymbolShape::Kind::RoundRectangle;
    if (text == "ARC") return SymbolShape::Kind::Arc;
    if (text == "ELLIPSE") return SymbolShape::Kind::Ellipse;
    if (text == "TEXT") return SymbolShape::Kind::Text;
    return SymbolShape::Kind::Line;
}

std::string shapeKindName(SymbolShape::Kind kind) {
    switch (kind) {
        case SymbolShape::Kind::Polyline: return "POLYLINE";
        case SymbolShape::Kind::Rectangle: return "RECT";
        case SymbolShape::Kind::RoundRectangle: return "ROUNDRECT";
        case SymbolShape::Kind::Arc: return "ARC";
        case SymbolShape::Kind::Ellipse: return "ELLIPSE";
        case SymbolShape::Kind::Text: return "TEXT";
        default: return "LINE";
    }
}

/// "x1,y1;x2,y2" → points, in mil.
std::vector<Point> parsePoints(const std::string& text) {
    std::vector<Point> points;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t end = text.find(';', pos);
        const std::string pair =
            text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        const std::size_t comma = pair.find(',');
        if (comma != std::string::npos) {
            Point point;
            point.x = parseLength(pair.substr(0, comma));
            point.y = parseLength(pair.substr(comma + 1));
            points.push_back(point);
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return points;
}

std::string formatPoints(const std::vector<Point>& points) {
    std::ostringstream out;
    out << std::setprecision(10);
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (i > 0) out << ';';
        out << points[i].x << ',' << points[i].y;
    }
    return out.str();
}

std::string mil(double value) {
    std::ostringstream out;
    out << std::setprecision(10) << value << "mil";
    return out.str();
}

void extend(Bounds& bounds, double x, double y) { bounds.add(x, y); }

void computeBounds(LibraryFootprint& footprint) {
    footprint.bounds = Bounds();
    for (const Pad& pad : footprint.geometry.pads) {
        const double halfW = pad.width / 2.0;
        const double halfH = pad.height / 2.0;
        extend(footprint.bounds, pad.x - halfW, pad.y - halfH);
        extend(footprint.bounds, pad.x + halfW, pad.y + halfH);
    }
    for (const TrackFeature& track : footprint.geometry.tracks) {
        const double half = track.width / 2.0;
        extend(footprint.bounds, track.x1 - half, track.y1 - half);
        extend(footprint.bounds, track.x1 + half, track.y1 + half);
        extend(footprint.bounds, track.x2 - half, track.y2 - half);
        extend(footprint.bounds, track.x2 + half, track.y2 + half);
    }
    for (const ArcFeature& arc : footprint.geometry.arcs) {
        extend(footprint.bounds, arc.cx - arc.radius, arc.cy - arc.radius);
        extend(footprint.bounds, arc.cx + arc.radius, arc.cy + arc.radius);
    }
    for (const RegionFeature& region : footprint.geometry.regions) {
        for (const Point& point : region.outline) extend(footprint.bounds, point.x, point.y);
    }
    for (const FillFeature& fill : footprint.geometry.fills) {
        extend(footprint.bounds, std::min(fill.x1, fill.x2), std::min(fill.y1, fill.y2));
        extend(footprint.bounds, std::max(fill.x1, fill.x2), std::max(fill.y1, fill.y2));
    }
    for (const ViaFeature& via : footprint.geometry.vias) {
        extend(footprint.bounds, via.x - via.diameter / 2.0, via.y - via.diameter / 2.0);
        extend(footprint.bounds, via.x + via.diameter / 2.0, via.y + via.diameter / 2.0);
    }
}

void countGeometry(LibraryFootprint& footprint) {
    footprint.padCount = static_cast<int>(footprint.geometry.pads.size());
    footprint.trackCount = static_cast<int>(footprint.geometry.tracks.size());
    footprint.arcCount = static_cast<int>(footprint.geometry.arcs.size());
    footprint.viaCount = static_cast<int>(footprint.geometry.vias.size());
    footprint.fillCount = static_cast<int>(footprint.geometry.fills.size());
    footprint.regionCount = static_cast<int>(footprint.geometry.regions.size());
    footprint.textCount = static_cast<int>(footprint.geometry.texts.size());
    footprint.bodyCount = static_cast<int>(footprint.models.size());
}

/// One part while it is being read, before it becomes a footprint or a symbol.
struct PartBuild {
    std::string name;
    std::string description;
    std::string prefix;
    int subParts = 1;
    std::vector<Property> parameters;
    PcbGeometry geometry;
    std::vector<EmbeddedModel> models;
    std::vector<Pin> pins;
    std::vector<SymbolPin> pinGeometry;
    std::vector<SymbolShape> shapes;
};

} // namespace

bool looksLikeBundle(const std::vector<std::uint8_t>& head) {
    if (head.size() < 10) return false;
    return std::string(head.begin(), head.begin() + 10) == kHeader;
}

bool readBundle(const std::string& text, Library& library, std::string& error) {
    if (text.compare(0, 10, kHeader) != 0) {
        error = "This is not a PCB Viewer library bundle.";
        return false;
    }

    library = Library();
    library.container = Container::AsciiText;
    library.formatVersion = "PCB Viewer library bundle v1";

    std::string type = "PCB";
    std::vector<PartBuild> parts;

    auto partAt = [&parts](std::size_t index) -> PartBuild& {
        while (parts.size() <= index) parts.push_back(PartBuild());
        return parts[index];
    };

    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t lineEnd = text.find('\n', pos);
        const std::string line =
            text.substr(pos, lineEnd == std::string::npos ? std::string::npos : lineEnd - pos);
        std::size_t next = lineEnd == std::string::npos ? text.size() : lineEnd + 1;

        Record record;
        if (!parseRecord(line, record)) {
            pos = next;
            continue;
        }

        if (record.name == "PCBV-LIB") {
            if (!record.text("TYPE").empty()) type = record.text("TYPE");
            library.fileName = record.text("NAME");
            pos = next;
            continue;
        }
        if (record.name == "ENDSTEP") {
            pos = next;
            continue;
        }

        // A PART record opens the next part; every other record names its part explicitly.
        const std::size_t index =
            record.name == "PART"
                ? parts.size()
                : (record.has("PART")
                       ? static_cast<std::size_t>(std::max(0, record.integer("PART")))
                       : 0);
        PartBuild& part = partAt(index);

        if (record.name == "PART") {
            part.name = record.text("NAME");
            part.description = record.text("DESCRIPTION");
            part.prefix = record.text("PREFIX");
            part.subParts = std::max(1, record.integer("PARTS", 1));
            for (const char* key : {"MANUFACTURER", "MPN", "DATASHEET", "VALUE", "CATEGORY",
                                    "LICENSE", "CREDIT"}) {
                const std::string value = record.text(key);
                if (!value.empty()) part.parameters.push_back(Property{key, value});
            }
        } else if (record.name == "PAD") {
            Pad pad;
            pad.designator = record.text("NAME");
            pad.x = record.number("X");
            pad.y = record.number("Y");
            pad.width = record.number("XSIZE");
            pad.height = record.number("YSIZE");
            pad.holeSize = record.number("HOLE");
            pad.rotation = record.number("ROTATION");
            pad.layer = record.integer("LAYER", 1);
            pad.plated = pad.holeSize > 0.0;
            pad.componentIndex = static_cast<int>(index);
            part.geometry.pads.push_back(pad);
        } else if (record.name == "TRACK") {
            TrackFeature track;
            track.x1 = record.number("X1");
            track.y1 = record.number("Y1");
            track.x2 = record.number("X2");
            track.y2 = record.number("Y2");
            track.width = record.number("WIDTH");
            track.layer = record.integer("LAYER", 33);
            part.geometry.tracks.push_back(track);
        } else if (record.name == "ARC") {
            ArcFeature arc;
            arc.cx = record.number("X");
            arc.cy = record.number("Y");
            arc.radius = record.number("RADIUS");
            arc.startAngle = record.number("START");
            arc.endAngle = record.number("END");
            arc.width = record.number("WIDTH");
            arc.layer = record.integer("LAYER", 33);
            part.geometry.arcs.push_back(arc);
        } else if (record.name == "REGION") {
            RegionFeature region;
            region.outline = parsePoints(record.text("POINTS"));
            region.layer = record.integer("LAYER", 33);
            if (region.outline.size() >= 3) part.geometry.regions.push_back(region);
        } else if (record.name == "FILL") {
            FillFeature fill;
            fill.x1 = record.number("X1");
            fill.y1 = record.number("Y1");
            fill.x2 = record.number("X2");
            fill.y2 = record.number("Y2");
            fill.rotation = record.number("ROTATION");
            fill.layer = record.integer("LAYER", 33);
            part.geometry.fills.push_back(fill);
        } else if (record.name == "VIA") {
            ViaFeature via;
            via.x = record.number("X");
            via.y = record.number("Y");
            via.diameter = record.number("DIAMETER");
            via.hole = record.number("HOLE");
            part.geometry.vias.push_back(via);
        } else if (record.name == "PIN") {
            SymbolPin pin;
            pin.designator = record.text("NUMBER");
            pin.name = record.text("NAME");
            pin.electrical = record.text("ELECTRICAL");
            pin.x = record.number("X");
            pin.y = record.number("Y");
            pin.length = record.number("LENGTH");
            pin.rotation = record.integer("ROTATION");
            pin.ownerPart = record.integer("PARTNUM", 1);
            pin.hidden = record.integer("HIDDEN") != 0;
            part.pinGeometry.push_back(pin);

            Pin simple;
            simple.name = pin.name;
            simple.designator = pin.designator;
            simple.electrical = pin.electrical;
            simple.ownerPart = pin.ownerPart;
            part.pins.push_back(simple);
        } else if (record.name == "SHAPE") {
            SymbolShape shape;
            shape.kind = shapeKind(record.text("KIND"));
            shape.points = parsePoints(record.text("POINTS"));
            shape.radius = record.number("RADIUS");
            shape.secondaryRadius = record.number("RADIUS2");
            shape.startAngle = record.number("START");
            shape.endAngle = record.number("END");
            shape.width = record.number("WIDTH");
            shape.rotation = record.number("ROTATION");
            shape.fill = record.integer("FILL");
            shape.text = record.text("TEXT");
            shape.textHeight = record.number("HEIGHT");
            shape.ownerPart = record.integer("PARTNUM", 0);
            part.shapes.push_back(shape);
        } else if (record.name == "STEP") {
            const std::size_t bytes = static_cast<std::size_t>(std::max(0, record.integer("BYTES")));
            EmbeddedModel model;
            model.name = record.text("NAME");
            model.rotationX = record.number("ROTX");
            model.rotationY = record.number("ROTY");
            model.rotationZ = record.number("ROTZ");
            model.dz = record.number("DZ");
            model.dx = record.number("DX");
            model.dy = record.number("DY");
            model.hasPlacement = record.integer("PLACED", 1) != 0;
            if (next + bytes <= text.size()) {
                model.stepText = text.substr(next, bytes);
                part.models.push_back(model);
                next += bytes;
                while (next < text.size() && (text[next] == '\r' || text[next] == '\n')) ++next;
            } else {
                library.warnings.push_back("Model '" + model.name + "' is truncated.");
            }
        }
        pos = next;
    }

    const bool schematic = type == "SCH";
    library.type = schematic ? DocType::SchLib : DocType::PcbLib;

    for (PartBuild& part : parts) {
        if (part.name.empty() && part.geometry.pads.empty() && part.shapes.empty() &&
            part.pinGeometry.empty()) {
            continue;   // an empty placeholder, not a part
        }
        if (schematic) {
            LibrarySymbol symbol;
            symbol.name = part.name;
            symbol.description = part.description;
            symbol.designatorPrefix = part.prefix;
            symbol.partCount = part.subParts;
            symbol.pins = part.pins;
            symbol.pinGeometry = part.pinGeometry;
            symbol.shapes = part.shapes;
            symbol.parameters = part.parameters;
            library.symbols.push_back(std::move(symbol));
        } else {
            LibraryFootprint footprint;
            footprint.name = part.name;
            footprint.description = part.description;
            footprint.geometry = part.geometry;
            footprint.parameters = part.parameters;
            footprint.models = part.models;
            computeBounds(footprint);
            countGeometry(footprint);
            library.footprints.push_back(std::move(footprint));
        }
    }

    if (library.itemCount() == 0) {
        error = "The bundle has no parts.";
        return false;
    }
    return true;
}

std::string writeBundle(const Library& library) {
    const bool schematic = library.type == DocType::SchLib;
    std::ostringstream out;
    out << "|PCBV-LIB|VERSION=1|TYPE=" << (schematic ? "SCH" : "PCB") << "|NAME="
        << escapeValue(library.fileName) << "|\n";

    auto writeParameters = [&out](const std::vector<Property>& parameters) {
        for (const Property& property : parameters) {
            if (property.name == "MANUFACTURER" || property.name == "MPN" ||
                property.name == "DATASHEET" || property.name == "VALUE" ||
                property.name == "CATEGORY" || property.name == "LICENSE" ||
                property.name == "CREDIT") {
                out << '|' << property.name << '=' << escapeValue(property.value);
            }
        }
    };

    if (schematic) {
        for (std::size_t index = 0; index < library.symbols.size(); ++index) {
            const LibrarySymbol& symbol = library.symbols[index];
            out << "\n|PART|NAME=" << escapeValue(symbol.name)
                << "|DESCRIPTION=" << escapeValue(symbol.description)
                << "|PREFIX=" << escapeValue(symbol.designatorPrefix)
                << "|PARTS=" << symbol.partCount;
            writeParameters(symbol.parameters);
            out << "|\n";
            for (const SymbolPin& pin : symbol.pinGeometry) {
                out << "|PIN|PART=" << index << "|NUMBER=" << escapeValue(pin.designator)
                    << "|NAME=" << escapeValue(pin.name) << "|X=" << mil(pin.x)
                    << "|Y=" << mil(pin.y) << "|LENGTH=" << mil(pin.length)
                    << "|ROTATION=" << pin.rotation << "|ELECTRICAL=" << escapeValue(pin.electrical)
                    << "|PARTNUM=" << pin.ownerPart << "|\n";
            }
            for (const SymbolShape& shape : symbol.shapes) {
                out << "|SHAPE|PART=" << index << "|KIND=" << shapeKindName(shape.kind)
                    << "|POINTS=" << formatPoints(shape.points);
                if (shape.radius > 0.0) out << "|RADIUS=" << mil(shape.radius);
                if (shape.secondaryRadius > 0.0) out << "|RADIUS2=" << mil(shape.secondaryRadius);
                if (shape.kind == SymbolShape::Kind::Arc) {
                    out << "|START=" << shape.startAngle << "|END=" << shape.endAngle;
                }
                if (shape.width > 0.0) out << "|WIDTH=" << mil(shape.width);
                if (shape.fill != 0) out << "|FILL=" << shape.fill;
                if (!shape.text.empty()) out << "|TEXT=" << escapeValue(shape.text);
                if (shape.textHeight > 0.0) out << "|HEIGHT=" << mil(shape.textHeight);
                if (shape.ownerPart > 0) out << "|PARTNUM=" << shape.ownerPart;
                out << "|\n";
            }
        }
    } else {
        for (std::size_t index = 0; index < library.footprints.size(); ++index) {
            const LibraryFootprint& footprint = library.footprints[index];
            out << "\n|PART|NAME=" << escapeValue(footprint.name)
                << "|DESCRIPTION=" << escapeValue(footprint.description);
            writeParameters(footprint.parameters);
            out << "|\n";

            for (const Pad& pad : footprint.geometry.pads) {
                out << "|PAD|PART=" << index << "|NAME=" << escapeValue(pad.designator)
                    << "|X=" << mil(pad.x) << "|Y=" << mil(pad.y) << "|XSIZE=" << mil(pad.width)
                    << "|YSIZE=" << mil(pad.height) << "|LAYER=" << pad.layer
                    << "|ROTATION=" << pad.rotation << "|HOLE=" << mil(pad.holeSize) << "|\n";
            }
            for (const TrackFeature& track : footprint.geometry.tracks) {
                out << "|TRACK|PART=" << index << "|LAYER=" << track.layer
                    << "|X1=" << mil(track.x1) << "|Y1=" << mil(track.y1) << "|X2=" << mil(track.x2)
                    << "|Y2=" << mil(track.y2) << "|WIDTH=" << mil(track.width) << "|\n";
            }
            for (const ArcFeature& arc : footprint.geometry.arcs) {
                out << "|ARC|PART=" << index << "|LAYER=" << arc.layer << "|X=" << mil(arc.cx)
                    << "|Y=" << mil(arc.cy) << "|RADIUS=" << mil(arc.radius)
                    << "|START=" << arc.startAngle << "|END=" << arc.endAngle
                    << "|WIDTH=" << mil(arc.width) << "|\n";
            }
            for (const RegionFeature& region : footprint.geometry.regions) {
                out << "|REGION|PART=" << index << "|LAYER=" << region.layer
                    << "|POINTS=" << formatPoints(region.outline) << "|\n";
            }
            for (const FillFeature& fill : footprint.geometry.fills) {
                out << "|FILL|PART=" << index << "|LAYER=" << fill.layer << "|X1=" << mil(fill.x1)
                    << "|Y1=" << mil(fill.y1) << "|X2=" << mil(fill.x2) << "|Y2=" << mil(fill.y2)
                    << "|ROTATION=" << fill.rotation << "|\n";
            }
            for (const EmbeddedModel& model : footprint.models) {
                out << "|STEP|PART=" << index << "|NAME=" << escapeValue(model.name)
                    << "|DX=" << mil(model.dx) << "|DY=" << mil(model.dy)
                    << "|DZ=" << mil(model.dz) << "|ROTX=" << model.rotationX
                    << "|ROTY=" << model.rotationY << "|ROTZ=" << model.rotationZ
                    << "|PLACED=" << (model.hasPlacement ? 1 : 0)
                    << "|BYTES=" << model.stepText.size() << "|\n";
                out << model.stepText;
                if (!model.stepText.empty() && model.stepText.back() != '\n') out << '\n';
            }
        }
    }
    return out.str();
}

} // namespace pcbv
