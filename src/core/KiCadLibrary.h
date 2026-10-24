// KiCadLibrary.h — reads the open KiCad formats into the viewer's library model.
//
//   .kicad_mod  one footprint  (pads, silkscreen/copper graphics, 3D model reference)
//   .kicad_sym  a symbol library (symbols, pins, body graphics, properties)
//
// Both are S-expressions; `SExpr.h` turns them into a tree and this file maps that tree onto
// `LibraryFootprint` / `LibrarySymbol`, the same structures the Altium reader fills. That way
// the tables, the board preview, the SVG export and the 3D renderer work without knowing
// which CAD tool the file came from.
//
// Units: KiCad stores millimetres. Pad and graphic *positions* in a footprint are given with
// Y pointing down, while the viewer's model uses Y up, so Y is flipped. Symbol coordinates
// already point up. Everything the rest of the program sees is in mil.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Library.h"

namespace pcbv {

/// True when the first bytes look like one of the KiCad S-expression files.
bool looksLikeKiCad(const std::vector<std::uint8_t>& head);

/// Reads one file into `library` (one footprint, or every symbol of the file).
/// Returns false with a reason when the text is not a KiCad footprint/symbol library.
bool readKiCadLibrary(const std::string& text, const std::string& fileName, Library& library,
                      std::string& error);

/// Reads a single footprint from its text (used by the tests and by the footprint preview).
bool readKiCadFootprint(const std::string& text, LibraryFootprint& footprint,
                        std::string& error);

/// Reads every symbol of a `.kicad_sym` text.
bool readKiCadSymbols(const std::string& text, std::vector<LibrarySymbol>& symbols,
                      std::string& error);

} // namespace pcbv
