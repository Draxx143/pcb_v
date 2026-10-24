// Icons.cpp — see Icons.h. Every glyph is drawn on a 20 x 20 grid with a 1.5 unit
// stroke and rounded caps, so all icons share the same optical weight.
#include "Icons.h"

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QPolygonF>

#include <cmath>
#include <functional>

namespace pcbv {
namespace gui {
namespace {

constexpr qreal kGrid = 20.0;      // every glyph is designed on a 20 x 20 grid
constexpr qreal kPixelRatio = 2.0; // render at 2x for HiDPI screens

const QColor kIdle(0xcf, 0xd8, 0xdc);
const QColor kHover(0xff, 0xff, 0xff);
const QColor kChecked(0x8a, 0xb4, 0xf8);
const QColor kDisabled(0x6b, 0x72, 0x7c);

using Glyph = std::function<void(QPainter&)>;

/// Renders one glyph at `size` logical pixels. The pixmap carries the device pixel
/// ratio, so QPainter already works in logical coordinates: only the 20 unit design
/// grid has to be scaled, never the DPR again (doing both clips the drawing).
QPixmap renderGlyph(const Glyph& glyph, const QColor& color, int size, qreal pixelRatio) {
    QPixmap pixmap(static_cast<int>(std::lround(size * pixelRatio)),
                   static_cast<int>(std::lround(size * pixelRatio)));
    pixmap.setDevicePixelRatio(pixelRatio);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.scale(size / kGrid, size / kGrid);

    QPen pen(color, 1.55);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    glyph(painter);
    return pixmap;
}

QIcon buildIcon(const Glyph& glyph) {
    QIcon icon;
    // Provide the sizes the toolbars and menus ask for, so nothing is upscaled.
    for (const int size : {16, 20, 24, 32, 48}) {
        icon.addPixmap(renderGlyph(glyph, kIdle, size, kPixelRatio), QIcon::Normal, QIcon::Off);
        icon.addPixmap(renderGlyph(glyph, kHover, size, kPixelRatio), QIcon::Active, QIcon::Off);
        icon.addPixmap(renderGlyph(glyph, kChecked, size, kPixelRatio), QIcon::Normal, QIcon::On);
        icon.addPixmap(renderGlyph(glyph, kDisabled, size, kPixelRatio), QIcon::Disabled,
                       QIcon::Off);
    }
    return icon;
}

// --- glyphs ----------------------------------------------------------------

void folderGlyph(QPainter& painter) {
    QPainterPath path;
    path.moveTo(2.8, 15.4);
    path.lineTo(2.8, 5.4);
    path.lineTo(7.6, 5.4);
    path.lineTo(9.4, 7.4);
    path.lineTo(17.2, 7.4);
    path.lineTo(17.2, 15.4);
    path.closeSubpath();
    painter.drawPath(path);
}

void magnifier(QPainter& painter) {
    painter.drawEllipse(QRectF(3.4, 3.4, 10.2, 10.2));
    painter.drawLine(QPointF(12.6, 12.6), QPointF(16.8, 16.8));
}

void zoomInGlyph(QPainter& painter) {
    magnifier(painter);
    painter.drawLine(QPointF(6.6, 8.5), QPointF(10.4, 8.5));
    painter.drawLine(QPointF(8.5, 6.6), QPointF(8.5, 10.4));
}

void zoomOutGlyph(QPainter& painter) {
    magnifier(painter);
    painter.drawLine(QPointF(6.6, 8.5), QPointF(10.4, 8.5));
}

void fitGlyph(QPainter& painter) {
    const qreal edge = 2.9;
    const qreal arm = 4.6;
    const qreal far = 17.1;
    painter.drawLine(QPointF(edge, edge), QPointF(edge + arm, edge));
    painter.drawLine(QPointF(edge, edge), QPointF(edge, edge + arm));
    painter.drawLine(QPointF(far, edge), QPointF(far - arm, edge));
    painter.drawLine(QPointF(far, edge), QPointF(far, edge + arm));
    painter.drawLine(QPointF(edge, far), QPointF(edge + arm, far));
    painter.drawLine(QPointF(edge, far), QPointF(edge, far - arm));
    painter.drawLine(QPointF(far, far), QPointF(far - arm, far));
    painter.drawLine(QPointF(far, far), QPointF(far, far - arm));
}

void layersGlyph(QPainter& painter) {
    const qreal centreX = 10.0;
    const qreal halfWidth = 6.6;
    const qreal halfHeight = 2.8;
    for (int i = 0; i < 3; ++i) {
        const qreal centreY = 4.9 + static_cast<qreal>(i) * 4.4;
        const QPolygonF rhombus({QPointF(centreX, centreY - halfHeight),
                                 QPointF(centreX + halfWidth, centreY),
                                 QPointF(centreX, centreY + halfHeight),
                                 QPointF(centreX - halfWidth, centreY)});
        painter.drawPolygon(rhombus);
    }
}

void exportGlyph(QPainter& painter) {
    QPainterPath tray;
    tray.moveTo(3.6, 12.6);
    tray.lineTo(3.6, 16.6);
    tray.lineTo(16.4, 16.6);
    tray.lineTo(16.4, 12.6);
    painter.drawPath(tray);
    painter.drawLine(QPointF(10.0, 14.6), QPointF(10.0, 3.4));
    painter.drawLine(QPointF(6.4, 7.0), QPointF(10.0, 3.4));
    painter.drawLine(QPointF(13.6, 7.0), QPointF(10.0, 3.4));
}

void measureGlyph(QPainter& painter) {
    // A diagonal ruler with three ticks, drawn as a closed parallelogram.
    const QPolygonF body({QPointF(2.6, 12.2), QPointF(12.2, 2.6), QPointF(17.4, 7.8),
                          QPointF(7.8, 17.4)});
    painter.drawPolygon(body);
    const QPointF direction(0.707, 0.707);  // across the ruler
    for (const qreal t : {0.30, 0.55, 0.80}) {
        const QPointF from(2.6 + (12.2 - 2.6) * t, 12.2 + (2.6 - 12.2) * t);
        painter.drawLine(from, from + direction * 3.1);
    }
}

void threeDGlyph(QPainter& painter) {
    // An isometric cube: hexagon outline plus the three edges that meet in the middle.
    const QPolygonF cube({QPointF(10.0, 2.6), QPointF(16.4, 6.4), QPointF(16.4, 13.6),
                          QPointF(10.0, 17.4), QPointF(3.6, 13.6), QPointF(3.6, 6.4)});
    painter.drawPolygon(cube);
    painter.drawLine(QPointF(10.0, 10.0), QPointF(10.0, 17.4));
    painter.drawLine(QPointF(10.0, 10.0), QPointF(3.6, 6.4));
    painter.drawLine(QPointF(10.0, 10.0), QPointF(16.4, 6.4));
}

void printGlyph(QPainter& painter) {
    painter.drawRoundedRect(QRectF(2.8, 7.6, 14.4, 5.6), 1.6, 1.6);
    painter.drawRect(QRectF(5.9, 3.2, 8.2, 4.4));
    painter.drawRect(QRectF(5.9, 13.2, 8.2, 3.6));
}

} // namespace

QIcon openIcon() { return buildIcon(folderGlyph); }
QIcon zoomInIcon() { return buildIcon(zoomInGlyph); }
QIcon zoomOutIcon() { return buildIcon(zoomOutGlyph); }
QIcon fitIcon() { return buildIcon(fitGlyph); }
QIcon layersIcon() { return buildIcon(layersGlyph); }
QIcon exportIcon() { return buildIcon(exportGlyph); }
QIcon measureIcon() { return buildIcon(measureGlyph); }
QIcon printIcon() { return buildIcon(printGlyph); }
QIcon threeDIcon() { return buildIcon(threeDGlyph); }

QIcon appIcon() {
    constexpr int size = 64;
    constexpr qreal pixelRatio = 2.0;
    QPixmap pixmap(static_cast<int>(size * pixelRatio), static_cast<int>(size * pixelRatio));
    pixmap.setDevicePixelRatio(pixelRatio);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    // Painted in 64 x 64 logical units; the device pixel ratio is applied by Qt.

    // Board body.
    painter.setPen(QPen(QColor(0x2a, 0x5d, 0x9f), 2.0));
    painter.setBrush(QColor(0x12, 0x16, 0x1c));
    painter.drawRoundedRect(QRectF(3.0, 3.0, 58.0, 58.0), 10.0, 10.0);

    // Traces (top copper in red, bottom copper in blue, like the board view).
    painter.setBrush(Qt::NoBrush);
    QPen trace(QColor(0xe0, 0x5c, 0x5c), 3.0);
    trace.setCapStyle(Qt::RoundCap);
    painter.setPen(trace);
    painter.drawLine(QPointF(14.0, 20.0), QPointF(38.0, 20.0));
    painter.drawLine(QPointF(38.0, 20.0), QPointF(38.0, 30.0));
    painter.drawLine(QPointF(14.0, 34.0), QPointF(26.0, 34.0));

    QPen lower(QColor(0x4c, 0x86, 0xd9), 3.0);
    lower.setCapStyle(Qt::RoundCap);
    painter.setPen(lower);
    painter.drawLine(QPointF(26.0, 44.0), QPointF(48.0, 44.0));
    painter.drawLine(QPointF(48.0, 44.0), QPointF(48.0, 32.0));

    // Pads.
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0xf2, 0xe6, 0xa8));
    painter.drawEllipse(QPointF(14.0, 20.0), 4.6, 4.6);
    painter.drawEllipse(QPointF(14.0, 34.0), 4.6, 4.6);
    painter.drawEllipse(QPointF(26.0, 44.0), 4.6, 4.6);
    painter.drawEllipse(QPointF(48.0, 32.0), 4.6, 4.6);
    painter.drawEllipse(QPointF(38.0, 30.0), 3.6, 3.6);
    painter.drawEllipse(QPointF(26.0, 34.0), 3.6, 3.6);

    return QIcon(pixmap);
}

} // namespace gui
} // namespace pcbv
