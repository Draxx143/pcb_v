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
#include <filesystem>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <map>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "AltiumInternal.h"
#include "AltiumParser.h"
#include "Inflate.h"
#include "Step.h"
#include "AltiumText.h"
#include "BoardIndex.h"

namespace pcbv {
namespace {

using text::parseMil;
using text::parseNumber;
using text::valueOf;

constexpr std::uint32_t kBlockSizeMask = 0x00FFFFFF;

// Defined further down, next to the schematic record handling.
void buildSymbols(const std::vector<detail::SchRecord>& records, Library& library);

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

/// Reads the 3D models of a library. Altium stores each model as a zlib compressed STEP
/// file under the "Models" storage ("0", "1", …) and keeps its name, rotation and offset in
/// the "Models/Data" parameter block. This is the real part data — not a guess.
std::vector<EmbeddedModel> readEmbeddedModels(CompoundFile& file) {
    std::vector<EmbeddedModel> models;
    // Models live under the library storage in binary PcbLib files.
    const FileEntry* library = file.root().find("Library");
    const FileEntry* storage = library != nullptr ? library->find("Models")
                                                 : file.root().find("Models");
    if (storage == nullptr) return models;
    const std::string base = storage->path.empty() ? std::string("Models") : storage->path;

    // Model names/placements, in the same order as the numeric stream names.
    std::vector<std::vector<Property>> parameters;
    {
        const std::vector<std::uint8_t> data = file.stream(base + "/Data");
        std::size_t start = 0;
        for (std::size_t i = 0; i <= data.size(); ++i) {
            if (i < data.size() && data[i] != 0) continue;
            if (i > start) {
                std::vector<Property> block =
                    text::parseParameters(data.data() + start, i - start);
                if (!block.empty()) parameters.push_back(std::move(block));
            }
            start = i + 1;
        }
    }

    // Numeric stream names sorted by their value: "0", "1", "2", …
    std::vector<std::pair<int, std::string>> streams;
    for (const FileEntry& entry : storage->children) {
        if (entry.isStorage) continue;
        bool numeric = !entry.name.empty();
        for (const char c : entry.name) {
            if (c < '0' || c > '9') numeric = false;
        }
        if (!numeric) continue;
        streams.emplace_back(std::atoi(entry.name.c_str()), entry.name);
    }
    std::sort(streams.begin(), streams.end());

    for (std::size_t index = 0; index < streams.size(); ++index) {
        const std::string path = base + "/" + streams[index].second;
        const std::vector<std::uint8_t> raw = file.stream(path);
        if (raw.empty()) continue;

        std::vector<std::uint8_t> decoded;
        if (!inflateAny(raw.data(), raw.size(), decoded)) continue;
        const std::string stepText(reinterpret_cast<const char*>(decoded.data()), decoded.size());
        if (!looksLikeStep(stepText)) continue;

        EmbeddedModel model;
        model.stepText = stepText;
        model.name = "model " + streams[index].second;
        if (index < parameters.size()) {
            const std::vector<Property>& block = parameters[index];
            const std::string name = valueOf(block, "NAME");
            if (!name.empty()) model.name = name;
            model.id = valueOf(block, "ID");
            model.rotationX = text::parseNumber(valueOf(block, "ROTX"));
            model.rotationY = text::parseNumber(valueOf(block, "ROTY"));
            model.rotationZ = text::parseNumber(valueOf(block, "ROTZ"));
            model.dz = text::parseNumber(valueOf(block, "DZ"));
        }
        models.push_back(std::move(model));
    }
    return models;
}

/// Every model id the footprint refers to ("MODELID={…}" inside its 3D body record).
std::vector<std::string> modelIds(const std::vector<std::uint8_t>& data) {
    static const std::string marker = "MODELID=";
    std::vector<std::string> ids;
    const std::string text(reinterpret_cast<const char*>(data.data()), data.size());
    std::size_t pos = 0;
    while ((pos = text.find(marker, pos)) != std::string::npos) {
        pos += marker.size();
        const std::size_t end = text.find_first_of("|\0", pos);
        const std::string id = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (!id.empty()) ids.push_back(id);
        if (end == std::string::npos) break;
        pos = end;
    }
    return ids;
}

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

    // Trailing sub-record (per-layer shapes): always consume its size prefix, even when it
    // is empty (size 0), otherwise the next record would start one byte into the size field.
    if (pos + 4 <= data.size()) {
        const std::uint32_t tailSize = rdU32(data, pos) & kBlockSizeMask;
        if (pos + 4 + tailSize <= data.size()) pos += 4 + tailSize;
    }
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

    // The OLE storage name is limited to 31 characters while the library keeps the full
    // footprint name, so the storage has to be matched (by name, then by position).
    std::vector<std::string> storages;
    for (const FileEntry& entry : file.root().children) {
        if (!entry.isStorage) continue;
        if (entry.name == "Library" || entry.name == "FileVersionInfo" || entry.name == "Models")
            continue;
        storages.push_back(entry.name);
    }
    if (names.empty()) names = storages;

    auto storageFor = [&file, &storages](const std::string& name, std::size_t index) {
        if (file.exists(name + "/Data")) return name;
        const std::string shortened = name.substr(0, 31);
        for (const std::string& candidate : storages) {
            if (candidate == shortened || name.compare(0, candidate.size(), candidate) == 0)
                return candidate;
        }
        if (index < storages.size()) return storages[index];
        return name;
    };

    // Real 3D models, shared by the footprints that refer to them.
    const std::vector<EmbeddedModel> models = readEmbeddedModels(file);

