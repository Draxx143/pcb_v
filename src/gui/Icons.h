// Icons.h — flat vector icons drawn at runtime.
//
// The application ships no image files: every icon is painted with QPainter from a
// small path description, exactly like the board view. That keeps the binary small,
// makes the icons crisp on HiDPI screens and lets them follow the theme colours.
#pragma once

#include <QIcon>

namespace pcbv {
namespace gui {

QIcon openIcon();
QIcon zoomInIcon();
QIcon zoomOutIcon();
QIcon fitIcon();
QIcon layersIcon();
QIcon exportIcon();

/// Window / task bar icon: a small stylised PCB.
QIcon appIcon();

} // namespace gui
} // namespace pcbv
