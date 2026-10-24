// KiCadLibrary.cpp — see KiCadLibrary.h.
#include "KiCadLibrary.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "SExpr.h"

namespace pcbv {
namespace {

constexpr double kMmToMil = 1.0 / 0.0254;

/// KiCad layer names -> the viewer's Altium layer ids (1 = top copper, 32 = bottom copper).
int layerId(const std::string& name) {
    if (name == "F.Cu") return 1;
    if (name == "In1.Cu") return 2;
    if (name == "In2.Cu") return 3;
    if (name == "B.Cu") return 32;
    if (name == "F.SilkS") return 21;     // top overlay
    if (name == "B.SilkS") return 22;     // bottom overlay
    if (name == "F.Paste") return 37;
    if (name == "B.Paste") return 38;
    if (name == "F.Mask") return 39;
    if (name == "B.Mask") return 40;
    if (name == "F.Fab") return 51;
    if (name == "B.Fab") return 52;
    if (name == "F.CrtYd") return 41;
    if (name == "B.CrtYd") return 42;
    if (name == "Edge.Cuts") return 56;   // board outline
    if (name == "Dwgs.User") return 44;
    if (name == "Cmts.User") return 45;
    if (name == "Eco1.User") return 46;
    if (name == "Eco2.User") return 47;
    return 51;                            // documentation by default
}

/// The first layer of a graphic item, as an Altium id.
int firstLayer(const sexpr::Node& item) {
    const sexpr::Node* layers = item.child("layers");
    if (layers != nullptr) {
        for (std::size_t i = 1; i < layers->children.size(); ++i) {
            if (!layers->children[i].isList) return layerId(layers->children[i].atom);
        }
        return 0;
    }
    const sexpr::Node* layer = item.child("layer");
    if (layer != nullptr && layer->children.size() > 1) return layerId(layer->children[1].atom);
    return 0;
}

/// `(stroke (width 0.12) (type solid))` or the older `(width 0.12)`.
double strokeWidth(const sexpr::Node& item) {
    if (const sexpr::Node* stroke = item.child("stroke")) {
        const double width = stroke->number("width", 0, 0.0);
        if (width > 0.0) return width * kMmToMil;
    }
    const double width = item.number("width", 0, 0.0);
    return width > 0.0 ? width * kMmToMil : 0.0;
}

/// A point of an `(xy …)` / `(start …)` / `(at …)` list, y flipped and converted to mil.
bool pointOf(const sexpr::Node* node, double& x, double& y) {
    if (node == nullptr || node->children.size() < 3) return false;
    const double px = node->children[1].asNumber(0.0);
    const double py = node->children[2].asNumber(0.0);
    x = px * kMmToMil;
    y = -py * kMmToMil;
    return true;
}

void grow(Bounds& bounds, double x, double y) { bounds.add(x, y); }

/// `(pts (xy …) (xy …) …)` -> polygon points (y flipped).
std::vector<Point> polygonOf(const sexpr::Node* pts) {
    std::vector<Point> out;
    if (pts == nullptr) return out;
    for (const sexpr::Node& child : pts->children) {
        if (!child.isList) continue;
        double x = 0.0, y = 0.0;
        if (pointOf(&child, x, y)) out.push_back(Point{x, y});
    }
    return out;
}

/// A circle of the footprint becomes a closed polygon so every existing renderer can draw it.
void addCirclePolygon(std::vector<Point>& out, double cx, double cy, double radius,
                      int steps = 32) {
    for (int i = 0; i < steps; ++i) {
        const double angle = 2.0 * 3.14159265358979323846 * static_cast<double>(i) / steps;
        out.push_back(Point{cx + std::cos(angle) * radius, cy + std::sin(angle) * radius});
    }
}

/// Arc of a footprint: KiCad gives start/mid/end in recent versions and start/end/angle in
/// older ones. Both are reduced to the (centre, radius, start, end) form the viewer uses.
bool arcGeometry(const sexpr::Node& item, double& cx, double& cy, double& radius,
                 double& startAngle, double& endAngle) {
    double sx = 0.0, sy = 0.0, ex = 0.0, ey = 0.0, mx = 0.0, my = 0.0;
    if (!pointOf(item.child("start"), sx, sy)) return false;
    if (!pointOf(item.child("end"), ex, ey)) return false;
    if (!pointOf(item.child("mid"), mx, my)) {
        // Old form: (start …) (end …) (angle …). Treat start as the arc start and end as
        // the arc end with the centre taken from the angle: fall back to a straight line
        // for anything we cannot solve, so a footprint never renders garbage.
        return false;
    }
    // Circumcentre of the three points.
    const double d = 2.0 * (sx * (my - ey) + mx * (ey - sy) + ex * (sy - my));
    if (std::fabs(d) < 1e-12) return false;
    cx = ((sx * sx + sy * sy) * (my - ey) + (mx * mx + my * my) * (ey - sy) +
          (ex * ex + ey * ey) * (sy - my)) / d;
    cy = ((sx * sx + sy * sy) * (ex - mx) + (mx * mx + my * my) * (sx - ex) +
          (ex * ex + ey * ey) * (mx - sx)) / d;
    radius = std::hypot(sx - cx, sy - cy);
    startAngle = std::atan2(sy - cy, sx - cx) * 180.0 / 3.14159265358979323846;
    endAngle = std::atan2(ey - cy, ex - cx) * 180.0 / 3.14159265358979323846;
    // Stay on the side of the arc that contains the mid point.
    double midAngle = std::atan2(my - cy, mx - cx) * 180.0 / 3.14159265358979323846;
    while (startAngle < 0.0) { startAngle += 360.0; endAngle += 360.0; }
    while (endAngle <= startAngle) endAngle += 360.0;
    if (midAngle < 0.0) midAngle += 360.0;
    if (midAngle < startAngle || midAngle > endAngle) {   // went the other way round
        std::swap(startAngle, endAngle);
        while (endAngle <= startAngle) endAngle += 360.0;
    }
    return true;
}

/// Reads one `(pad …)` of a footprint.
void readPad(const sexpr::Node& node, LibraryFootprint& footprint) {
    pcbv::Pad pad;
    pad.designator = node.first();
    // Type: smd / thru_hole / np_thru_hole / connect. The type is the second bare atom.
    std::string type;
    for (std::size_t i = 1; i < node.children.size(); ++i) {
        const sexpr::Node& child = node.children[i];
        if (child.isList) continue;
        if (i == 1) continue;               // the designator
        type = child.atom;
        break;
    }
    const double atX = node.number("at", 0, 0.0) * kMmToMil;
    const double atY = -node.number("at", 1, 0.0) * kMmToMil;
    pad.x = atX;
    pad.y = atY;
    pad.rotation = node.number("at", 2, 0.0);
    pad.width = node.number("size", 0, 0.0) * kMmToMil;
    pad.height = node.number("size", 1, pad.width / kMmToMil) * kMmToMil;

    if (const sexpr::Node* drill = node.child("drill")) {
        // (drill 0.8) / (drill oval 0.8 1.2) / (drill offset 0 0 0.8)
        double drillSize = 0.0;
        for (std::size_t i = 1; i < drill->children.size(); ++i) {
            const sexpr::Node& child = drill->children[i];
            if (child.isList) continue;
            const double value = child.asNumber(-1.0);
            if (value > 0.0) drillSize = std::max(drillSize, value);
        }
        pad.holeSize = drillSize * kMmToMil;
    }
    if (type == "thru_hole") {
        pad.plated = true;
    } else if (type == "np_thru_hole") {
        pad.plated = false;
    } else {
        pad.plated = false;
    }
    pad.layer = firstLayer(node);
    if (pad.layer == 0) pad.layer = (type == "thru_hole" || type == "np_thru_hole") ? 32 : 1;
    // A through-hole pad exists on every copper layer; layer 1 (top) is where it is drawn.
    if (type == "thru_hole" || type == "np_thru_hole") pad.layer = 1;

    footprint.geometry.pads.push_back(pad);
    if (pad.designator.compare(0, 1, "1") == 0 || footprint.padCount == 0) {
        // nothing to do: padCount is recomputed from the pads afterwards
    }
    grow(footprint.bounds, pad.x - pad.width / 2.0, pad.y - pad.height / 2.0);
    grow(footprint.bounds, pad.x + pad.width / 2.0, pad.y + pad.height / 2.0);
}

/// Reads the silkscreen / courtyard / fabrication graphics of a footprint.
void readGraphic(const sexpr::Node& node, LibraryFootprint& footprint) {
    const std::string& kind = node.head();
    const int layer = firstLayer(node);
    const double width = strokeWidth(node);

    if (kind == "fp_line" || kind == "fp_rect") {
        double x1 = 0.0, y1 = 0.0, x2 = 0.0, y2 = 0.0;
        if (!pointOf(node.child("start"), x1, y1)) return;
        if (!pointOf(node.child("end"), x2, y2)) return;
        if (kind == "fp_rect") {
            const double corners[4][2] = {{x1, y1}, {x2, y1}, {x2, y2}, {x1, y2}};
            for (int i = 0; i < 4; ++i) {
                const int next = (i + 1) % 4;
                TrackFeature track;
                track.x1 = corners[i][0];
                track.y1 = corners[i][1];
                track.x2 = corners[next][0];
                track.y2 = corners[next][1];
                track.width = width;
                track.layer = layer;
                footprint.geometry.tracks.push_back(track);
                grow(footprint.bounds, track.x1, track.y1);
                grow(footprint.bounds, track.x2, track.y2);
            }
            return;
        }
        TrackFeature track;
        track.x1 = x1;
        track.y1 = y1;
        track.x2 = x2;
        track.y2 = y2;
        track.width = width;
        track.layer = layer;
        footprint.geometry.tracks.push_back(track);
        grow(footprint.bounds, x1, y1);
        grow(footprint.bounds, x2, y2);
        return;
    }
    if (kind == "fp_arc" || kind == "fp_circle") {
        if (kind == "fp_circle") {
            double cx = 0.0, cy = 0.0;
            if (!pointOf(node.child("center"), cx, cy)) return;
            double ex = 0.0, ey = 0.0;
            if (!pointOf(node.child("end"), ex, ey)) return;
            const double radius = std::hypot(ex - cx, ey - cy);
            std::vector<Point> outline;
            addCirclePolygon(outline, cx, cy, radius, 40);
            RegionFeature region;
            region.outline = outline;
            region.layer = layer;
            footprint.geometry.regions.push_back(region);
            grow(footprint.bounds, cx - radius, cy - radius);
            grow(footprint.bounds, cx + radius, cy + radius);
            return;
        }
        double cx = 0.0, cy = 0.0, radius = 0.0, startAngle = 0.0, endAngle = 0.0;
        if (!arcGeometry(node, cx, cy, radius, startAngle, endAngle)) {
            // Unknown arc form: keep a line between the endpoints so nothing disappears.
            double x1 = 0.0, y1 = 0.0, x2 = 0.0, y2 = 0.0;
            if (!pointOf(node.child("start"), x1, y1)) return;
            if (!pointOf(node.child("end"), x2, y2)) return;
            TrackFeature track;
            track.x1 = x1; track.y1 = y1; track.x2 = x2; track.y2 = y2;
            track.width = width;
            track.layer = layer;
            footprint.geometry.tracks.push_back(track);
            return;
        }
        ArcFeature arc;
        arc.cx = cx;
        arc.cy = cy;
        arc.radius = radius;
        arc.startAngle = startAngle;
        arc.endAngle = endAngle;
        arc.width = width;
        arc.layer = layer;
        footprint.geometry.arcs.push_back(arc);
        grow(footprint.bounds, cx - radius, cy - radius);
        grow(footprint.bounds, cx + radius, cy + radius);
        return;
    }
    if (kind == "fp_poly" || kind == "fp_polygon") {
        std::vector<Point> points = polygonOf(node.child("pts"));
        if (points.size() < 3) return;
        RegionFeature region;
        region.outline = points;
        region.layer = layer;
        footprint.geometry.regions.push_back(region);
        for (const Point& point : points) grow(footprint.bounds, point.x, point.y);
        return;
    }
    if (kind == "fp_text" || kind == "fp_text_box") {
        TextFeature text;
        double x = 0.0, y = 0.0;
        if (!pointOf(node.child("at"), x, y)) return;
        text.x = x;
        text.y = y;
        text.rotation = node.number("at", 2, 0.0);
        text.layer = layer;
        text.height = node.number("font", 0, 1.0);   // `size` lives inside `effects`
        if (const sexpr::Node* effects = node.child("effects")) {
            if (const sexpr::Node* font = effects->child("font")) {
                const double size = font->number("size", 0, 1.0);
                if (size > 0.0) text.height = size * kMmToMil;
            }
        }
        if (text.height <= 0.0) text.height = 1.0 * kMmToMil;
        // The text itself is the third bare atom: (fp_text reference "REF**" (at …)).
        const std::vector<std::string> atoms = node.atoms();
        text.text = atoms.size() > 2 ? atoms[2] : "";
        if (!text.text.empty()) footprint.geometry.texts.push_back(text);
        return;
    }
    if (kind == "fp_curve") {
        // Bezier: approximate with its control polygon so the outline stays visible.
        std::vector<Point> points = polygonOf(node.child("pts"));
        for (std::size_t i = 0; i + 1 < points.size(); ++i) {
            TrackFeature track;
            track.x1 = points[i].x;
            track.y1 = points[i].y;
            track.x2 = points[i + 1].x;
            track.y2 = points[i + 1].y;
            track.width = width;
            track.layer = layer;
            footprint.geometry.tracks.push_back(track);
        }
    }
}

/// `(model "…/X.step" (offset (xyz 0 0 1)) (rotate (xyz 0 0 90)) (scale (xyz 1 1 1)))`
bool readModel(const sexpr::Node& node, EmbeddedModel& model) {
    const std::string path = node.first();
    if (path.empty()) return false;
    model.sourcePath = path;
    const std::size_t slash = path.find_last_of("/\\");
    model.name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (model.name.size() > 5 && model.name.compare(model.name.size() - 5, 5, ".step") == 0) {
        model.name.replace(model.name.size() - 5, 5, ".step");
    }
    if (const sexpr::Node* offset = node.child("offset")) {
        if (const sexpr::Node* xyz = offset->child("xyz")) {
            model.dx = xyz->children.size() > 1 ? xyz->children[1].asNumber(0.0) : 0.0;
            model.dy = xyz->children.size() > 2 ? xyz->children[2].asNumber(0.0) : 0.0;
            model.dz = xyz->children.size() > 3 ? xyz->children[3].asNumber(0.0) : 0.0;
        }
    }
    if (const sexpr::Node* rotate = node.child("rotate")) {
        if (const sexpr::Node* xyz = rotate->child("xyz")) {
            model.rotationX = xyz->children.size() > 1 ? xyz->children[1].asNumber(0.0) : 0.0;
            model.rotationY = xyz->children.size() > 2 ? xyz->children[2].asNumber(0.0) : 0.0;
            model.rotationZ = xyz->children.size() > 3 ? xyz->children[3].asNumber(0.0) : 0.0;
        }
    }
    if (const sexpr::Node* scale = node.child("scale")) {
        if (const sexpr::Node* xyz = scale->child("xyz")) {
            for (std::size_t i = 1; i < xyz->children.size() && i <= 3; ++i) {
                const double value = xyz->children[i].asNumber(1.0);
                if (std::fabs(value - 1.0) > 1e-6) return false;   // non-unit scale is refused
            }
        }
    }
    // KiCad always stores a full placement; the viewer honours it.
    model.hasPlacement = true;
    return true;
}

/// `(property "Reference" "U" (at …))` / `(fp_text reference "U" …)`
void readProperties(const sexpr::Node& node, std::vector<Property>& out) {
    for (const sexpr::Node& child : node.children) {
        if (!child.isList) continue;
        const std::string& head = child.head();
        if (head == "property") {
            const std::vector<std::string> atoms = child.atoms();
            if (atoms.size() >= 3) out.push_back(Property{atoms[1], atoms[2]});
        } else if (head == "fp_text") {
            const std::vector<std::string> atoms = child.atoms();
            if (atoms.size() >= 3) {
                const std::string& kind = atoms[1];
                if (kind == "reference") out.push_back(Property{"Reference", atoms[2]});
                else if (kind == "value") out.push_back(Property{"Value", atoms[2]});
            }
        }
    }
}

std::string propertyOf(const std::vector<Property>& properties, const std::string& name) {
    for (const Property& property : properties) {
        if (property.name == name) return property.value;
    }
    return {};
}

/// Recursively reads the graphics of a symbol, descending into its sub-units.
void readSymbolShapes(const sexpr::Node& container, LibrarySymbol& symbol, int part) {
    for (const sexpr::Node& node : container.children) {
        if (!node.isList) continue;
        const std::string& kind = node.head();
        if (kind == "symbol") {
            // Sub-symbols are named "NAME_<part>_<unit>"; keep the part number of the name.
            const std::string name = node.first();
            int subPart = part;
            const std::size_t last = name.find_last_of('_');
            if (last != std::string::npos && last + 1 < name.size()) {
                const std::size_t before = name.find_last_of('_', last - 1);
                if (before != std::string::npos) {
                    const std::string middle = name.substr(before + 1, last - before - 1);
                    const int value = std::atoi(middle.c_str());
                    if (value > 0) subPart = value;
                }
            }
            readSymbolShapes(node, symbol, subPart);
            continue;
        }
        const int stroke = static_cast<int>(strokeWidth(node));
        if (kind == "rectangle" || kind == "roundrect") {
            double x1 = 0.0, y1 = 0.0, x2 = 0.0, y2 = 0.0;
            if (!pointOf(node.child("start"), x1, y1)) continue;
            if (!pointOf(node.child("end"), x2, y2)) continue;
            SymbolShape shape;
            shape.kind = SymbolShape::Kind::Rectangle;
            shape.points = {Point{std::min(x1, x2), std::min(y1, y2)},
                            Point{std::max(x1, x2), std::max(y1, y2)}};
            shape.width = stroke;
            shape.fill = 0;
            if (const sexpr::Node* fill = node.child("fill")) {
                const std::string type = fill->text("type");
                if (type == "background" || type == "outline") shape.fill = 2;
            }
            shape.ownerPart = part;
            symbol.shapes.push_back(shape);
            continue;
        }
        if (kind == "polyline" || kind == "bezier") {
            const std::vector<Point> points = polygonOf(node.child("pts"));
            if (points.size() < 2) continue;
            SymbolShape shape;
            shape.kind = SymbolShape::Kind::Polyline;
            shape.points = points;
            shape.width = stroke;
            shape.ownerPart = part;
            symbol.shapes.push_back(shape);
            continue;
        }
        if (kind == "circle") {
            double cx = 0.0, cy = 0.0;
            if (!pointOf(node.child("center"), cx, cy)) continue;
            const double radius = node.number("radius", 0, 0.0) * kMmToMil;
            SymbolShape shape;
            shape.kind = SymbolShape::Kind::Ellipse;
            shape.points = {Point{cx, cy}};
            shape.radius = radius;
            shape.secondaryRadius = radius;
            shape.width = stroke;
            shape.ownerPart = part;
            symbol.shapes.push_back(shape);
            continue;
        }
        if (kind == "arc") {
            double sx = 0.0, sy = 0.0, mx = 0.0, my = 0.0, ex = 0.0, ey = 0.0;
            if (!pointOf(node.child("start"), sx, sy)) continue;
            if (!pointOf(node.child("mid"), mx, my)) continue;
            if (!pointOf(node.child("end"), ex, ey)) continue;
            const double d = 2.0 * (sx * (my - ey) + mx * (ey - sy) + ex * (sy - my));
            SymbolShape shape;
            shape.kind = SymbolShape::Kind::Arc;
            shape.width = stroke;
            shape.ownerPart = part;
            if (std::fabs(d) > 1e-12) {
                const double cx = ((sx * sx + sy * sy) * (my - ey) + (mx * mx + my * my) * (ey - sy) +
                                   (ex * ex + ey * ey) * (sy - my)) / d;
                const double cy = ((sx * sx + sy * sy) * (ex - mx) + (mx * mx + my * my) * (sx - ex) +
                                   (ex * ex + ey * ey) * (mx - sx)) / d;
                shape.points = {Point{cx, cy}};
                shape.radius = std::hypot(sx - cx, sy - cy);
                shape.startAngle = std::atan2(sy - cy, sx - cx) * 180.0 / 3.14159265358979323846;
                shape.endAngle = std::atan2(ey - cy, ex - cx) * 180.0 / 3.14159265358979323846;
                if (shape.endAngle <= shape.startAngle) shape.endAngle += 360.0;
            }
            symbol.shapes.push_back(shape);
            continue;
        }
        if (kind == "pin") {
            // (pin passive line (at x y angle) (length l) (name "…") (number "…"))
            SymbolPin pin;
            const std::vector<std::string> atoms = node.atoms();
            if (atoms.size() >= 3) {
                pin.electrical = atoms[1];
            }
            const double angle = node.number("at", 2, 0.0);
            pin.x = node.number("at", 0, 0.0) * kMmToMil;
            pin.y = node.number("at", 1, 0.0) * kMmToMil;
            pin.length = node.number("length", 0, 0.0) * kMmToMil;
            pin.rotation = static_cast<int>(std::lround(angle));
            pin.name = node.text("name");
            pin.designator = node.text("number");
            if (pin.name == "~") pin.name.clear();
            pin.ownerPart = part;
            symbol.pinGeometry.push_back(pin);
            Pin listPin;
            listPin.designator = pin.designator;
            listPin.name = pin.name;
            listPin.electrical = pin.electrical;
            symbol.pins.push_back(listPin);
            continue;
        }
    }
}

} // namespace

bool looksLikeKiCad(const std::vector<std::uint8_t>& head) {
    if (head.empty()) return false;
    std::size_t start = 0;
    if (head.size() >= 3 && head[0] == 0xEF && head[1] == 0xBB && head[2] == 0xBF) start = 3;
    while (start < head.size() &&
           (head[start] == ' ' || head[start] == '\t' || head[start] == '\r' || head[start] == '\n')) {
        ++start;
    }
    if (start >= head.size() || head[start] != '(') return false;
    std::string text;
    for (std::size_t i = start; i < head.size() && i < start + 64; ++i) {
        text.push_back(static_cast<char>(head[i]));
    }
    static const char* kHeads[] = {"(kicad_pcb", "(kicad_symbol_lib", "(module", "(footprint",
                                   "(kicad_sch"};
    for (const char* wanted : kHeads) {
        if (text.compare(0, std::strlen(wanted), wanted) == 0) return true;
    }
    return false;
}

bool readKiCadFootprint(const std::string& text, LibraryFootprint& footprint,
                        std::string& error) {
    std::vector<sexpr::Node> roots;
    if (!sexpr::parse(text, roots, &error)) return false;
    const sexpr::Node* root = sexpr::findRoot(roots, "footprint");
    if (root == nullptr) root = sexpr::findRoot(roots, "module");
    if (root == nullptr) {
        error = "no (footprint …) or (module …) record";
        return false;
    }
    footprint = LibraryFootprint{};
    footprint.name = root->first();
    if (footprint.name.empty()) footprint.name = "footprint";

    readProperties(*root, footprint.parameters);
    footprint.description = propertyOf(footprint.parameters, "Description");
    if (footprint.description.empty()) {
        // v6/v7 write the description as (descr "…") and the keywords as (tags "…").
        const sexpr::Node* descr = root->child("descr");
        if (descr != nullptr && descr->children.size() > 1) {
            footprint.description = descr->children[1].atom;
        }
        const sexpr::Node* tags = root->child("tags");
        if (tags != nullptr && tags->children.size() > 1) {
            footprint.parameters.push_back(Property{"KEYWORDS", tags->children[1].atom});
        }
    }
    for (const Property& property : footprint.parameters) {
        if (property.name != "Datasheet") continue;
        const std::string& link = property.value;
        if (link.empty() || link == "~") continue;
        footprint.parameters.push_back(Property{"DATASHEET", link});
        break;
    }

    for (const sexpr::Node& node : root->children) {
        if (!node.isList) continue;
        const std::string& kind = node.head();
        if (kind == "pad") {
            readPad(node, footprint);
        } else if (kind.rfind("fp_", 0) == 0) {
            readGraphic(node, footprint);
        } else if (kind == "model") {
            EmbeddedModel model;
            if (readModel(node, model)) footprint.models.push_back(model);
        }
    }

    // Counters and the height come from the primitives that were read.
    footprint.padCount = static_cast<int>(footprint.geometry.pads.size());
    footprint.trackCount = static_cast<int>(footprint.geometry.tracks.size());
    footprint.arcCount = static_cast<int>(footprint.geometry.arcs.size());
    footprint.regionCount = static_cast<int>(footprint.geometry.regions.size());
    footprint.textCount = static_cast<int>(footprint.geometry.texts.size());
    footprint.viaCount = 0;
    footprint.fillCount = 0;
    footprint.bodyCount = static_cast<int>(footprint.models.size());
    for (const Property& property : footprint.parameters) {
        if (property.name != "Height" && property.name != "height") continue;
        const double mm = std::atof(property.value.c_str());
        if (mm > 0.0) footprint.height = mm * kMmToMil;
    }
    if (footprint.geometry.pads.empty() && footprint.geometry.tracks.empty() &&
        footprint.geometry.regions.empty()) {
        error = "the footprint has no pads and no graphics";
        return false;
    }
    return true;
}

bool readKiCadSymbols(const std::string& text, std::vector<LibrarySymbol>& symbols,
                      std::string& error) {
    std::vector<sexpr::Node> roots;
    if (!sexpr::parse(text, roots, &error)) return false;
    const sexpr::Node* root = sexpr::findRoot(roots, "kicad_symbol_lib");
    if (root == nullptr) {
        // A bare `(symbol …)` file is accepted as a one-symbol library.
        root = sexpr::findRoot(roots, "symbol");
        if (root == nullptr) {
            error = "no (kicad_symbol_lib …) record";
            return false;
        }
    }
    std::vector<const sexpr::Node*> symbolNodes;
    if (root->head() == "symbol") {
        symbolNodes.push_back(root);
    } else {
        symbolNodes = root->childrenNamed("symbol");
    }
    if (symbolNodes.empty()) {
        error = "the symbol library holds no symbol";
        return false;
    }
    symbols.clear();
    for (const sexpr::Node* node : symbolNodes) {
        LibrarySymbol symbol;
        symbol.name = node->first();
        if (symbol.name.empty()) symbol.name = "symbol";
        readProperties(*node, symbol.parameters);
        symbol.description = propertyOf(symbol.parameters, "Description");
        if (symbol.description.empty()) symbol.description = propertyOf(symbol.parameters, "ki_description");
        if (symbol.description.empty()) symbol.description = propertyOf(symbol.parameters, "ki_keywords");
        const std::string reference = propertyOf(symbol.parameters, "Reference");
        if (!reference.empty()) symbol.designatorPrefix = reference + "?";
        const std::string datasheet = propertyOf(symbol.parameters, "Datasheet");
        if (!datasheet.empty() && datasheet != "~") {
            symbol.parameters.push_back(Property{"DATASHEET", datasheet});
        }
        const std::string footprint = propertyOf(symbol.parameters, "Footprint");
        if (!footprint.empty()) {
            symbol.parameters.push_back(Property{"FOOTPRINT", footprint});
        }
        readSymbolShapes(*node, symbol, 1);
        // Multi-part symbols: the highest sub-part number that appeared.
        int parts = 1;
        for (const SymbolShape& shape : symbol.shapes) parts = std::max(parts, shape.ownerPart);
        for (const SymbolPin& pin : symbol.pinGeometry) parts = std::max(parts, pin.ownerPart);
        symbol.partCount = parts;
        symbols.push_back(std::move(symbol));
    }
    return true;
}

bool readKiCadLibrary(const std::string& text, const std::string& fileName, Library& library,
                      std::string& error) {
    library.fileName = fileName;
    library.container = Container::AsciiText;
    std::vector<sexpr::Node> roots;
    if (!sexpr::parse(text, roots, &error)) return false;

    if (sexpr::findRoot(roots, "kicad_symbol_lib") != nullptr ||
        sexpr::findRoot(roots, "symbol") != nullptr) {
        library.type = DocType::SchLib;
        library.formatVersion = "KiCad symbol library";
        return readKiCadSymbols(text, library.symbols, error);
    }
    if (sexpr::findRoot(roots, "footprint") == nullptr &&
        sexpr::findRoot(roots, "module") == nullptr) {
        error = "neither a KiCad footprint nor a KiCad symbol library";
        return false;
    }
    library.type = DocType::PcbLib;
    library.formatVersion = "KiCad footprint";
    LibraryFootprint footprint;
    if (!readKiCadFootprint(text, footprint, error)) return false;
    library.footprints.push_back(std::move(footprint));
    return true;
}

} // namespace pcbv
