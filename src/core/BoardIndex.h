// BoardIndex.h — derived indexes over a parsed PCB (nets, layer usage).
//
// Kept separate from the parsers so the binary and the ASCII readers produce exactly
// the same summaries: both call finalizeBoard() as their last step.
#pragma once

#include "Model.h"

namespace pcbv {

/// Recomputes Document::netSummaries and Document::layerUsage from the geometry that
/// was just parsed. Cheap enough to run once per document (a few 10k primitives).
void finalizeBoard(Document& document);

/// Human readable "1-2-1" style stackup description of the copper layers in use.
std::string copperStackup(const Document& document);

} // namespace pcbv