    for (std::size_t index = 0; index < names.size(); ++index) {
        const std::string& name = names[index];
        LibraryFootprint footprint;
        footprint.name = name;
        const std::string base = storageFor(name, index);

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

        // The 3D body record names its model ("MODELID={…}"), so the right model can be
        // attached without guessing.
        for (const std::string& id : modelIds(stream)) {
            for (const EmbeddedModel& model : models) {
                if (!model.id.empty() && model.id == id) {
                    footprint.models.push_back(model);
                    break;
                }
            }
        }
        if (footprint.models.empty() && models.size() == 1 && footprint.bodyCount > 0) {
            footprint.models = models;   // a single model library: it is the body
        }
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
        const std::string storageName = storages[i];
        const std::vector<std::uint8_t> stream = file.stream(storageName + "/Data");
        if (stream.empty()) {
            LibrarySymbol symbol;
            symbol.name = i < libRefs.size() ? libRefs[i] : storageName;
            library.warnings.push_back("Symbol '" + symbol.name + "' has no data stream.");
            library.symbols.push_back(std::move(symbol));
            continue;
        }

        const std::size_t before = library.symbols.size();
        buildSymbols(detail::readSchRecordStream(stream), library);

        for (std::size_t index = before; index < library.symbols.size(); ++index) {
            LibrarySymbol& symbol = library.symbols[index];
            // The full name: the record's LibReference, else the FileHeader LibRef entry,
            // else the (31 character) storage name.
            if (symbol.name.empty()) symbol.name = i < libRefs.size() ? libRefs[i] : storageName;
            for (const Property& property : symbol.parameters) {
                if (text::equalsNoCase(property.name, "DESIGNATOR") && !property.value.empty()) {
                    symbol.designatorPrefix = property.value;
                    break;
                }
            }
            if (symbol.pins.empty() && symbol.parameters.empty() && symbol.shapes.empty()) {
                library.warnings.push_back("Symbol '" + symbol.name +
                                           "' was found but contains no readable records.");
            }
        }
        if (library.symbols.size() == before) {
            LibrarySymbol symbol;
            symbol.name = i < libRefs.size() ? libRefs[i] : storageName;
            library.symbols.push_back(std::move(symbol));
            library.warnings.push_back("Symbol '" + symbol.name + "' contains no records.");
        }
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
    const std::vector<detail::SchRecord> records = detail::readAsciiSchRecords(bytes);
    buildSymbols(records, library);

    for (std::size_t i = 0; i < library.symbols.size(); ++i) {
        LibrarySymbol& symbol = library.symbols[i];
        if (symbol.name.empty()) symbol.name = "Symbol " + std::to_string(i + 1);
        for (const Property& property : symbol.parameters) {
            if (text::equalsNoCase(property.name, "DESIGNATOR") && symbol.designatorPrefix.empty())
                symbol.designatorPrefix = property.value;
            if (text::equalsNoCase(property.name, "DESCRIPTION") && symbol.description.empty())
                symbol.description = property.value;
        }
    }
    if (library.symbols.empty()) {
        library.warnings.push_back("No RECORD=1 (symbol) entries were found in this ASCII library.");
    }
}

// ---------------------------------------------------------------------------
// SchLib symbol graphics
// ---------------------------------------------------------------------------

/// Schematic coordinates are stored in 10 mil units (like SchDoc LOCATION.X).
constexpr double kSchUnit = 10.0;

std::string pinElectricalName(int value) { return text::pinElectricalName(value); }

/// Altium escapes a run of characters with a leading backslash ("S\\P\\I\\2\\_\\N\\S\\S\\"
/// is "SPI2_NSS"), so drop the backslash in front of a printable character.
std::string decodeEscapes(const std::string& value) {
    if (value.find('\\') == std::string::npos) return value;
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size() && value[i + 1] != '\\') {
            out.push_back(value[++i]);
            continue;
        }
        out.push_back(value[i]);
    }
    return out;
}

