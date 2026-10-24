// BoardRender.h — turns PCB geometry into a framework independent display list.
//
// The display list is what the SVG export and the Qt board view both paint, so the
// two can never drift apart. Coordinates in the list are "device units" inside a
// rectangle of `width` x `height` (the Y axis already points down).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Export.h"
#include "Model.h"

namespace pcbv {

struct RenderPrimitive {
    enum class Kind { Line, Polygon, Circle, Text };

    Kind kind = Kind::Line;
    std::uint32_t color = 0xFFFFFF;  ///< 0xRRGGBB
    double opacity = 1.0;
    double strokeWidth = 0.0;        ///< device units, 0 = no stroke
    bool filled = false;
    bool dashed = false;
    bool closed = true;   ///< Polygon only: join the last point back to the first

    std::vector<Point> points;       ///< Line: 2 points; Polygon: n points
    Point center;                    ///< Circle
    double radius = 0.0;             ///< Circle, device units

    bool background = false;         ///< the page fill behind the board (not printed)
    int layer = 1;                   ///< Altium layer id (drives the 3D view's z)
    std::string net;                 ///< net this primitive belongs to ("" = none)
    bool selectable = false;         ///< true for copper primitives (click to highlight)

    std::string text;                ///< Text
    int textAnchor = 0;              ///< Text: 0 = centred, 1 = left, 2 = right
    double fontSize = 0.0;           ///< Text, device units
    double rotation = 0.0;           ///< Text/rect rotation in degrees (board space)
    double x = 0.0, y = 0.0;         ///< Text anchor
};

struct BoardRenderList {
    double width = 0.0;   ///< device units (longer side = 1000)
    double height = 0.0;
    double boardWidthMil = 0.0;
    double boardHeightMil = 0.0;
    double unitsPerMil = 1.0;  ///< device units per mil (used to recover real sizes)
    std::vector<RenderPrimitive> primitives;

    bool empty() const { return primitives.empty(); }
};

/// Builds the display list for the given document. `unitSize` is the length of the
/// longer side in device units (1000 by default).
BoardRenderList buildBoardRender(const Document& doc, const BoardRenderOptions& options,
                                 double unitSize = 1000.0);

/// True when the document has drawable PCB primitives.
bool hasBoardGeometry(const Document& doc);

} // namespace pcbv
