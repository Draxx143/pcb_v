// Library.cpp — readers for Altium .PcbLib / .SchLib files and the ASCII library export.
//
// Binary .PcbLib layout (verified against real libraries):
//   FileHeader                       "PCB 6.0 Binary Library File" + library id
//   Library/Data                     [parameter block][u32 footprint count][name blocks]
//   <footprint>/Data                 [name block][objectId + block]* (pads, tracks, arcs …)
//   <footprint>/Parameters           "|PATTERN=…|HEIGHT=…|DESCRIPTION=…"
//
// Binary .SchLib layout:
//   FileHeader                       "|HEADER=…|CompCount=n|LibRef0=…|PartCount0=…"
//   <libref>/Data                    a record stream (same framing as a SchDoc)
//
// ASCII exports of both use exactly the same "|RECORD=…" lines as the ASCII documents.
#include "Library.h"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <map>
#include <sstream>

#include "AltiumInternal.h"
#include "AltiumText.h"
#include "BoardIndex.h"

namespace pcbv {
namespace {

using text::parseMil;
using text::parseNumber;
using text::valueOf;

constexpr std::uint32_t kBlockSizeMask = 0x00FFFFFF;

std::uint16_t rdU16(const std::vector<std::uint8_t>& b, std::size_t off) {
    if (off + 2 > b.size()) return 0;
    return static_cast<std::uint16_t>(b[off] | (b[off + 1] << 8));
}

std::uint32_t rdU32(const std::vector<std::uint8_t>& b, std::size_t off) {
    if (off + 4 > b.size()) return 0;
    return static_cast<std::uint32_t>(b[off]) | (static_cast<std::uint32_t>(b[off + 1]) << 8) |
           (static_cast<std::uint32_t>(b[off + 2]) << 16) |
           (static_cast<std::uint32_t>(b[off + 3]) << 24);
}

std::int32_t rdI32(const std::vector<std::uint8_t>& b, std::size_t off) {
    return static_cast<std::int32_t>(rdU32(b, off));
}

double rdF64(const std::vector<std::uint8_t>& b, std::size_t off) {
    if (off + 8 > b.size()) return 0.0;
    std::uint64_t raw = 0;
    for (int i = 7; i >= 0; --i) raw = (raw << 8) | b[off + static_cast<std::size_t>(i)];
    double value = 0.0;
    static_assert(sizeof(double) == sizeof(std::uint64_t), "unexpected double size");
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

std::uint8_t rdB(const std::vector<std::uint8_t>& b, std::size_t off) {
    return off < b.size() ? b[off] : 0;
}

/// Reads a size-prefixed block: [u32 size][size bytes]. Returns false at the end.
bool readBlock(const std::vector<std::uint8_t>& data, std::size_t& pos,
               std::vector<std::uint8_t>& out) {
    if (pos + 4 > data.size()) return false;
    const std::uint32_t size = rdU32(data, pos) & kBlockSizeMask;
    pos += 4;
    if (size == 0 || pos + size > data.size()) return false;
    out.assign(data.begin() + static_cast<std::ptrdiff_t>(pos),
               data.begin() + static_cast<std::ptrdiff_t>(pos + size));
    pos += size;
    return true;
}

/// Reads a "string block": [u32 size][u8 length][text]. Altium sometimes writes a size
/// that is one byte short, so the length is clamped to what is actually there.
std::string readStringBlock(const std::vector<std::uint8_t>& data, std::size_t& pos) {
    if (pos + 4 > data.size()) {
        pos = data.size();
        return {};
    }
    const std::uint32_t size = rdU32(data, pos) & kBlockSizeMask;
    pos += 4;
    if (size == 0) return {};
    const std::size_t available = std::min<std::size_t>(size, data.size() - pos);
    if (available == 0) {
        pos = std::min(pos + size, data.size());
        return {};
    }
    const std::size_t declared = data[pos];
    // Altium's size field sometimes counts the length byte but not the last character
    // (declared == size), so allow the string to run one byte past the block.
    const std::size_t readLength = std::min<std::size_t>(declared, data.size() - (pos + 1));
    const std::string result = text::fromCp1252(data.data() + pos + 1, readLength);
    const std::size_t overrun = declared >= size ? 1 : 0;
    pos = std::min<std::size_t>(pos + size + overrun, data.size());
    return result;
}

/// Parses a parameter text block ("|KEY=VALUE|…", usually NUL terminated).
std::vector<Property> parseParameterText(const std::vector<std::uint8_t>& block) {
    if (block.empty()) return {};
    std::size_t length = block.size();
    while (length > 0 && block[length - 1] == 0) --length;
    return text::parseParameters(block.data(), length);
}

double milFromRaw(std::int32_t raw) { return static_cast<double>(raw) / 10000.0; }

/// "TOP" / "BOTTOM" / "" from the layers the pads sit on (a footprint is surface mount
/// when all of its pads share one outer layer, through-hole when they sit on multi-layer).
std::string padLayerLabel(const std::vector<Pad>& pads) {
    int top = 0;
    int bottom = 0;
    int multi = 0;
    for (const Pad& pad : pads) {
        if (pad.layer == 1) ++top;
        else if (pad.layer == 32) ++bottom;
        else if (pad.layer == 74 || pad.holeSize > 0.0) ++multi;
    }
    if (multi > 0 || (top > 0 && bottom > 0)) return "BOTH";
    if (bottom > 0) return "BOTTOM";
    if (top > 0) return "TOP";
    return {};
}

// ---------------------------------------------------------------------------
// PcbLib
// ---------------------------------------------------------------------------

/// Reads one pad record: three string blocks, one skipped block, the main block and the
/// trailing solder-mask block (the same layout the PcbDoc reader uses).
bool readPadRecord(const std::vector<std::uint8_t>& data, std::size_t& pos, Pad& pad) {
    std::vector<std::uint8_t> block;
    if (pos >= data.size() || data[pos] != 2) return false;
    ++pos;

    for (int i = 0; i < 3; ++i) {
        const std::size_t before = pos;
        const std::string value = readStringBlock(data, pos);
        if (i == 0) pad.designator = value;
        if (pos == before) return false;
    }
    if (!readBlock(data, pos, block)) return false;  // padding block

    if (!readBlock(data, pos, block) || block.size() < 61) return false;
    pad.layer = rdB(block, 0);
    pad.layerName = text::pcbLayerName(pad.layer);
    pad.x = milFromRaw(rdI32(block, 13));
    pad.y = milFromRaw(rdI32(block, 17));
    pad.width = milFromRaw(rdI32(block, 21));
    pad.height = milFromRaw(rdI32(block, 25));
    pad.holeSize = milFromRaw(rdI32(block, 45));
    pad.rotation = rdF64(block, 52);
    pad.plated = rdB(block, 60) != 0;

    // Trailing sub-record (per-layer shapes); skip it when present.
    const std::size_t after = pos;
    if (readBlock(data, pos, block) && block.empty()) pos = after;
    return true;
}

void addRegionRecord(const std::vector<std::uint8_t>& block, LibraryFootprint& footprint) {
    if (block.size() < 22) return;
    const std::uint16_t holeCount = rdU16(block, 14);
    std::size_t pos = 18;
    std::vector<std::uint8_t> params;
    if (!readBlock(block, pos, params)) return;
    if (pos + 4 > block.size()) return;
    const std::uint32_t vertexCount = rdU32(block, pos);
    pos += 4;
    RegionFeature region;
    region.layer = rdB(block, 0);
    for (std::uint32_t i = 0; i < vertexCount; ++i) {
        if (pos + 16 > block.size()) break;
        Point point;
        // Region vertices are 1/10000 mil stored as doubles.
        point.x = rdF64(block, pos) / 10000.0;
        point.y = rdF64(block, pos + 8) / 10000.0;
        pos += 16;
        region.outline.push_back(point);
        footprint.bounds.add(point.x, point.y);
    }
    (void)holeCount;
    if (region.outline.size() < 3) return;
    footprint.geometry.regions.push_back(region);
    ++footprint.regionCount;
}

/// Walks the primitive records of one footprint ("<name>/Data", after the name block).
void readFootprintPrimitives(const std::vector<std::uint8_t>& data, std::size_t start,
                             LibraryFootprint& footprint, std::vector<std::string>& warnings,
                             int& unknownRecords) {
    std::size_t pos = start;
    std::vector<std::uint8_t> block;
    while (pos < data.size()) {
        const std::size_t before = pos;
        const std::uint8_t objectId = data[pos];

        switch (objectId) {
            case 1: {  // arc
                ++pos;
                if (!readBlock(data, pos, block) || block.size() < 45) break;
                ArcFeature arc;
                arc.layer = rdB(block, 0);
                arc.cx = milFromRaw(rdI32(block, 13));
                arc.cy = milFromRaw(rdI32(block, 17));
                arc.radius = milFromRaw(rdI32(block, 21));
                arc.startAngle = rdF64(block, 25);
                arc.endAngle = rdF64(block, 33);
                arc.width = milFromRaw(rdI32(block, 41));
                footprint.geometry.arcs.push_back(arc);
                ++footprint.arcCount;
                if (arc.radius > 0.0) {
                    footprint.bounds.add(arc.cx - arc.radius, arc.cy - arc.radius);
                    footprint.bounds.add(arc.cx + arc.radius, arc.cy + arc.radius);
                }
                break;
            }
            case 2: {  // pad
                Pad pad;
                if (!readPadRecord(data, pos, pad)) {
                    ++unknownRecords;
                    break;
                }
                footprint.geometry.pads.push_back(pad);
                ++footprint.padCount;
                footprint.bounds.add(pad.x - pad.width / 2.0, pad.y - pad.height / 2.0);
                footprint.bounds.add(pad.x + pad.width / 2.0, pad.y + pad.height / 2.0);
                break;
            }
            case 3: {  // via
                ++pos;
                if (!readBlock(data, pos, block) || block.size() < 29) break;
                ViaFeature via;
                via.x = milFromRaw(rdI32(block, 13));
                via.y = milFromRaw(rdI32(block, 17));
                via.diameter = milFromRaw(rdI32(block, 21));
                via.hole = milFromRaw(rdI32(block, 25));
                footprint.geometry.vias.push_back(via);
                ++footprint.viaCount;
                footprint.bounds.add(via.x - via.diameter / 2.0, via.y - via.diameter / 2.0);
                footprint.bounds.add(via.x + via.diameter / 2.0, via.y + via.diameter / 2.0);
                break;
            }
            case 4: {  // track
                ++pos;
                if (!readBlock(data, pos, block) || block.size() < 33) break;
                TrackFeature track;
                track.layer = rdB(block, 0);
                track.x1 = milFromRaw(rdI32(block, 13));
                track.y1 = milFromRaw(rdI32(block, 17));
                track.x2 = milFromRaw(rdI32(block, 21));
                track.y2 = milFromRaw(rdI32(block, 25));
                track.width = milFromRaw(rdI32(block, 29));
                footprint.geometry.tracks.push_back(track);
                ++footprint.trackCount;
                footprint.bounds.add(track.x1, track.y1);
                footprint.bounds.add(track.x2, track.y2);
                break;
            }
            case 5: {  // text (position/size live in a fixed block we only count)
                ++pos;
                if (!readBlock(data, pos, block)) break;
                ++footprint.textCount;
                break;
            }
            case 6: {  // fill
                ++pos;
                if (!readBlock(data, pos, block) || block.size() < 37) break;
                FillFeature fill;
                fill.layer = rdB(block, 0);
                fill.x1 = milFromRaw(rdI32(block, 13));
                fill.y1 = milFromRaw(rdI32(block, 17));
                fill.x2 = milFromRaw(rdI32(block, 21));
                fill.y2 = milFromRaw(rdI32(block, 25));
                fill.rotation = rdF64(block, 29);
                footprint.geometry.fills.push_back(fill);
                ++footprint.fillCount;
                footprint.bounds.add(fill.x1, fill.y1);
                footprint.bounds.add(fill.x2, fill.y2);
                break;
            }
            case 11: {  // region / polygon
                ++pos;
                if (!readBlock(data, pos, block)) break;
                addRegionRecord(block, footprint);
                break;
            }
            case 12: {  // 3D component body
                ++pos;
                if (!readBlock(data, pos, block)) break;
                ++footprint.bodyCount;
                break;
            }
            default: {
                // Unknown object: skip its block so the rest of the footprint still parses.
                ++pos;
                if (!readBlock(data, pos, block)) pos = data.size();
                ++unknownRecords;
                break;
            }
        }

        if (pos == before) {  // never spin on a malformed record
            ++unknownRecords;
            pos = before + 1;
        }
    }
    if (unknownRecords > 0) {
        warnings.push_back("Footprint '" + footprint.name + "': " +
                           std::to_string(unknownRecords) +
                           " primitive record(s) could not be decoded and were skipped.");
    }
}

bool readPcbLibrary(CompoundFile& file, Library& library, std::string& error) {
    library.type = DocType::PcbLib;
    library.container = Container::CompoundBinary;
    library.structure = file.root().children;

    const std::vector<std::uint8_t> header = file.stream("FileHeader");
    if (!header.empty()) {
        std::size_t pos = 0;
        const std::string version = readStringBlock(header, pos);
        if (!version.empty()) library.formatVersion = version;
    }
    if (library.formatVersion.empty()) library.formatVersion = "PCB binary library (OLE2)";

    std::vector<std::uint8_t> data = file.stream("Library/Data");
    if (data.empty()) {
        error = "This .PcbLib has no 'Library/Data' stream — the file may be damaged.";
        return false;
    }

    std::vector<std::uint8_t> block;
    std::size_t pos = 0;
    if (!readBlock(data, pos, block)) {
        error = "The .PcbLib library header could not be decoded.";
        return false;
    }
    const std::vector<Property> libraryParameters = parseParameterText(block);
    (void)libraryParameters;

    if (pos + 4 > data.size()) {
        error = "The .PcbLib library header is truncated (no footprint count).";
        return false;
    }
    const std::uint32_t footprintCount = rdU32(data, pos);
    pos += 4;

    std::vector<std::string> names;
    for (std::uint32_t i = 0; i < footprintCount && pos < data.size(); ++i) {
        const std::string name = readStringBlock(data, pos);
        if (!name.empty()) names.push_back(name);
    }
    if (names.empty()) {
        // Fall back to the stored names: every storage under the root is a footprint,
        // apart from the bookkeeping ones Altium always writes.
        for (const FileEntry& entry : file.root().children) {
            if (!entry.isStorage) continue;
            if (entry.name == "Library" || entry.name == "FileVersionInfo") continue;
            names.push_back(entry.name);
        }
    }

    for (const std::string& name : names) {
        LibraryFootprint footprint;
        footprint.name = name;
        const std::string base = name;

        // Parameters stream (PATTERN / HEIGHT / DESCRIPTION / …).
        std::vector<std::uint8_t> parametersText = file.stream(base + "/Parameters");
        if (!parametersText.empty()) {
            std::size_t parametersPos = 0;
            std::vector<std::uint8_t> parametersBlock;
            if (!readBlock(parametersText, parametersPos, parametersBlock))
                parametersBlock = parametersText;
            footprint.parameters = parseParameterText(parametersBlock);
            footprint.description = valueOf(footprint.parameters, "DESCRIPTION");
            footprint.itemGuid = valueOf(footprint.parameters, "ITEMGUID");
            footprint.height = parseMil(valueOf(footprint.parameters, "HEIGHT"));
        }

        std::vector<std::uint8_t> stream = file.stream(base + "/Data");
        if (stream.empty()) {
            library.warnings.push_back("Footprint '" + name + "' has no data stream.");
            library.footprints.push_back(std::move(footprint));
            continue;
        }

        std::size_t namePos = 0;
        const std::string storedName = readStringBlock(stream, namePos);
        if (!storedName.empty()) footprint.name = storedName;

        int unknown = 0;
        readFootprintPrimitives(stream, namePos, footprint, library.warnings, unknown);
        library.footprints.push_back(std::move(footprint));
    }

    if (library.footprints.empty()) {
        error = "No footprints were found inside this .PcbLib.";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// SchLib
// ---------------------------------------------------------------------------

bool readSchLibrary(CompoundFile& file, Library& library, std::string& error) {
    library.type = DocType::SchLib;
    library.container = Container::CompoundBinary;
    library.structure = file.root().children;

    std::vector<std::string> libRefs;
    const std::vector<std::uint8_t> header = file.stream("FileHeader");
    if (!header.empty()) {
        std::size_t pos = 0;
        std::vector<std::uint8_t> headerBlock;
        const std::string headerText = readStringBlock(header, pos);
        if (!readBlock(header, pos = 0, headerBlock)) headerBlock = header;
        const std::vector<Property> headerProps = parseParameterText(headerBlock);
        const std::string version = valueOf(headerProps, "HEADER");
        library.formatVersion = !version.empty() ? version
                                : !headerText.empty() ? headerText
                                                      : std::string();
        const int count = text::parseInt(valueOf(headerProps, "COMPCOUNT"), 0);
        for (int i = 0; i < count; ++i) {
            const std::string name = valueOf(headerProps, "LIBREF" + std::to_string(i));
            if (!name.empty()) libRefs.push_back(name);
        }
    }
    if (library.formatVersion.empty()) library.formatVersion = "Schematic library (OLE2)";

    // The OLE storage name is limited to 31 characters, so the library reference is the
    // human readable name while the storage name is what has to be looked up.
    std::vector<std::string> storages;
    for (const FileEntry& entry : file.root().children) {
        if (!entry.isStorage) continue;
        if (entry.name == "Library" || entry.name == "Storage" ||
            entry.name == "FileVersionInfo" || entry.name == "Models")
            continue;
        storages.push_back(entry.name);
    }

    for (std::size_t i = 0; i < storages.size(); ++i) {
        LibrarySymbol symbol;
        symbol.name = i < libRefs.size() ? libRefs[i] : storages[i];
        const std::string storageName = storages[i];

        const std::vector<std::uint8_t> stream = file.stream(storageName + "/Data");
        if (stream.empty()) {
            library.warnings.push_back("Symbol '" + symbol.name + "' has no data stream.");
            library.symbols.push_back(std::move(symbol));
            continue;
        }

        const std::vector<detail::SchRecord> records = detail::readSchRecordStream(stream);
        Document temporary;
        detail::buildSchDocument(records, temporary);
        if (!temporary.components.empty()) {
            const Component& component = temporary.components.front();
            if (!component.name.empty()) symbol.name = component.name;
            symbol.description = component.description;
            symbol.partCount = component.partCount > 0 ? component.partCount : 1;
            symbol.pins = component.pins;
            symbol.parameters = component.properties;
            symbol.designatorPrefix = component.designator;
        }
        // The default designator lives in its own record; take it from the parameters.
        for (const Property& property : symbol.parameters) {
            if (text::equalsNoCase(property.name, "DESIGNATOR")) {
                symbol.designatorPrefix = property.value;
                break;
            }
        }
        if (symbol.pins.empty() && symbol.parameters.empty()) {
            library.warnings.push_back("Symbol '" + symbol.name +
                                       "' was found but contains no readable records.");
        }
        library.symbols.push_back(std::move(symbol));
    }

    if (library.symbols.empty()) {
        error = "No symbols were found inside this .SchLib.";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// ASCII exports
// ---------------------------------------------------------------------------

std::string asciiText(const std::vector<std::uint8_t>& bytes) {
    const std::size_t length = std::min<std::size_t>(bytes.size(), 4096);
    return std::string(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(length));
}

void readAsciiPcbLibrary(const std::vector<std::uint8_t>& bytes, Library& library) {
    const std::vector<std::vector<Property>> records = detail::readAsciiRecords(bytes);

    std::map<int, std::size_t> byId;  // COMPONENT id -> index in library.footprints
    for (const std::vector<Property>& props : records) {
        if (!text::equalsNoCase(valueOf(props, "RECORD"), "Component")) continue;
        LibraryFootprint footprint;
        footprint.name = valueOf(props, "PATTERN");
        if (footprint.name.empty()) footprint.name = valueOf(props, "SOURCEDESIGNATOR");
        if (footprint.name.empty()) footprint.name = "Footprint " + std::to_string(library.footprints.size() + 1);
        footprint.description = valueOf(props, "DESCRIPTION");
        footprint.height = parseMil(valueOf(props, "HEIGHT"));
        footprint.parameters = props;
        const int id = text::parseInt(valueOf(props, "ID"), static_cast<int>(library.footprints.size()));
        byId[id] = library.footprints.size();
        library.footprints.push_back(std::move(footprint));
    }

    for (const std::vector<Property>& props : records) {
        const std::string type = valueOf(props, "RECORD");
        const int owner = text::parseInt(valueOf(props, "COMPONENT"), -1);
        const auto it = byId.find(owner);
        if (it == byId.end()) continue;
        LibraryFootprint& footprint = library.footprints[it->second];
        const int layer = text::pcbLayerIdFromName(valueOf(props, "LAYER"));

        if (text::equalsNoCase(type, "Pad")) {
            Pad pad;
            pad.designator = valueOf(props, "NAME");
            if (pad.designator.empty()) pad.designator = valueOf(props, "DESIGNATOR");
            pad.layer = layer;
            pad.layerName = text::pcbLayerName(layer);
            pad.x = parseMil(valueOf(props, "X"));
            pad.y = parseMil(valueOf(props, "Y"));
            pad.width = parseMil(valueOf(props, "XSIZE"));
            pad.height = parseMil(valueOf(props, "YSIZE"));
            pad.holeSize = parseMil(valueOf(props, "HOLESIZE"));
            pad.rotation = parseNumber(valueOf(props, "ROTATION"));
            footprint.geometry.pads.push_back(pad);
            ++footprint.padCount;
            footprint.bounds.add(pad.x - pad.width / 2.0, pad.y - pad.height / 2.0);
            footprint.bounds.add(pad.x + pad.width / 2.0, pad.y + pad.height / 2.0);
        } else if (text::equalsNoCase(type, "Track")) {
            TrackFeature track;
            track.layer = layer;
            track.x1 = parseMil(valueOf(props, "X1"));
            track.y1 = parseMil(valueOf(props, "Y1"));
            track.x2 = parseMil(valueOf(props, "X2"));
            track.y2 = parseMil(valueOf(props, "Y2"));
            track.width = parseMil(valueOf(props, "WIDTH"));
            footprint.geometry.tracks.push_back(track);
            ++footprint.trackCount;
            footprint.bounds.add(track.x1, track.y1);
            footprint.bounds.add(track.x2, track.y2);
        } else if (text::equalsNoCase(type, "Arc")) {
            ArcFeature arc;
            arc.layer = layer;
            arc.cx = parseMil(valueOf(props, "LOCATION.X"));
            arc.cy = parseMil(valueOf(props, "LOCATION.Y"));
            arc.radius = parseMil(valueOf(props, "RADIUS"));
            arc.startAngle = parseNumber(valueOf(props, "STARTANGLE"));
            arc.endAngle = parseNumber(valueOf(props, "ENDANGLE"));
            arc.width = parseMil(valueOf(props, "WIDTH"));
            footprint.geometry.arcs.push_back(arc);
            ++footprint.arcCount;
            if (arc.radius > 0.0) {
                footprint.bounds.add(arc.cx - arc.radius, arc.cy - arc.radius);
                footprint.bounds.add(arc.cx + arc.radius, arc.cy + arc.radius);
            }
        } else if (text::equalsNoCase(type, "Via")) {
            ViaFeature via;
            via.x = parseMil(valueOf(props, "X"));
            via.y = parseMil(valueOf(props, "Y"));
            via.diameter = parseMil(valueOf(props, "DIAMETER"));
            via.hole = parseMil(valueOf(props, "HOLESIZE"));
            footprint.geometry.vias.push_back(via);
            ++footprint.viaCount;
            footprint.bounds.add(via.x, via.y);
        } else if (text::equalsNoCase(type, "Text")) {
            ++footprint.textCount;
        } else if (text::equalsNoCase(type, "Fill")) {
            FillFeature fill;
            fill.layer = layer;
            fill.x1 = parseMil(valueOf(props, "X1"));
            fill.y1 = parseMil(valueOf(props, "Y1"));
            fill.x2 = parseMil(valueOf(props, "X2"));
            fill.y2 = parseMil(valueOf(props, "Y2"));
            fill.rotation = parseNumber(valueOf(props, "ROTATION"));
            footprint.geometry.fills.push_back(fill);
            ++footprint.fillCount;
            footprint.bounds.add(fill.x1, fill.y1);
            footprint.bounds.add(fill.x2, fill.y2);
        }
    }

    if (library.footprints.empty()) {
        library.warnings.push_back("No RECORD=Component entries were found in this ASCII library.");
    }
}

void readAsciiSchLibrary(const std::vector<std::uint8_t>& bytes, Library& library) {
    Document temporary;
    detail::parseAsciiSch(bytes, temporary);
    library.formatVersion = temporary.formatVersion;

    for (const Component& component : temporary.components) {
        LibrarySymbol symbol;
        symbol.name = component.name.empty() ? component.comment : component.name;
        if (symbol.name.empty()) symbol.name = "Symbol " + std::to_string(library.symbols.size() + 1);
        symbol.description = component.description;
        symbol.partCount = component.partCount > 0 ? component.partCount : 1;
        symbol.pins = component.pins;
        symbol.parameters = component.properties;
        library.symbols.push_back(std::move(symbol));
    }
    for (const std::string& warning : temporary.warnings) library.warnings.push_back(warning);
    if (library.symbols.empty()) {
        library.warnings.push_back("No RECORD=1 (symbol) entries were found in this ASCII library.");
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool readBinaryLibrary(CompoundFile& file, Library& library, std::string& error) {
    if (library.type == DocType::PcbLib) return readPcbLibrary(file, library, error);
    if (library.type == DocType::SchLib) return readSchLibrary(file, library, error);
    error = "Only .PcbLib and .SchLib libraries are supported.";
    return false;
}

void readAsciiLibrary(const std::vector<std::uint8_t>& bytes, Library& library) {
    library.container = Container::AsciiText;
    if (library.formatVersion.empty()) {
        library.formatVersion = library.type == DocType::PcbLib ? "PCB ASCII library"
                                                                : "Schematic ASCII library";
    }
    if (library.type == DocType::PcbLib) {
        readAsciiPcbLibrary(bytes, library);
    } else {
        readAsciiSchLibrary(bytes, library);
    }
    if (library.formatVersion.empty()) {
        const std::string head = asciiText(bytes);
        if (!head.empty()) library.formatVersion = head;
    }
}

void buildLibraryDocument(Document& document, Library& library) {
    document.type = library.type;
    document.container = library.container;
    document.formatVersion = library.formatVersion;
    document.structure = library.structure;
    document.components.clear();

    if (library.isFootprintLibrary()) {
        for (const LibraryFootprint& footprint : library.footprints) {
            Component component;
            component.designator = footprint.name;      // a library item has no designator
            component.name = footprint.name;
            component.footprint = footprint.name;
            component.comment = footprint.description;
            component.description = footprint.description;
            component.height = footprint.height;
            component.padCount = footprint.padCount;
            component.pads = footprint.geometry.pads;
            component.properties = footprint.parameters;
            for (const Pad& pad : footprint.geometry.pads) {
                if (pad.holeSize > 0.0) ++component.throughHolePads;
            }
            component.layer = padLayerLabel(footprint.geometry.pads);
            document.components.push_back(std::move(component));
        }
        document.pcb.pads = 0;
        document.pcb.tracks = 0;
        document.pcb.arcs = 0;
        document.pcb.vias = 0;
        document.pcb.texts = 0;
        document.pcb.regions = 0;
        document.pcb.fills = 0;
        for (const LibraryFootprint& footprint : library.footprints) {
            document.pcb.pads += footprint.padCount;
            document.pcb.tracks += footprint.trackCount;
            document.pcb.arcs += footprint.arcCount;
            document.pcb.vias += footprint.viaCount;
            document.pcb.texts += footprint.textCount;
            document.pcb.regions += footprint.regionCount;
            document.pcb.fills += footprint.fillCount;
            document.pcb.componentBodies += footprint.bodyCount;
        }
    } else {
        for (const LibrarySymbol& symbol : library.symbols) {
            Component component;
            component.designator = symbol.name;
            component.name = symbol.name;
            component.description = symbol.description;
            component.pinCount = static_cast<int>(symbol.pins.size());
            component.pins = symbol.pins;
            component.partCount = symbol.partCount;
            component.properties = symbol.parameters;
            document.components.push_back(std::move(component));
        }
        document.sch.pins = 0;
        for (const LibrarySymbol& symbol : library.symbols) {
            document.sch.pins += static_cast<int>(symbol.pins.size());
        }
        document.sch.components = static_cast<int>(library.symbols.size());
    }

    for (const std::string& warning : library.warnings) document.warnings.push_back(warning);
}

Document footprintDocument(const LibraryFootprint& footprint, const std::string& fileName) {
    Document document;
    document.fileName = fileName + " · " + footprint.name;
    document.type = DocType::PcbLib;
    document.container = Container::CompoundBinary;
    document.formatVersion = "Footprint preview";

    Component component;
    component.designator = footprint.name;
    component.name = footprint.name;
    component.footprint = footprint.name;
    component.comment = footprint.description;
    component.description = footprint.description;
    component.height = footprint.height;
    component.padCount = footprint.padCount;
    component.pads = footprint.geometry.pads;
    component.layer = padLayerLabel(footprint.geometry.pads);
    document.components.push_back(std::move(component));

    // Pads belong to this single component.
    for (Pad& pad : document.components.front().pads) pad.componentIndex = 0;
    document.geometry = footprint.geometry;
    for (Pad& pad : document.geometry.pads) pad.componentIndex = 0;

    auto addExtent = [&document](double x, double y) { document.bounds.add(x, y); };
    for (const TrackFeature& track : document.geometry.tracks) {
        addExtent(track.x1, track.y1);
        addExtent(track.x2, track.y2);
    }
    for (const ArcFeature& arc : document.geometry.arcs) {
        addExtent(arc.cx - arc.radius, arc.cy - arc.radius);
        addExtent(arc.cx + arc.radius, arc.cy + arc.radius);
    }
    for (const ViaFeature& via : document.geometry.vias) addExtent(via.x, via.y);
    for (const FillFeature& fill : document.geometry.fills) {
        addExtent(fill.x1, fill.y1);
        addExtent(fill.x2, fill.y2);
    }
    for (const RegionFeature& region : document.geometry.regions) {
        for (const Point& point : region.outline) addExtent(point.x, point.y);
    }
    for (const Pad& pad : document.geometry.pads) {
        addExtent(pad.x - pad.width / 2.0, pad.y - pad.height / 2.0);
        addExtent(pad.x + pad.width / 2.0, pad.y + pad.height / 2.0);
    }

    document.pcb.pads = static_cast<int>(document.geometry.pads.size());
    document.pcb.tracks = static_cast<int>(document.geometry.tracks.size());
    document.pcb.arcs = static_cast<int>(document.geometry.arcs.size());
    document.pcb.vias = static_cast<int>(document.geometry.vias.size());
    document.pcb.regions = static_cast<int>(document.geometry.regions.size());
    document.pcb.fills = static_cast<int>(document.geometry.fills.size());

    finalizeBoard(document);
    return document;
}

namespace {
/// Shortens a name so table columns stay aligned.
std::string ellipsize(const std::string& value, std::size_t width) {
    if (value.size() <= width) return value;
    return value.substr(0, width > 3 ? width - 3 : 0) + "...";
}
} // namespace

std::string libraryToText(const Library& library) {
    std::ostringstream out;
    out << "Library   " << (library.isFootprintLibrary() ? "PCB footprints (.PcbLib)"
                                                         : "schematic symbols (.SchLib)") << "\n";
    out << "Format    " << library.formatVersion << "\n";
    out << "Items     " << library.itemCount() << "  (" << librarySummary(library) << ")\n";

    if (library.isFootprintLibrary()) {
        out << "\n" << std::left;
        out << std::setw(34) << "Footprint" << std::right << std::setw(6) << "Pads"
            << std::setw(7) << "Track" << std::setw(5) << "Arc" << std::setw(5) << "Via"
            << ' ' << "      Size mm" << "   Description\n";
        out << std::string(96, '-') << "\n";
        for (const LibraryFootprint& footprint : library.footprints) {
            std::ostringstream size;
            if (footprint.bounds.valid) {
                size.setf(std::ios::fixed);
                size.precision(2);
                size << footprint.bounds.width() * 0.0254 << " x "
                     << footprint.bounds.height() * 0.0254;
            } else {
                size << "-";
            }
            std::string description = footprint.description;
            if (description.size() > 44) description = description.substr(0, 41) + "...";
            std::string sizeText = size.str();
            if (sizeText.size() < 13) sizeText = std::string(13 - sizeText.size(), ' ') + sizeText;
            out << std::left << std::setw(34) << ellipsize(footprint.name, 33) << std::right << std::setw(6)
                << footprint.padCount << std::setw(7) << footprint.trackCount << std::setw(5)
                << footprint.arcCount << std::setw(5) << footprint.viaCount << ' ' << sizeText
                << "   " << description << "\n";
        }
    } else {
        out << "\n" << std::left;
        out << std::setw(34) << "Symbol" << std::right << std::setw(6) << "Pins"
            << std::setw(7) << "Parts" << "   Description\n";
        out << std::string(96, '-') << "\n";
        for (const LibrarySymbol& symbol : library.symbols) {
            std::string description = symbol.description;
            if (description.size() > 44) description = description.substr(0, 41) + "...";
            out << std::left << std::setw(34) << ellipsize(symbol.name, 33) << std::right << std::setw(6)
                << symbol.pins.size() << std::setw(7) << symbol.partCount << "   " << description
                << "\n";
        }
    }
    if (!library.warnings.empty()) {
        out << "\nWarnings\n";
        for (const std::string& warning : library.warnings) out << "  * " << warning << "\n";
    }
    return out.str();
}

std::string libraryToCsv(const Library& library) {
    std::ostringstream out;
    out << "Kind,Name,Pads,Pins,Tracks,Arcs,Vias,Width (mm),Height (mm),Description,File\n";
    auto quote = [](const std::string& value) {
        if (value.find_first_of(",\"\n") == std::string::npos) return value;
        std::string quoted = "\"";
        for (char c : value) {
            if (c == '"') quoted += "\"\"";
            else quoted += c;
        }
        quoted += '"';
        return quoted;
    };
    auto number = [](double mil) {
        std::ostringstream value;
        value.setf(std::ios::fixed);
        value.precision(3);
        value << mil * 0.0254;
        return value.str();
    };
    if (library.isFootprintLibrary()) {
        for (const LibraryFootprint& footprint : library.footprints) {
            out << "Footprint," << quote(footprint.name) << ',' << footprint.padCount << ",,"
                << footprint.trackCount << ',' << footprint.arcCount << ',' << footprint.viaCount
                << ',' << (footprint.bounds.valid ? number(footprint.bounds.width()) : "")
                << ',' << (footprint.bounds.valid ? number(footprint.bounds.height()) : "") << ','
                << quote(footprint.description) << ',' << quote(library.fileName) << '\n';
        }
    } else {
        for (const LibrarySymbol& symbol : library.symbols) {
            out << "Symbol," << quote(symbol.name) << ",," << symbol.pins.size() << ",,,,"
                << symbol.partCount << ',' << quote(symbol.description) << ','
                << quote(library.fileName) << '\n';
        }
    }
    return out.str();
}

std::string librarySummary(const Library& library) {
    std::ostringstream out;
    out << (library.isFootprintLibrary() ? "PcbLib" : "SchLib") << " · ";
    if (library.isFootprintLibrary()) {
        out << library.footprints.size() << " footprint";
        if (library.footprints.size() != 1) out << 's';
    } else {
        out << library.symbols.size() << " symbol";
        if (library.symbols.size() != 1) out << 's';
    }
    return out.str();
}

} // namespace pcbv