/// Applies one schematic record to a symbol: pins, texts, parameters and the drawing
/// primitives (line, polyline, rectangle, round rectangle, arc, ellipse, text).
void applySymbolRecord(const std::vector<Property>& props, int type, LibrarySymbol& symbol) {
    auto number = [&props](const char* key) { return text::parseNumber(valueOf(props, key)); };
    auto present = [&props](const char* key) { return text::find(props, key) != nullptr; };
    // Every graphic primitive carries OWNERPARTID; -1 means it is shared by all parts.
    const int ownerPart = text::parseInt(valueOf(props, "OWNERPARTID"), 1);

    switch (type) {
        case 2: {  // pin
            SymbolPin pin;
            pin.designator = decodeEscapes(valueOf(props, "DESIGNATOR"));
            pin.name = decodeEscapes(valueOf(props, "NAME"));
            pin.electrical = pinElectricalName(text::parseInt(valueOf(props, "ELECTRICAL"), 0));
            pin.x = number("LOCATION.X") * kSchUnit;
            pin.y = number("LOCATION.Y") * kSchUnit;
            pin.length = number("PINLENGTH") * kSchUnit;
            const int conglomerate = text::parseInt(valueOf(props, "PINCONGLOMERATE"), 0);
            pin.rotation = (conglomerate & 0x03) * 90;
            pin.hidden = (conglomerate & 0x04) != 0 ||
                         text::equalsNoCase(valueOf(props, "HIDDEN"), "T");
            pin.showName = (conglomerate & 0x08) != 0;
            pin.showDesignator = (conglomerate & 0x10) != 0;
            pin.ownerPart = std::max(1, text::parseInt(valueOf(props, "OWNERPARTID"), 1));

            Pin row;
            row.name = pin.name;
            row.designator = pin.designator;
            row.electrical = pin.electrical;
            row.ownerPart = pin.ownerPart;
            symbol.pins.push_back(row);
            symbol.pinGeometry.push_back(pin);
            break;
        }
        case 34: {  // designator text ("U?")
            const std::string value = valueOf(props, "TEXT");
            if (!value.empty()) symbol.designatorPrefix = value;
            break;
        }
        case 41: {  // parameter
            const std::string name = valueOf(props, "NAME");
            if (name.empty()) break;
            symbol.parameters.push_back(Property{name, valueOf(props, "TEXT")});
            break;
        }
        case 13: {  // line
            SymbolShape shape;
            shape.ownerPart = ownerPart;
            shape.kind = SymbolShape::Kind::Line;
            shape.points = {Point{number("X1") * kSchUnit, number("Y1") * kSchUnit},
                            Point{number("X2") * kSchUnit, number("Y2") * kSchUnit}};
            shape.width = number("WIDTH") * kSchUnit;
            symbol.shapes.push_back(shape);
            break;
        }
        case 6:   // polyline
        case 7: {  // polygon
            SymbolShape shape;
            shape.ownerPart = ownerPart;
            shape.kind = SymbolShape::Kind::Polyline;
            shape.fill = type == 7 && text::equalsNoCase(valueOf(props, "FILLED"), "T") ? 2 : 0;
            const int count = text::parseInt(valueOf(props, "LOCATIONCOUNT"), 0);
            for (int i = 0; i < count; ++i) {
                if (!present(("X" + std::to_string(i + 1)).c_str())) break;
                shape.points.push_back(Point{number(("X" + std::to_string(i + 1)).c_str()) * kSchUnit,
                                             number(("Y" + std::to_string(i + 1)).c_str()) * kSchUnit});
            }
            shape.width = number("WIDTH") * kSchUnit;
            if (shape.points.size() >= 2) symbol.shapes.push_back(shape);
            break;
        }
        case 14:    // rectangle
        case 10: {  // round rectangle
            SymbolShape shape;
            shape.ownerPart = ownerPart;
            shape.kind = type == 10 ? SymbolShape::Kind::RoundRectangle : SymbolShape::Kind::Rectangle;
            shape.points = {Point{number("LOCATION.X") * kSchUnit, number("LOCATION.Y") * kSchUnit},
                            Point{number("CORNER.X") * kSchUnit, number("CORNER.Y") * kSchUnit}};
            shape.fill = text::equalsNoCase(valueOf(props, "FILLED"), "T") ? 2 : 1;
            shape.width = present("LINEWIDTH") ? number("LINEWIDTH") * kSchUnit : 0.0;
            symbol.shapes.push_back(shape);
            break;
        }
        case 12: {  // arc
            SymbolShape shape;
            shape.ownerPart = ownerPart;
            shape.kind = SymbolShape::Kind::Arc;
            shape.points = {Point{number("LOCATION.X") * kSchUnit, number("LOCATION.Y") * kSchUnit}};
            shape.radius = number("RADIUS") * kSchUnit;
            shape.startAngle = number("STARTANGLE");
            shape.endAngle = number("ENDANGLE");
            shape.width = number("WIDTH") * kSchUnit;
            if (shape.radius > 0.0) symbol.shapes.push_back(shape);
            break;
        }
        case 8: {  // ellipse
            SymbolShape shape;
            shape.ownerPart = ownerPart;
            shape.kind = SymbolShape::Kind::Ellipse;
            shape.points = {Point{number("LOCATION.X") * kSchUnit, number("LOCATION.Y") * kSchUnit}};
            shape.radius = number("RADIUS") * kSchUnit;
            shape.secondaryRadius = present("SECONDARYRADIUS") ? number("SECONDARYRADIUS") * kSchUnit
                                                               : shape.radius;
            shape.fill = text::equalsNoCase(valueOf(props, "FILLED"), "T") ? 2 : 1;
            symbol.shapes.push_back(shape);
            break;
        }
        case 28: {  // text
            SymbolShape shape;
            shape.ownerPart = ownerPart;
            shape.kind = SymbolShape::Kind::Text;
            shape.text = valueOf(props, "TEXT");
            shape.points = {Point{number("LOCATION.X") * kSchUnit, number("LOCATION.Y") * kSchUnit}};
            shape.rotation = number("ORIENTATION");
            shape.textHeight = present("HEIGHT") ? number("HEIGHT") * kSchUnit : 60.0;
            if (!shape.text.empty()) symbol.shapes.push_back(shape);
            break;
        }
        default:
            break;
    }
}

