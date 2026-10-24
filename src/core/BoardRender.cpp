// BoardRender.cpp — see BoardRender.h.
#include "BoardRender.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "LayerStyle.h"

namespace pcbv {
namespace {

constexpr double kPi = 3.14159265358979323846;

struct View {
    double minX = 0.0, minY = 0.0;    // mil
    double width = 1.0, height = 1.0; // mil
    double margin = 0.0;              // mil
    double unit = 1.0;                // device units per mil

    double maxY() const { return minY + height; }
    double x(double mil) const { return (mil - minX + margin) * unit; }
    double y(double mil) const { return (maxY() - mil + margin) * unit; }
};

bool wanted(int layer, const BoardRenderOptions& options) {
    if (options.isHidden(layer)) return false;
    if (!options.silk && isSilk(layer)) return false;
    return true;
}

double normaliseAngle(double angle) {
    double value = std::fmod(angle, 360.0);
    if (value < 0.0) value += 360.0;
    return value;
}

/// Text on a board may be stored at any angle; half of the files use 180 for a label the
/// designer meant upright. Snapping to the nearest quarter turn and flipping the upside down
/// ones keeps every designator readable, which is what a viewer has to guarantee.
double readableRotation(double degrees) {
    double value = std::fmod(degrees, 360.0);
    if (value < 0.0) value += 360.0;
    const double quarter = std::round(value / 90.0) * 90.0;
    if (std::fabs(value - quarter) <= 4.0) value = quarter;
    value = std::fmod(value, 360.0);
    if (value > 90.0 && value <= 270.0) value -= 180.0;   // never upside down
    if (value > 270.0) value -= 360.0;
    return value;
}

} // namespace

bool hasBoardGeometry(const Document& doc) {
    return !doc.geometry.empty();
}

std::set<int> defaultHiddenLayers() {
    std::set<int> hidden;
    for (const int layer : {35, 36, 37, 38, 55, 56, 57, 73}) hidden.insert(layer);
    for (int layer = 58; layer <= 72; ++layer) hidden.insert(layer);  // mechanical 1..15
    return hidden;
}

BoardRenderList buildBoardRender(const Document& doc, const BoardRenderOptions& options,
                                 double unitSize) {
    BoardRenderList list;
    if (doc.geometry.empty()) return list;

    View view;
    if (doc.bounds.valid) {
        view.minX = doc.bounds.minX;
        view.minY = doc.bounds.minY;
        view.width = std::max(doc.bounds.width(), 1.0);
        view.height = std::max(doc.bounds.height(), 1.0);
    }
    const double longest = std::max(view.width, view.height);
    view.margin = longest * 0.03;
    view.unit = unitSize / (longest + 2.0 * view.margin);

    list.width = (view.width + 2.0 * view.margin) * view.unit;
    list.height = (view.height + 2.0 * view.margin) * view.unit;
    list.boardWidthMil = view.width;
    list.boardHeightMil = view.height;
    list.unitsPerMil = view.unit;

    auto push = [&list](RenderPrimitive primitive) {
        list.primitives.push_back(std::move(primitive));
    };

    const ThemeColours& colours = themeColours();

    // --- background ----------------------------------------------------------
    {
        RenderPrimitive background;
        background.kind = RenderPrimitive::Kind::Polygon;
        background.filled = true;
        background.color = colours.canvas;
        background.background = true;
        background.points = {Point{0.0, 0.0}, Point{list.width, 0.0},
                             Point{list.width, list.height}, Point{0.0, list.height}};
        push(background);
    }

    // --- board outline -------------------------------------------------------
    if (doc.geometry.outline.size() >= 3) {
        RenderPrimitive outline;
        outline.kind = RenderPrimitive::Kind::Polygon;
        outline.filled = true;
        outline.color = colours.boardFill;
        for (const auto& point : doc.geometry.outline) {
            outline.points.push_back(Point{view.x(point.x), view.y(point.y)});
        }
        list.boardOutline = outline.points;
        push(outline);
        RenderPrimitive edge = outline;
        edge.filled = false;
        edge.color = colours.boardEdge;
        edge.strokeWidth = 1.4;
        push(edge);
    } else {
        RenderPrimitive frame;
        frame.kind = RenderPrimitive::Kind::Polygon;
        frame.filled = false;
        frame.dashed = true;
        frame.color = colours.boardEdge;
        frame.strokeWidth = 1.0;
        const double x0 = view.x(view.minX);
        const double y0 = view.y(view.maxY());
        frame.points = {Point{x0, y0}, Point{x0 + view.width * view.unit, y0},
                        Point{x0 + view.width * view.unit, y0 + view.height * view.unit},
                        Point{x0, y0 + view.height * view.unit}};
        list.boardOutline = frame.points;
        push(frame);
    }

    // --- copper pours / regions ---------------------------------------------
    for (const auto& region : doc.geometry.regions) {
        if (!wanted(region.layer, options) || region.outline.size() < 3) continue;
        RenderPrimitive polygon;
        polygon.kind = RenderPrimitive::Kind::Polygon;
        polygon.filled = true;
        polygon.color = layerColor(region.layer);
        polygon.layer = region.layer;
        polygon.net = region.net;
        if (region.pour) {
            // Without Altium's clearance engine the filled shape of a pour cannot be
            // reproduced; draw its contour so the polygon stays visible.
            polygon.opacity = 0.10;
            polygon.dashed = true;
            polygon.strokeWidth = 0.10;
        } else if (isSilk(region.layer)) {
            // A silkscreen region is the outline of a part (or of lettering): filling it
            // would paint a white slab over the part, so it is drawn as a line.
            polygon.filled = false;
            polygon.strokeWidth = 1.6;
            polygon.opacity = 1.0;
        } else {
            polygon.opacity = 0.5;
            polygon.strokeWidth = 0.05;
        }
        for (const auto& point : region.outline) {
            polygon.points.push_back(Point{view.x(point.x), view.y(point.y)});
        }
        push(polygon);
    }

    // --- fills ---------------------------------------------------------------
    for (const auto& fill : doc.geometry.fills) {
        if (!wanted(fill.layer, options)) continue;
        const double cx = (fill.x1 + fill.x2) / 2.0;
        const double cy = (fill.y1 + fill.y2) / 2.0;
        const double halfW = std::fabs(fill.x2 - fill.x1) / 2.0 * view.unit;
        const double halfH = std::fabs(fill.y2 - fill.y1) / 2.0 * view.unit;
        const double rad = -fill.rotation * kPi / 180.0;
        const double cosA = std::cos(rad);
        const double sinA = std::sin(rad);
        const double px = view.x(cx);
        const double py = view.y(cy);
        RenderPrimitive rect;
        rect.kind = RenderPrimitive::Kind::Polygon;
        rect.filled = !isSilk(fill.layer);
        rect.color = layerColor(fill.layer);
        rect.layer = fill.layer;
        if (!rect.filled) rect.strokeWidth = 1.6;
        for (const auto& corner : {Point{-halfW, -halfH}, Point{halfW, -halfH},
                                   Point{halfW, halfH}, Point{-halfW, halfH}}) {
            rect.points.push_back(Point{px + corner.x * cosA - corner.y * sinA,
                                        py + corner.x * sinA + corner.y * cosA});
        }
        push(rect);
    }

    // --- tracks --------------------------------------------------------------
    for (const auto& track : doc.geometry.tracks) {
        if (!wanted(track.layer, options)) continue;
        RenderPrimitive line;
        line.kind = RenderPrimitive::Kind::Line;
        line.color = layerColor(track.layer);
        line.layer = track.layer;
        line.strokeWidth = std::max(track.width * view.unit, 0.6);
        line.points = {Point{view.x(track.x1), view.y(track.y1)},
                       Point{view.x(track.x2), view.y(track.y2)}};
        line.net = track.net;
        line.selectable = true;
        push(line);
    }

    // --- arcs ----------------------------------------------------------------
    for (const auto& arc : doc.geometry.arcs) {
        if (!wanted(arc.layer, options) || arc.radius <= 0.0) continue;
        const double start = normaliseAngle(arc.startAngle);
        double sweep = normaliseAngle(arc.endAngle - arc.startAngle);
        if (sweep <= 1e-6) sweep = 360.0;
        const double stroke = std::max(arc.width * view.unit, 0.6);
        const double cx = view.x(arc.cx);
        const double cy = view.y(arc.cy);
        const double r = arc.radius * view.unit;
        if (sweep >= 359.999) {
            RenderPrimitive circle;
            circle.kind = RenderPrimitive::Kind::Circle;
            circle.center = Point{cx, cy};
            circle.radius = r;
            circle.filled = false;
            circle.color = layerColor(arc.layer);
            circle.layer = arc.layer;
            circle.strokeWidth = stroke;
            push(circle);
            continue;
        }
        // Y is mirrored for the display, so the Altium counter clockwise arc runs
        // clockwise on screen; sample it into a polyline (the display list has no
        // bezier primitive and this keeps SVG and Qt pixel identical).
        const int steps = std::max(6, static_cast<int>(sweep / 5.0));
        RenderPrimitive line;
        line.kind = RenderPrimitive::Kind::Line;
        line.color = layerColor(arc.layer);
        line.layer = arc.layer;
        line.strokeWidth = stroke;
        for (int i = 0; i <= steps; ++i) {
            const double angle = (start + sweep * static_cast<double>(i) / steps) * kPi / 180.0;
            line.points.push_back(Point{cx + r * std::cos(angle), cy - r * std::sin(angle)});
        }
        RenderPrimitive polyline = line;
        polyline.kind = RenderPrimitive::Kind::Polygon;
        polyline.filled = false;
        polyline.closed = false;
        push(polyline);
    }

    // --- pads ----------------------------------------------------------------
    if (options.pads) {
        for (const auto& pad : doc.geometry.pads) {
            const double widthMil = pad.width > 0.0 ? pad.width : pad.height;
            const double heightMil = pad.height > 0.0 ? pad.height : pad.width;
            const double halfW = widthMil * view.unit / 2.0;
            const double halfH = heightMil * view.unit / 2.0;
            if (halfW <= 0.0 || halfH <= 0.0) continue;
            const bool throughHole = pad.holeSize > 0.0;
            // A through-hole pad lives on every copper layer; it only disappears once
            // both outer layers are switched off.
            const int padLayer = throughHole ? 74 : (pad.layer == 0 ? 1 : pad.layer);
            if (throughHole ? (options.isHidden(1) && options.isHidden(32))
                            : options.isHidden(padLayer)) {
                continue;
            }
            const double px = view.x(pad.x);
            const double py = view.y(pad.y);
            const double rad = -pad.rotation * kPi / 180.0;
            const double cosA = std::cos(rad);
            const double sinA = std::sin(rad);

            RenderPrimitive body;
            body.kind = RenderPrimitive::Kind::Polygon;
            body.filled = true;
            // A pad is bare metal, not copper under the mask: it reads gold on every layer,
            // exactly as the plated surface does on the finished board.
            body.color = colours.pad;
            body.layer = padLayer;
            body.strokeWidth = 0.9;
            body.opacity = 1.0;
            body.net = pad.net;
            body.selectable = true;
            // Real pads are not all rectangles: a through-hole pad with the same size in
            // both directions is a disc, and a surface pad has softened corners. Drawing
            // every pad as a sharp box is the single clearest "this is not a CAD tool" tell.
            const bool roundPad = throughHole &&
                                  std::fabs(widthMil - heightMil) <= 0.12 * std::max(widthMil, heightMil);
            std::vector<Point> local;
            if (roundPad) {
                const int steps = 40;
                for (int i = 0; i < steps; ++i) {
                    const double angle = 2.0 * kPi * static_cast<double>(i) / steps;
                    local.push_back(Point{halfW * std::cos(angle), halfH * std::sin(angle)});
                }
            } else {
                const double r = std::min(std::min(halfW, halfH) * 0.28, std::min(halfW, halfH));
                const int steps = 5;
                const struct { double cx, cy, from; } corners[4] = {
                    {-halfW + r, -halfH + r, 180.0}, {halfW - r, -halfH + r, 270.0},
                    {halfW - r, halfH - r, 0.0},     {-halfW + r, halfH - r, 90.0}};
                for (const auto& corner : corners) {
                    for (int i = 0; i <= steps; ++i) {
                        const double angle = (corner.from + 90.0 * static_cast<double>(i) / steps) * kPi / 180.0;
                        local.push_back(Point{corner.cx + r * std::cos(angle), corner.cy + r * std::sin(angle)});
                    }
                }
            }
            for (const Point& corner : local) {
                body.points.push_back(Point{px + corner.x * cosA - corner.y * sinA,
                                            py + corner.x * sinA + corner.y * cosA});
            }
            push(body);
            if (body.strokeWidth > 0.0) {
                RenderPrimitive rim = body;
                rim.filled = false;
                rim.color = colours.padEdge;
                rim.selectable = false;
                push(rim);
            }

            if (throughHole) {
                RenderPrimitive hole;
                hole.kind = RenderPrimitive::Kind::Circle;
                hole.center = Point{px, py};
                hole.radius = pad.holeSize * view.unit / 2.0;
                hole.filled = true;
                hole.color = colours.padHole;
                push(hole);
            }
        }
    }

    // --- vias ----------------------------------------------------------------
    for (const auto& via : doc.geometry.vias) {
        RenderPrimitive ring;
        ring.kind = RenderPrimitive::Kind::Circle;
        ring.center = Point{view.x(via.x), view.y(via.y)};
        ring.radius = std::max(via.diameter, 0.0) / 2.0 * view.unit;
        ring.filled = true;
        ring.color = colours.pad;
        ring.layer = 74;
        ring.opacity = 0.9;
        ring.net = via.net;
        ring.selectable = true;
        push(ring);
        if (via.hole > 0.0) {
            RenderPrimitive hole = ring;
            hole.color = colours.padHole;
            hole.opacity = 1.0;
            hole.radius = via.hole / 2.0 * view.unit;
            push(hole);
        }
    }

    // --- silkscreen text -----------------------------------------------------
    if (options.silk) {
        for (const auto& label : doc.geometry.texts) {
            if (label.text.empty() || !isSilk(label.layer) || label.height <= 0.0) continue;
            if (options.isHidden(label.layer)) continue;
            RenderPrimitive text;
            text.kind = RenderPrimitive::Kind::Text;
            text.color = label.color != 0 ? label.color : layerColor(label.layer);
            text.layer = label.layer;
            text.text = label.text;
            text.fontSize = label.height * view.unit;
            text.x = view.x(label.x);
            text.y = view.y(label.y);
            text.rotation = readableRotation(-label.rotation);
            text.textAnchor = label.anchor;
            push(text);
        }
    }

    // --- designators ---------------------------------------------------------
    if (options.designators) {
        const double fontSize = std::max(std::min(view.width, view.height) * view.unit * 0.035, 14.0);
        for (const auto& component : doc.components) {
            if (component.designator.empty()) continue;
            // A label dropped on the part's own centre sits on top of its pads, which is
            // exactly where the eye needs to see copper. Park it just above the footprint
            // instead, and only fall back to "below" when that would leave the board.
            double minX = component.x - 40.0, maxX = component.x + 40.0;
            double minY = component.y - 40.0, maxY = component.y + 40.0;
            if (!component.pads.empty()) {
                minX = maxX = component.pads.front().x;
                minY = maxY = component.pads.front().y;
                for (const auto& pad : component.pads) {
                    minX = std::min(minX, pad.x - pad.width / 2.0);
                    maxX = std::max(maxX, pad.x + pad.width / 2.0);
                    minY = std::min(minY, pad.y - pad.height / 2.0);
                    maxY = std::max(maxY, pad.y + pad.height / 2.0);
                }
            }
            const double lineHeight = fontSize / view.unit;   // label height in mil
            double labelY = maxY + lineHeight * 0.55;
            if (doc.bounds.valid && labelY > doc.bounds.maxY) labelY = minY - lineHeight * 0.45;

            RenderPrimitive text;
            text.kind = RenderPrimitive::Kind::Text;
            text.color = colours.designator;
            std::string layerText = component.layer;
            for (char& c : layerText) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            text.layer = layerText.find("BOTTOM") != std::string::npos ? 32 : 1;
            text.text = component.designator;
            text.fontSize = fontSize;
            text.x = view.x((minX + maxX) / 2.0);
            text.y = view.y(labelY);
            text.strokeWidth = 0.0;  // plain ink reads cleaner than an outlined label
            push(text);
        }
    }

    return list;
}

} // namespace pcbv
