// Bundle.h — the portable component library format of this viewer (".pcbvlib").
//
// An Altium .PcbLib / .SchLib is an OLE2 container that only Altium can produce, so the
// libraries shipped with this project use a plain text bundle instead: one line per
// record, exactly the key=value style the Altium ASCII exports use, plus the real STEP
// models appended as byte-exact blocks.
//
//   |PCBV-LIB|VERSION=1|TYPE=PCB|NAME=Starter library|LICENSE=CC-BY-SA-4.0|
//   |PART|NAME=TO-252-2|DESCRIPTION=DPAK|DATASHEET=https://…|MANUFACTURER=…|MPN=…|
//   |PAD|PART=0|NAME=1|X=0mil|Y=0mil|XSIZE=250mil|YSIZE=100mil|LAYER=1|HOLE=0|
//   |TRACK|PART=0|LAYER=33|X1=…|Y1=…|X2=…|Y2=…|WIDTH=10mil|
//   |ARC|PART=0|LAYER=33|X=…|Y=…|RADIUS=…|START=…|END=…|WIDTH=10mil|
//   |REGION|PART=0|LAYER=33|POINTS=x,y;x,y;…|
//   |STEP|PART=0|NAME=TO-252-2.step|BYTES=134002|
//   <134002 bytes of STEP text>
//
// A "TYPE=SCH" bundle carries |PIN| and |SHAPE| records instead of pads, so symbols use
// the same table and preview as a real .SchLib. Geometry is in mil, angles in degrees,
// which is what the rest of the viewer uses internally.
#pragma once

#include <string>
#include <vector>

#include "Library.h"

namespace pcbv {

/// True when the first bytes of a file are a bundle header ("|PCBV-LIB|").
bool looksLikeBundle(const std::vector<std::uint8_t>& head);

/// Reads a bundle into `library`. Returns false (with a reason) when the file is not a
/// bundle or has no recognisable records.
bool readBundle(const std::string& text, Library& library, std::string& error);

/// Serialises `library` as a bundle (used by the tools that build the shipped libraries
/// and by the smoke test, which writes a bundle and reads it back).
std::string writeBundle(const Library& library);

} // namespace pcbv