/// Walks a record stream (binary SchLib symbol or ASCII library) and fills the symbols.
void buildSymbols(const std::vector<detail::SchRecord>& records, Library& library) {
    std::map<int, std::size_t> byIndex;
    int lastSymbol = -1;
    std::string version;

    for (const detail::SchRecord& record : records) {
        if (record.type < 0) {
            const std::string header = text::valueOf(record.props, "HEADER");
            if (!header.empty()) version = header;
            continue;
        }
        if (record.type == 1) {
            LibrarySymbol symbol;
            symbol.name = valueOf(record.props, "LIBREFERENCE");
            if (symbol.name.empty()) symbol.name = valueOf(record.props, "LIBREF");
            symbol.description = valueOf(record.props, "COMPONENTDESCRIPTION");
            symbol.partCount = std::max(1, text::parseInt(valueOf(record.props, "PARTCOUNT"), 2) - 1);
            for (const Property& property : record.props) symbol.parameters.push_back(property);
            byIndex[record.index] = library.symbols.size();
            library.symbols.push_back(std::move(symbol));
            lastSymbol = record.index;
            continue;
        }

        int owner = text::parseInt(valueOf(record.props, "OWNERINDEX"), -1);
        if (owner < 0) owner = lastSymbol;
        const auto it = byIndex.find(owner);
        if (it == byIndex.end() || it->second >= library.symbols.size()) continue;
        applySymbolRecord(record.props, record.type, library.symbols[it->second]);
    }
    if (library.formatVersion.empty() && !version.empty()) library.formatVersion = version;
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
    // The real 3D model of this footprint travels with the preview document.
    if (!footprint.models.empty() && !footprint.models.front().stepText.empty()) {
        document.models = footprint.models;
        component.modelIndex = 0;
    }
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
        // A footprint can carry the real STEP body; the 3D view draws it instead of a box.
        std::size_t withModel = 0;
        for (const LibraryFootprint& footprint : library.footprints) {
            if (!footprint.models.empty()) ++withModel;
        }
        if (withModel > 0) {
            out << "\n3D models  " << withModel << " of " << library.footprints.size()
                << " footprint(s) carry a STEP body (Models storage)\n";
        }
        std::size_t withDatasheet = 0;
        for (const LibraryFootprint& footprint : library.footprints) {
            if (!partProperty(footprint.parameters, "DATASHEET").empty()) ++withDatasheet;
        }
        if (withDatasheet > 0) {
            out << "\nDatasheets\n";
            for (const LibraryFootprint& footprint : library.footprints) {
                const std::string url = partProperty(footprint.parameters, "DATASHEET");
                if (url.empty()) continue;
                out << "  " << std::left << std::setw(24) << ellipsize(footprint.name, 23)
                    << url << "\n";
            }
        }
    } else {
        out << "\n" << std::left;
        out << std::setw(34) << "Symbol" << std::right << std::setw(6) << "Pins"
            << std::setw(7) << "Parts" << std::setw(8) << "Shapes" << "   Description\n";
        out << std::string(96, '-') << "\n";
        for (const LibrarySymbol& symbol : library.symbols) {
            std::string description = symbol.description;
            if (description.size() > 44) description = description.substr(0, 41) + "...";
            out << std::left << std::setw(34) << ellipsize(symbol.name, 33) << std::right << std::setw(6)
                << symbol.pins.size() << std::setw(7) << symbol.partCount << std::setw(8)
                << symbol.shapes.size() << "   " << description << "\n";
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

Document symbolDocument(const LibrarySymbol& symbol, const std::string& fileName) {
    Document document;
    document.fileName = fileName + " · " + symbol.name;
    document.type = DocType::SchLib;
    document.container = Container::CompoundBinary;
    document.formatVersion = "Symbol preview";

    // Symbols are drawn with the board primitives: outlines/lines/pins on the top overlay
    // (white) and filled bodies as pale-yellow fills, which is what Altium looks like.
    constexpr int kOutlineLayer = 33;  // Top Overlay
    constexpr int kFillLayer = 74;     // Multi-Layer (pale yellow in every renderer)

    auto addLine = [&document](double x1, double y1, double x2, double y2, double width) {
        TrackFeature track;
        track.x1 = x1;
        track.y1 = y1;
        track.x2 = x2;
        track.y2 = y2;
        track.width = width;
        track.layer = 33;
        document.geometry.tracks.push_back(track);
    };
    // Body text sits on the pale-yellow fill, so it is drawn dark like Altium does.
    auto addTextAligned = [&document](double x, double y, const std::string& text, double height,
                                      int anchor, std::uint32_t color) {
        TextFeature label;
        label.x = x;
        label.y = y;
        label.text = text;
        label.height = height;
        label.layer = 33;
        label.color = color;
        label.anchor = anchor;
        document.geometry.texts.push_back(label);
    };
    auto addText = [&document](double x, double y, const std::string& text, double height) {
        TextFeature label;
        label.x = x;
        label.y = y;
        label.text = text;
        label.height = height;
        label.layer = 33;
        label.color = 0x171b21;
        document.geometry.texts.push_back(label);
    };
    auto addPinText = [&document](double x, double y, const std::string& text, double height) {
        TextFeature label;
        label.x = x;
        label.y = y;
        label.text = text;
        label.height = height;
        label.layer = 33;
        label.color = 0xd8dde3;   // outside the body: light on the dark background
        document.geometry.texts.push_back(label);
    };

    // A multi-part symbol keeps every part in the same coordinate space, so drawing the
    // parts as they are stored would stack them on top of each other. Shift each part to
    // its own column instead, which is how a compound symbol reads in Altium's editor.
    std::vector<int> parts;
    for (const SymbolShape& shape : symbol.shapes) {
        if (shape.ownerPart > 0) parts.push_back(shape.ownerPart);
    }
    for (const SymbolPin& pin : symbol.pinGeometry) {
        if (pin.ownerPart > 0) parts.push_back(pin.ownerPart);
    }
    std::sort(parts.begin(), parts.end());
    parts.erase(std::unique(parts.begin(), parts.end()), parts.end());
    if (parts.size() < 2) parts.assign(1, 1);

    auto partOf = [&parts](int owner) {
        return std::find(parts.begin(), parts.end(), owner) != parts.end() ? owner : parts.front();
    };

    // Horizontal extent of every part (body rectangles and pins), used for the columns.
    std::map<int, std::pair<double, double>> extent;
    auto touchX = [&extent](int part, double x) {
        auto it = extent.find(part);
        if (it == extent.end()) {
            extent.emplace(part, std::make_pair(x, x));
        } else {
            it->second.first = std::min(it->second.first, x);
            it->second.second = std::max(it->second.second, x);
        }
    };
    for (const SymbolShape& shape : symbol.shapes) {
        const int part = partOf(shape.ownerPart);
        for (const Point& point : shape.points) touchX(part, point.x);
        if (shape.points.empty()) touchX(part, 0.0);
    }
    for (const SymbolPin& pin : symbol.pinGeometry) {
        if (pin.hidden) continue;
        const int part = partOf(pin.ownerPart);
        touchX(part, pin.x);
        touchX(part, pin.x + (pin.rotation == 180 ? -pin.length : pin.length));
    }

    std::map<int, double> shiftX;
    {
        const double columnGap = 900.0;
        double cursor = 0.0;
        for (const int part : parts) {
            const auto& range = extent[part];
            shiftX[part] = cursor - range.first;
            cursor += (range.second - range.first) + columnGap;
        }
    }

    const std::vector<SymbolShape> shapes = [&] {
        std::vector<SymbolShape> shifted = symbol.shapes;
        for (SymbolShape& shape : shifted) {
            const double offset = shiftX[partOf(shape.ownerPart)];
            for (Point& point : shape.points) point.x += offset;
        }
        return shifted;
    }();
    const std::vector<SymbolPin> pins = [&] {
        std::vector<SymbolPin> shifted = symbol.pinGeometry;
        for (SymbolPin& pin : shifted) pin.x += shiftX[partOf(pin.ownerPart)];
        return shifted;
    }();

    for (const SymbolShape& shape : shapes) {
        const double stroke = shape.width > 0.0 ? shape.width : 8.0;
        switch (shape.kind) {
            case SymbolShape::Kind::Line:
                if (shape.points.size() >= 2)
                    addLine(shape.points[0].x, shape.points[0].y, shape.points[1].x,
                            shape.points[1].y, stroke);
                break;
            case SymbolShape::Kind::Polyline:
            case SymbolShape::Kind::RoundRectangle:
            case SymbolShape::Kind::Rectangle: {
                if (shape.kind != SymbolShape::Kind::Polyline && shape.points.size() >= 2) {
                    const double x1 = std::min(shape.points[0].x, shape.points[1].x);
                    const double x2 = std::max(shape.points[0].x, shape.points[1].x);
                    const double y1 = std::min(shape.points[0].y, shape.points[1].y);
                    const double y2 = std::max(shape.points[0].y, shape.points[1].y);
                    if (shape.fill != 0) {
                        FillFeature body;
                        body.x1 = x1;
                        body.y1 = y1;
                        body.x2 = x2;
                        body.y2 = y2;
                        body.layer = kFillLayer;
                        document.geometry.fills.push_back(body);
                    }
                    addLine(x1, y1, x2, y1, stroke);
                    addLine(x2, y1, x2, y2, stroke);
                    addLine(x2, y2, x1, y2, stroke);
                    addLine(x1, y2, x1, y1, stroke);
                    break;
                }
                for (std::size_t i = 0; i + 1 < shape.points.size(); ++i)
                    addLine(shape.points[i].x, shape.points[i].y, shape.points[i + 1].x,
                            shape.points[i + 1].y, stroke);
                if (shape.fill == 2 && shape.points.size() >= 3) {
                    RegionFeature region;
                    region.outline = shape.points;
                    region.layer = kFillLayer;
                    document.geometry.regions.push_back(region);
                }
                break;
            }
            case SymbolShape::Kind::Arc: {
                if (shape.points.empty() || shape.radius <= 0.0) break;
                ArcFeature arc;
                arc.cx = shape.points[0].x;
                arc.cy = shape.points[0].y;
                arc.radius = shape.radius;
                arc.startAngle = shape.startAngle;
                arc.endAngle = shape.endAngle;
                arc.width = stroke;
                arc.layer = kOutlineLayer;
                document.geometry.arcs.push_back(arc);
                break;
            }
            case SymbolShape::Kind::Ellipse: {
                if (shape.points.empty() || shape.radius <= 0.0) break;
                const int steps = 48;
                RegionFeature ring;
                ring.layer = kFillLayer;
                for (int i = 0; i < steps; ++i) {
                    const double angle = 2.0 * 3.14159265358979323846 * i / steps;
                    Point point;
                    point.x = shape.points[0].x + shape.radius * std::cos(angle);
                    point.y = shape.points[0].y + shape.secondaryRadius * std::sin(angle);
                    ring.outline.push_back(point);
                }
                if (shape.fill == 2) {
                    document.geometry.regions.push_back(ring);
                } else {
                    for (std::size_t i = 0; i < ring.outline.size(); ++i) {
                        const Point& a = ring.outline[i];
                        const Point& b = ring.outline[(i + 1) % ring.outline.size()];
                        addLine(a.x, a.y, b.x, b.y, stroke);
                    }
                }
                break;
            }
            case SymbolShape::Kind::Text:
                if (!shape.points.empty())
                    addText(shape.points[0].x, shape.points[0].y,
                            shape.text, shape.textHeight > 0.0 ? shape.textHeight : 60.0);
                break;
        }
    }

    // Pins: Altium stores the pin's body-side point in LOCATION and draws the pin outwards
    // in the orientation direction, so the connection point is the far end.
    const double pinText = 50.0;

    // The body of a part is the union of its (round) rectangles. Pin names are clipped to
    // it, the way Altium clips them, so a long name can never spill onto the background.
    struct BodyRect {
        double minX = 0.0, maxX = 0.0, minY = 0.0, maxY = 0.0;
        bool valid = false;
    };
    std::map<int, BodyRect> bodies;
    for (const SymbolShape& shape : shapes) {
        if (shape.kind != SymbolShape::Kind::Rectangle &&
            shape.kind != SymbolShape::Kind::RoundRectangle) {
            continue;
        }
        if (shape.points.size() < 2) continue;
        BodyRect& body = bodies[partOf(shape.ownerPart)];
        const double x1 = std::min(shape.points[0].x, shape.points[1].x);
        const double x2 = std::max(shape.points[0].x, shape.points[1].x);
        const double y1 = std::min(shape.points[0].y, shape.points[1].y);
        const double y2 = std::max(shape.points[0].y, shape.points[1].y);
        if (!body.valid) {
            body.minX = x1;
            body.maxX = x2;
            body.minY = y1;
            body.maxY = y2;
            body.valid = true;
        } else {
            body.minX = std::min(body.minX, x1);
            body.maxX = std::max(body.maxX, x2);
            body.minY = std::min(body.minY, y1);
            body.maxY = std::max(body.maxY, y2);
        }
    }
    for (const SymbolPin& pin : pins) {
        if (pin.hidden) continue;
        double dx = 1.0;
        double dy = 0.0;
        if (pin.rotation == 90) { dx = 0.0; dy = 1.0; }
        else if (pin.rotation == 180) { dx = -1.0; dy = 0.0; }
        else if (pin.rotation == 270) { dx = 0.0; dy = -1.0; }
        const double endX = pin.x + dx * pin.length;
        const double endY = pin.y + dy * pin.length;
        addLine(pin.x, pin.y, endX, endY, 8.0);
        // Small ring at the connection point (the end a wire attaches to).
        {
            ArcFeature dot;
            dot.cx = endX;
            dot.cy = endY;
            dot.radius = 10.0;
            dot.width = 8.0;
            dot.layer = kOutlineLayer;
            document.geometry.arcs.push_back(dot);
        }
        if (pin.showDesignator && !pin.designator.empty()) {
            addPinText((pin.x + endX) / 2.0 + (dx == 0.0 ? 14.0 : -8.0),
                       (pin.y + endY) / 2.0 + (dy == 0.0 ? 16.0 : 8.0), pin.designator, pinText);
        }
        if (pin.showName && !pin.name.empty()) {
            // Pin names sit inside the body. They are anchored at the body edge and grow away
            // from it, so a long name can never run into the pins on the other side.
            double nameX = pin.x;
            double nameY = pin.y - pinText * 0.35;
            int anchor = 1;  // left aligned
            if (dx > 0.0) {           // body is to the left of the pin: grow leftwards
                nameX = pin.x - 24.0;
                anchor = 2;
            } else if (dx < 0.0) {    // body is to the right: grow rightwards
                nameX = pin.x + 24.0;
                anchor = 1;
            } else if (dy > 0.0) {
                nameY = pin.y - 30.0 - pinText * 0.5;
                anchor = 0;
            } else {
                nameY = pin.y + 30.0;
                anchor = 0;
            }
            std::string name = pin.name;
            const auto bodyIt = bodies.find(partOf(pin.ownerPart));
            if (bodyIt != bodies.end() && bodyIt->second.valid && (anchor == 1 || anchor == 2)) {
                // Names grow from the body edge towards the middle; each side may use at most
                // half of the body (minus a small gap), so the two columns never overlap.
                // Roughly 0.62 × the text height per character.
                const double bodyCenterX = (bodyIt->second.minX + bodyIt->second.maxX) / 2.0;
                const double usable = anchor == 1 ? (bodyCenterX - 40.0) - nameX
                                                  : nameX - (bodyCenterX + 40.0);
                const std::size_t maxChars =
                    static_cast<std::size_t>(std::max(0.0, usable / (pinText * 0.62)));
                if (name.size() > maxChars && maxChars > 3)
                    name = name.substr(0, maxChars - 3) + "...";
            }
            addTextAligned(nameX, nameY, name, pinText, anchor, 0x171b21);
        }
    }

    // Label each column of a compound symbol, so it is obvious which part is which.
    if (parts.size() > 1) {
        for (const int part : parts) {
            const auto it = bodies.find(part);
            if (it == bodies.end() || !it->second.valid) continue;
            const double captionHeight =
                std::max(80.0, (it->second.maxY - it->second.minY) * 0.06);
            addTextAligned((it->second.minX + it->second.maxX) / 2.0,
                           it->second.minY - captionHeight * 1.8,
                           "Part " + std::to_string(part) + " / " +
                               std::to_string(parts.size()),
                           captionHeight, 0, 0x93a6bc);
        }
    }

    if (document.geometry.empty()) {
        // Nothing to draw: still return an empty document so the caller can show a hint.
        return document;
    }

    for (const TrackFeature& track : document.geometry.tracks) {
        document.bounds.add(track.x1, track.y1);
        document.bounds.add(track.x2, track.y2);
    }
    for (const ArcFeature& arc : document.geometry.arcs) {
        document.bounds.add(arc.cx - arc.radius, arc.cy - arc.radius);
        document.bounds.add(arc.cx + arc.radius, arc.cy + arc.radius);
    }
    for (const FillFeature& fill : document.geometry.fills) {
        document.bounds.add(fill.x1, fill.y1);
        document.bounds.add(fill.x2, fill.y2);
    }
    for (const RegionFeature& region : document.geometry.regions)
        for (const Point& point : region.outline) document.bounds.add(point.x, point.y);
    for (const TextFeature& label : document.geometry.texts)
        document.bounds.add(label.x, label.y);

    document.pcb.tracks = static_cast<int>(document.geometry.tracks.size());
    document.pcb.arcs = static_cast<int>(document.geometry.arcs.size());

    Component component;
    component.name = symbol.name;
    component.footprint = symbol.name;
    component.description = symbol.description;
    document.components.push_back(std::move(component));

    finalizeBoard(document);
    return document;
}

namespace detail {
bool parsePadRecord(const std::vector<std::uint8_t>& data, std::size_t& pos, Pad& pad) {
    return readPadRecord(data, pos, pad);
}
} // namespace detail

namespace {

/// Lower case with everything but letters and digits dropped: "QFN-24_4x4" and
/// "qfn 24 4x4" have to compare equal when a board and a library spell a footprint
/// slightly differently.
std::string fingerprint(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        const auto ch = static_cast<unsigned char>(c);
        if (std::isalnum(ch)) out.push_back(static_cast<char>(std::tolower(ch)));
    }
    return out;
}

} // namespace

namespace {

/// Extent of a pad group in mil (0 x 0 when it has none).
void padExtent(const std::vector<Pad>& pads, double& width, double& height) {
    double minX = 1e18, maxX = -1e18, minY = 1e18, maxY = -1e18;
    for (const Pad& pad : pads) {
        minX = std::min(minX, pad.x - pad.width / 2.0);
        maxX = std::max(maxX, pad.x + pad.width / 2.0);
        minY = std::min(minY, pad.y - pad.height / 2.0);
        maxY = std::max(maxY, pad.y + pad.height / 2.0);
    }
    width = maxX > minX ? maxX - minX : 0.0;
    height = maxY > minY ? maxY - minY : 0.0;
}

/// How many different pad numbers a pad group has ("1","2","3"), not how many rectangles:
/// a library footprint often splits one pad into a copper and a paste object.
std::size_t uniquePadNumbers(const std::vector<Pad>& pads) {
    std::vector<std::string> names;
    for (const Pad& pad : pads) {
        if (pad.designator.empty()) continue;
        if (std::find(names.begin(), names.end(), pad.designator) == names.end()) {
            names.push_back(pad.designator);
        }
    }
    return names.size();
}

/// Extent of the pad *centres*: where the part's pins sit. This is what a land pattern is
/// built from, and unlike the copper outline it does not move when a designer picks bigger
/// or smaller pads for the same footprint.
void padCentreExtent(const std::vector<Pad>& pads, double& width, double& height) {
    double minX = 1e18, maxX = -1e18, minY = 1e18, maxY = -1e18;
    for (const Pad& pad : pads) {
        minX = std::min(minX, pad.x);
        maxX = std::max(maxX, pad.x);
        minY = std::min(minY, pad.y);
        maxY = std::max(maxY, pad.y);
    }
    width = maxX > minX ? maxX - minX : 0.0;
    height = maxY > minY ? maxY - minY : 0.0;
}

/// Number of pads that actually have a hole (a through-hole part never matches an SMD one).
std::size_t drilledPadCount(const std::vector<Pad>& pads) {
    std::size_t drilled = 0;
    for (const Pad& pad : pads) {
        if (pad.holeSize > 0.0) ++drilled;
    }
    return drilled;
}

/// Largest hole of a pad group: a resistor's 0.8 mm lead and a diode's 1.1 mm lead are a
/// good way to tell two otherwise similar axial footprints apart.
double largestHole(const std::vector<Pad>& pads) {
    double hole = 0.0;
    for (const Pad& pad : pads) hole = std::max(hole, pad.holeSize);
    return hole;
}

/// Last resort when the names do not match at all: the footprint whose pads sit in the same
/// places as the pads of the placed part. Pad centres must agree within 15 %, the drilled /
/// SMD nature must be identical, and the copper outline and the hole size must be close;
/// the copper outline is allowed to differ by more than the centres, because two libraries
/// often use different pad sizes for the same footprint. The best candidate must also be
/// clearly better than every other one, so a model is never attached by guesswork.
const LibraryFootprint* matchByGeometry(const Component& component, const Library& library) {
    if (component.pads.empty()) return nullptr;
    const std::size_t wantedPads = uniquePadNumbers(component.pads);
    if (wantedPads == 0) return nullptr;
    const std::size_t wantedDrilled = drilledPadCount(component.pads);

    double width = 0.0, height = 0.0;
    padCentreExtent(component.pads, width, height);
    // A single pad has no spread at all; such a footprint cannot be told apart from another.
    if (wantedPads > 1 && width <= 0.0 && height <= 0.0) return nullptr;
    double copperWidth = 0.0, copperHeight = 0.0;
    padExtent(component.pads, copperWidth, copperHeight);
    const double hole = largestHole(component.pads);

    auto relative = [](double a, double b) {
        const double scale = std::max(a, b);
        return scale > 0.0 ? std::fabs(a - b) / scale : 0.0;
    };

    const LibraryFootprint* best = nullptr;
    double bestScore = 1e18;
    double secondScore = 1e18;
    for (const LibraryFootprint& footprint : library.footprints) {
        if (footprint.models.empty()) continue;
        if (uniquePadNumbers(footprint.geometry.pads) != wantedPads) continue;
        if (drilledPadCount(footprint.geometry.pads) != wantedDrilled) continue;

        double fpWidth = 0.0, fpHeight = 0.0;
        padCentreExtent(footprint.geometry.pads, fpWidth, fpHeight);
        // Compare only the axes that actually spread; one straight row of pads has height 0.
        auto near = [](double a, double b, double tolerance) {
            if (a <= 0.0 && b <= 0.0) return true;
            if (a <= 0.0 || b <= 0.0) return false;   // one has a spread, the other does not
            return std::fabs(a - b) / std::max(a, b) <= tolerance;
        };
        if (!near(fpWidth, width, 0.15) || !near(fpHeight, height, 0.15)) continue;

        double fpCopperWidth = 0.0, fpCopperHeight = 0.0;
        padExtent(footprint.geometry.pads, fpCopperWidth, fpCopperHeight);
        if (!near(fpCopperWidth, copperWidth, 0.40) || !near(fpCopperHeight, copperHeight, 0.40))
            continue;

        const double holeError = relative(largestHole(footprint.geometry.pads), hole);
        if (!near(largestHole(footprint.geometry.pads), hole, 0.25)) continue;
        const double score = std::max({relative(fpWidth, width), relative(fpHeight, height),
                                       0.6 * relative(fpCopperWidth, copperWidth),
                                       0.6 * relative(fpCopperHeight, copperHeight),
                                       0.6 * holeError});
        if (score < bestScore) {
            secondScore = bestScore;
            bestScore = score;
            best = &footprint;
        } else if (score < secondScore) {
            secondScore = score;
        }
    }
    // Two footprints that fit about equally well mean the answer is not clear: attach
    // nothing. The winner also has to be a genuinely good fit on its own.
    if (best == nullptr || bestScore > 0.15 || secondScore - bestScore < 0.02) return nullptr;
    return best;
}

} // namespace

namespace {

/// Reads a text file whole; empty on failure.
std::string readTextFile(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) return {};
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

/// Fills `model.stepText` from disk for a model that only references a file (KiCad style).
/// The search is deterministic and local: the path as given (after expanding `${KICAD…}`),
/// then the models3d folder of the project layout next to the library and the board.
bool loadExternalModel(EmbeddedModel& model, const std::string& libraryPath) {
    if (!model.stepText.empty()) return true;
    const std::string& raw = model.sourcePath.empty() ? model.name : model.sourcePath;
    if (raw.empty()) return false;

    std::vector<std::filesystem::path> candidates;
    std::string expanded = raw;
    if (expanded.rfind("${", 0) == 0) {
        const std::size_t close = expanded.find('}');
        if (close != std::string::npos) {
            const std::string variable = expanded.substr(2, close - 2);
            const std::string rest = expanded.substr(close + 1);
            for (const char* prefix : {"", "KICAD_", "KICAD6_", "KICAD7_", "KICAD8_", "KICAD9_"}) {
                const std::string name = std::string(prefix) + "3DMODEL_DIR";
                if (variable != name && variable != "KISYS3DMOD") continue;
                if (const char* value = std::getenv(name.c_str())) {
                    if (*value != '\0') {
                        candidates.emplace_back(std::filesystem::path(value) / rest.substr(1));
                    }
                }
            }
        }
    } else {
        candidates.emplace_back(expanded);
    }

    const std::filesystem::path libraryFolder =
        std::filesystem::path(libraryPath).has_parent_path()
            ? std::filesystem::path(libraryPath).parent_path()
            : std::filesystem::current_path();
    for (const std::filesystem::path& base :
         {libraryFolder, libraryFolder / "..", libraryFolder / ".." / "..",
          std::filesystem::current_path()}) {
        candidates.push_back(base / "models3d" / model.name);
        candidates.push_back(base / "libraries" / "models3d" / model.name);
        candidates.push_back(base / model.name);
    }
    if (const char* home = std::getenv("HOME")) {
        candidates.emplace_back(std::filesystem::path(home) / ".local/share/pcbv/libraries" /
                                "models3d" / model.name);
    }
    for (const std::filesystem::path& candidate : candidates) {
        std::string text = readTextFile(candidate);
        if (text.empty()) continue;
        if (text.rfind("ISO-10303-21", 0) == std::string::npos &&
            text.find("ISO-10303-21;") == std::string::npos) {
            continue;   // not a STEP file
        }
        model.stepText = std::move(text);
        return true;
    }
    return false;
}

} // namespace

std::size_t attachModels(Document& document, const Library& library,
                         const std::string& libraryPath) {
    if (library.footprints.empty()) return 0;
    std::size_t attached = 0;
    for (Component& component : document.components) {
        if (component.modelIndex >= 0) continue;   // already has one
        const std::string wanted = !component.footprint.empty() ? component.footprint : component.name;
        std::string needle = fingerprint(wanted);
        if (needle.empty()) continue;

        const LibraryFootprint* match = nullptr;
        auto matchByName = [&library](const std::string& key) -> const LibraryFootprint* {
            if (key.empty()) return nullptr;
            for (const LibraryFootprint& footprint : library.footprints) {
                if (fingerprint(footprint.name) == key) return &footprint;
            }
            return nullptr;
        };
        match = matchByName(needle);
        // Some boards put the library item name in the value field (or in the symbolic name)
        // instead of the footprint field; those are still an exact name, not a guess.
        if (match == nullptr) {
            for (const std::string& other : {component.comment, component.name}) {
                const std::string key = fingerprint(other);
                if (key.empty() || key == needle) continue;
                if ((match = matchByName(key)) != nullptr) {
                    needle = key;
                    break;
                }
            }
        }
        if (match == nullptr) {
            // Fall back to a footprint whose name contains the part's, once the overlap is
            // long enough to mean something ("QFN-24" inside "QFN-24 4x4") — but only when
            // exactly one candidate exists, so a model is never attached by guesswork.
            const LibraryFootprint* candidateMatch = nullptr;
            int candidates = 0;
            for (const LibraryFootprint& footprint : library.footprints) {
                const std::string candidate = fingerprint(footprint.name);
                const std::string& longer = candidate.size() > needle.size() ? candidate : needle;
                const std::string& shorter = candidate.size() > needle.size() ? needle : candidate;
                if (shorter.size() >= 6 && longer.find(shorter) != std::string::npos) {
                    candidateMatch = &footprint;
                    ++candidates;
                }
            }
            if (candidates == 1) match = candidateMatch;
        }
        if (match == nullptr) match = matchByGeometry(component, library);
        if (match == nullptr) continue;
        component.libraryPattern = match->name;
        // The library knows what the part is (datasheet, maker, order number) even when the
        // board file does not say; fill only what the board left empty.
        if (component.datasheet.empty())
            component.datasheet = partProperty(match->parameters, "DATASHEET");
        if (component.manufacturer.empty())
            component.manufacturer = partProperty(match->parameters, "MANUFACTURER");
        if (component.partNumber.empty())
            component.partNumber = partProperty(match->parameters, "MPN");
        if (match->models.empty()) continue;

        // Copy the models once and point the component at them. A model that only names a
        // file (KiCad) is read from the disk first.
        for (const EmbeddedModel& stored : match->models) {
            EmbeddedModel model = stored;
            if (!loadExternalModel(model, libraryPath)) continue;
            if (component.modelIndex < 0) {
                component.modelIndex = static_cast<int>(document.models.size());
            }
            document.models.push_back(std::move(model));
        }
        if (component.modelIndex >= 0) ++attached;
    }
    return attached;
}

std::size_t attachModelsFromFile(Document& document, const std::string& libraryPath,
                                 std::string* error) {
    const ParseOutcome outcome = parseFile(libraryPath);
    if (!outcome.ok) {
        if (error != nullptr) *error = outcome.error;
        return 0;
    }
    return attachModels(document, outcome.library);
}

std::string partProperty(const std::vector<Property>& parameters, const std::string& name) {
    for (const Property& property : parameters) {
        if (property.name == name) return property.value;
    }
    return {};
}

namespace {

bool endsWithNoCase(const std::string& text, const std::string& tail) {
    if (text.size() < tail.size()) return false;
    for (std::size_t i = 0; i < tail.size(); ++i) {
        const char a = text[text.size() - tail.size() + i];
        const char b = tail[i];
        if (std::tolower(static_cast<unsigned char>(a)) !=
            std::tolower(static_cast<unsigned char>(b))) {
            return false;
        }
    }
    return true;
}

/// A file the viewer can take footprints (and therefore 3D models) from: its own bundle
/// format, or the open KiCad format.
bool isBundlePath(const std::string& path) {
    if (path.size() > 8 && fingerprint(path.substr(path.size() - 8)) == "pcbvlib") return true;
    return endsWithNoCase(path, ".kicad_mod");
}

void addBundlesIn(const std::filesystem::path& folder, std::vector<std::string>& out) {
    std::error_code error;
    if (!std::filesystem::is_directory(folder, error)) return;
    for (const auto& entry : std::filesystem::directory_iterator(
             folder, std::filesystem::directory_options::skip_permission_denied, error)) {
        if (error) break;
        if (!entry.is_regular_file(error)) continue;
        const std::string path = entry.path().string();
        if (!isBundlePath(path)) continue;
        if (std::find(out.begin(), out.end(), path) == out.end()) out.push_back(path);
    }
}

} // namespace

namespace {

/// Folder the running executable lives in, so the library that ships with the viewer is
/// found no matter where the user started it from.
std::filesystem::path executableFolder() {
#if defined(__linux__)
    std::error_code error;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error) return self.parent_path();
#endif
    return {};
}

} // namespace

std::vector<std::string> findLibraryBundles(const std::string& boardPath) {
    std::vector<std::string> files;
    const std::filesystem::path board(boardPath);
    const std::filesystem::path folder = board.has_parent_path() ? board.parent_path()
                                                                 : std::filesystem::current_path();
    addBundlesIn(folder, files);
    addBundlesIn(folder / "library", files);
    // The standard project layout: libraries/footprints holds one KiCad footprint per file.
    for (const std::filesystem::path& base : {folder, std::filesystem::path(".")}) {
        addBundlesIn(base / "libraries" / "footprints", files);
    }

    if (const char* configured = std::getenv("PCBV_LIBRARY")) {
        if (*configured != '\0') addBundlesIn(configured, files);
    }
    std::error_code error;
    addBundlesIn(std::filesystem::current_path(error) / "library", files);
    // build/pcbv-gui -> ../library, i.e. the library that is shipped in the repository.
    const std::filesystem::path exeFolder = executableFolder();
    if (!exeFolder.empty()) {
        addBundlesIn(exeFolder / "library", files);
        addBundlesIn(exeFolder / ".." / "library", files);
        addBundlesIn(exeFolder / ".." / "libraries" / "footprints", files);
    }
    if (const char* home = std::getenv("HOME")) {
        addBundlesIn(std::filesystem::path(home) / ".local/share/pcbv/library", files);
    }
    return files;
}

LibraryAttachment attachLibraryFolder(Document& document, const std::string& boardPath) {
    LibraryAttachment result;
    for (const std::string& path : findLibraryBundles(boardPath)) {
        const ParseOutcome outcome = parseFile(path);
        if (!outcome.ok) {
            document.warnings.push_back("Library '" + path + "' could not be read: " +
                                        outcome.error);
            continue;
        }
        if (outcome.library.footprints.empty()) continue;   // a symbol library: nothing 3D
        const std::size_t matched = attachModels(document, outcome.library, path);
        if (matched > 0) {
            ++result.libraries;
            result.parts += matched;
            result.files.push_back(path);
        }
    }
    return result;
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
