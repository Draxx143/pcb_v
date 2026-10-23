#include "AltiumParser.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

#include "AltiumInternal.h"
#include "BoardIndex.h"
#include "AltiumText.h"
#include "CompoundFile.h"

namespace pcbv {
namespace {

using text::parseInt;
using text::parseMil;
using text::upper;
using text::valueOf;

// ---------------------------------------------------------------------------
// Byte helpers for the binary format
// ---------------------------------------------------------------------------

constexpr std::uint32_t kBlockSizeMask = 0x00FFFFFFu;

std::uint16_t rdU16(const std::vector<std::uint8_t>& b, std::size_t off) {
    if (off + 2 > b.size()) return 0;
    return static_cast<std::uint16_t>(b[off] | (b[off + 1] << 8));
}
std::uint32_t rdU32(const std::vector<std::uint8_t>& b, std::size_t off) {
    if (off + 4 > b.size()) return 0;
    return static_cast<std::uint32_t>(b[off]) | (static_cast<std::uint32_t>(b[off + 1]) << 8) |
           (static_cast<std::uint32_t>(b[off + 2]) << 16) | (static_cast<std::uint32_t>(b[off + 3]) << 24);
}
std::int32_t rdI32(const std::vector<std::uint8_t>& b, std::size_t off) {
    return static_cast<std::int32_t>(rdU32(b, off));
}
std::uint8_t rdB(const std::vector<std::uint8_t>& b, std::size_t off) {
    return off < b.size() ? b[off] : 0;
}
double rdF64(const std::vector<std::uint8_t>& b, std::size_t off) {
    if (off + 8 > b.size()) return 0.0;
    std::uint64_t bits = 0;
    for (int i = 7; i >= 0; --i) bits = (bits << 8) | b[off + static_cast<std::size_t>(i)];
    double result = 0.0;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

/// Iterates over [uint32 size][payload] blocks inside a binary stream.
class BlockStream {
public:
    explicit BlockStream(const std::vector<std::uint8_t>& data) : data_(data) {}

    bool atEnd() const { return pos_ + 4 > data_.size(); }

    bool next(std::vector<std::uint8_t>& payload) {
        if (pos_ + 4 > data_.size()) return false;
        const std::uint32_t header = rdU32(data_, pos_);
        const std::uint32_t size = header & kBlockSizeMask;
        pos_ += 4;
        if (size == 0) return false;
        if (pos_ + size > data_.size()) return false;
        payload.assign(data_.begin() + static_cast<std::ptrdiff_t>(pos_),
                       data_.begin() + static_cast<std::ptrdiff_t>(pos_ + size));
        pos_ += size;
        return true;
    }

    std::vector<std::uint8_t> nextRaw() {
        std::vector<std::uint8_t> payload;
        if (!next(payload)) return {};
        return payload;
    }

    std::size_t position() const { return pos_; }
    void setPosition(std::size_t position) { pos_ = position; }
    const std::vector<std::uint8_t>& data() const { return data_; }

private:
    const std::vector<std::uint8_t>& data_;
    std::size_t pos_ = 0;
};

/// Reads a "string block": a size-prefixed block holding a 1-byte length + text.
///
/// Altium writes the length prefix equal to the string length but the block size equal
/// to that same length (not length + 1), so the last character sits one byte past the
/// block. We allow that one-byte overrun, as Altium's own readers do.
std::string readStringBlock(BlockStream& bs) {
    const std::vector<std::uint8_t>& data = bs.data();
    const std::size_t start = bs.position();
    if (start + 4 > data.size()) {
        bs.setPosition(data.size());
        return {};
    }
    const std::uint32_t size = rdU32(data, start) & kBlockSizeMask;
    const std::size_t pos = start + 4;
    if (size == 0 || pos >= data.size()) {
        bs.setPosition(std::min<std::size_t>(pos + size, data.size()));
        return {};
    }
    const std::size_t declared = data[pos];
    const std::size_t available = std::min<std::size_t>(declared, data.size() - (pos + 1));
    const std::string result = text::fromCp1252(data.data() + pos + 1, available);
    bs.setPosition(std::min<std::size_t>(pos + size, data.size()));
    return result;
}

/// Counts records that consist of one object-id byte + one size-prefixed block each.
int countSingleBlockRecords(const std::vector<std::uint8_t>& data) {
    std::size_t pos = 0;
    int count = 0;
    while (pos < data.size()) {
        ++pos; // object id byte
        if (pos + 4 > data.size()) return -1;
        const std::uint32_t size = rdU32(data, pos) & kBlockSizeMask;
        pos += 4;
        if (size == 0 || pos + size > data.size()) return -1;
        pos += size;
        ++count;
    }
    return pos == data.size() ? count : -1;
}

// ---------------------------------------------------------------------------
// PcbDoc (binary / OLE2)
// ---------------------------------------------------------------------------

/// Reads one Pads6 record starting at *pos (which points at the object id byte).
bool readPadRecord(const std::vector<std::uint8_t>& data, std::size_t& pos, Pad& pad,
                   int& componentIndex, int& netIndex) {
    if (pos >= data.size()) return false;
    ++pos; // object id (always 2 for pads)

    for (int i = 0; i < 3; ++i) {
        if (pos + 4 > data.size()) return false;
        const std::uint32_t size = rdU32(data, pos) & kBlockSizeMask;
        pos += 4;
        if (size == 0) continue;
        if (pos + size > data.size()) return false;
        if (i == 0 && size >= 1) {
            const std::size_t length = std::min<std::size_t>(data[pos], size - 1);
            pad.designator = text::fromCp1252(data.data() + pos + 1, length);
        }
        pos += size;
    }

    if (pos + 4 > data.size()) return false;
    const std::uint32_t skipSize = rdU32(data, pos) & kBlockSizeMask;
    pos += 4 + skipSize;

    if (pos + 4 > data.size()) return false;
    const std::uint32_t mainSize = rdU32(data, pos) & kBlockSizeMask;
    pos += 4;
    if (mainSize < 61 || pos + mainSize > data.size()) return false;
    const std::size_t main = pos;

    pad.layer = rdB(data, main + 0);
    netIndex = rdU16(data, main + 3);
    componentIndex = rdU16(data, main + 7);
    if (componentIndex == 0xFFFF) componentIndex = -1;
    if (netIndex == 0xFFFF) netIndex = -1;

    pad.x = static_cast<double>(rdI32(data, main + 13)) / 10000.0; // 1/10000 mil units
    pad.y = static_cast<double>(rdI32(data, main + 17)) / 10000.0;
    pad.width = static_cast<double>(rdI32(data, main + 21)) / 10000.0;
    pad.height = static_cast<double>(rdI32(data, main + 25)) / 10000.0;
    pad.holeSize = static_cast<double>(rdI32(data, main + 45)) / 10000.0;
    pad.rotation = rdF64(data, main + 52);
    pad.plated = rdB(data, main + 60) != 0;
    pad.layerName = text::pcbLayerName(pad.layer);
    pos = main + mainSize;

    if (pos + 4 <= data.size()) {
        const std::uint32_t sr6 = rdU32(data, pos) & kBlockSizeMask;
        if (pos + 4 + sr6 <= data.size()) pos += 4 + sr6;
    }
    return true;
}

void parseBinaryPcb(CompoundFile& cf, Document& doc) {
    doc.container = Container::CompoundBinary;

    // --- version stamps ---
    const std::vector<std::uint8_t> fh6 = cf.stream("FileHeaderSix");
    if (!fh6.empty()) {
        BlockStream bs(fh6);
        std::string version = readStringBlock(bs);
        while (!version.empty() && (version.back() == '\n' || version.back() == '\r')) version.pop_back();
        doc.formatVersion = version;
    }
    if (doc.formatVersion.empty()) {
        doc.formatVersion = "Altium PCB binary (OLE2 compound file)";
    }

    // --- board parameters ---
    const std::vector<std::uint8_t> boardData = cf.stream("Board6/Data");
    if (!boardData.empty()) {
        BlockStream bs(boardData);
        std::vector<std::uint8_t> payload;
        if (bs.next(payload)) {
            const std::vector<Property> props = text::parseParameters(payload.data(), payload.size());
            doc.boardVersion = valueOf(props, "VERSION");
            const std::string date = valueOf(props, "DATE");
            const std::string time = valueOf(props, "TIME");
            if (!date.empty() || !time.empty()) doc.lastSaved = text::trim(date + " " + time);
        }
    }

    // --- nets ---
    std::map<int, std::string> netNames;
    const std::vector<std::uint8_t> netsData = cf.stream("Nets6/Data");
    if (!netsData.empty()) {
        BlockStream bs(netsData);
        std::vector<std::uint8_t> payload;
        int index = 0;
        while (bs.next(payload)) {
            const std::vector<Property> props = text::parseParameters(payload.data(), payload.size());
            Net net;
            net.index = index;
            net.name = valueOf(props, "NAME");
            if (net.name.empty()) net.name = valueOf(props, "NETNAME");
            netNames[index] = net.name;
            doc.nets.push_back(net);
            ++index;
        }
        doc.pcb.nets = static_cast<int>(doc.nets.size());
    }

    // --- components ---
    const std::vector<std::uint8_t> compData = cf.stream("Components6/Data");
    if (!compData.empty()) {
        BlockStream bs(compData);
        std::vector<std::uint8_t> payload;
        int index = 0;
        while (bs.next(payload)) {
            const std::vector<Property> props = text::parseParameters(payload.data(), payload.size());
            if (props.empty()) {
                ++index;
                continue;
            }
            Component c;
            c.componentIndex = index;

            std::string designator = valueOf(props, "SOURCEDESIGNATOR");
            if (designator.empty()) designator = valueOf(props, "DESIGNATOR");
            c.designator = designator;
            c.name = valueOf(props, "SOURCELIBREFERENCE");
            c.footprint = valueOf(props, "PATTERN");
            c.comment = valueOf(props, "COMMENT");
            if (c.comment.empty()) c.comment = valueOf(props, "SOURCELIBREFERENCE");
            c.description = valueOf(props, "DESCRIPTION");
            if (c.description.empty()) c.description = valueOf(props, "FOOTPRINTDESCRIPTION");
            c.layer = valueOf(props, "LAYER");
            c.sourceLibrary = valueOf(props, "SOURCECOMPONENTLIBRARY");
            c.sourceFootprintLibrary = valueOf(props, "SOURCEFOOTPRINTLIBRARY");
            c.uniqueId = valueOf(props, "UNIQUEID");
            c.manufacturer = valueOf(props, "MANUFACTURER");
            c.partNumber = valueOf(props, "SOURCECOMPDESIGNITEMID");
            if (c.partNumber.empty()) c.partNumber = valueOf(props, "PARTNUMBER");
            c.supplier = valueOf(props, "SUPPLIER");
            c.supplierPartNumber = valueOf(props, "SUPPLIERPARTNUMBER");
            c.datasheet = valueOf(props, "DATASHEET");
            c.x = parseMil(valueOf(props, "X"));
            c.y = parseMil(valueOf(props, "Y"));
            c.rotation = text::parseNumber(valueOf(props, "ROTATION"));
            c.height = parseMil(valueOf(props, "HEIGHT"));
            for (const auto& p : props) c.properties.push_back(p);

            doc.components.push_back(std::move(c));
            ++index;
        }
    }

    // --- pads ---
    const std::vector<std::uint8_t> padData = cf.stream("Pads6/Data");
    if (!padData.empty()) {
        std::size_t pos = 0;
        int parsed = 0;
        while (pos < padData.size()) {
            Pad pad;
            int componentIndex = -1;
            int netIndex = -1;
            const std::size_t before = pos;
            if (!readPadRecord(padData, pos, pad, componentIndex, netIndex)) break;
            pad.componentIndex = componentIndex;
            pad.netIndex = netIndex;
            const auto it = netNames.find(netIndex);
            if (netIndex >= 0 && it != netNames.end()) pad.net = it->second;
            doc.bounds.add(pad.x, pad.y);
            doc.geometry.pads.push_back(pad);
            if (componentIndex >= 0 && componentIndex < static_cast<int>(doc.components.size())) {
                Component& c = doc.components[static_cast<std::size_t>(componentIndex)];
                c.pads.push_back(pad);
                ++c.padCount;
                if (pad.holeSize > 0.0) ++c.throughHolePads;
            }
            ++parsed;
            if (pos == before) break;
        }
        doc.pcb.pads = parsed;
        if (parsed == 0) doc.warnings.push_back("Pads6 stream could not be decoded.");
    }

    // --- tracks / vias ---
    const std::vector<std::uint8_t> trackData = cf.stream("Tracks6/Data");
    if (!trackData.empty()) {
        std::size_t pos = 0;
        int count = 0;
        while (pos < trackData.size()) {
            const std::size_t before = pos;
            ++pos; // object id
            if (pos + 4 > trackData.size()) break;
            const std::uint32_t size = rdU32(trackData, pos) & kBlockSizeMask;
            pos += 4;
            if (size == 0 || pos + size > trackData.size()) break;
            TrackFeature track;
            track.layer = rdB(trackData, pos);
            track.x1 = static_cast<double>(rdI32(trackData, pos + 13)) / 10000.0;
            track.y1 = static_cast<double>(rdI32(trackData, pos + 17)) / 10000.0;
            track.x2 = static_cast<double>(rdI32(trackData, pos + 21)) / 10000.0;
            track.y2 = static_cast<double>(rdI32(trackData, pos + 25)) / 10000.0;
            track.width = static_cast<double>(rdI32(trackData, pos + 29)) / 10000.0;
            const auto trackNet = netNames.find(rdU16(trackData, pos + 3));
            if (trackNet != netNames.end()) track.net = trackNet->second;
            doc.bounds.add(track.x1, track.y1);
            doc.bounds.add(track.x2, track.y2);
            doc.geometry.tracks.push_back(track);
            pos += size;
            ++count;
            if (pos == before) break;
        }
        doc.pcb.tracks = count;
    }

    const std::vector<std::uint8_t> viaData = cf.stream("Vias6/Data");
    if (!viaData.empty()) {
        std::size_t pos = 0;
        int count = 0;
        while (pos < viaData.size()) {
            const std::size_t before = pos;
            ++pos;
            if (pos + 4 > viaData.size()) break;
            const std::uint32_t size = rdU32(viaData, pos) & kBlockSizeMask;
            pos += 4;
            if (size == 0 || pos + size > viaData.size()) break;
            ViaFeature via;
            via.x = static_cast<double>(rdI32(viaData, pos + 13)) / 10000.0;
            via.y = static_cast<double>(rdI32(viaData, pos + 17)) / 10000.0;
            via.diameter = static_cast<double>(rdI32(viaData, pos + 21)) / 10000.0;
            via.hole = static_cast<double>(rdI32(viaData, pos + 25)) / 10000.0;
            const auto viaNet = netNames.find(rdU16(viaData, pos + 3));
            if (viaNet != netNames.end()) via.net = viaNet->second;
            doc.bounds.add(via.x, via.y);
            doc.geometry.vias.push_back(via);
            pos += size;
            ++count;
            if (pos == before) break;
        }
        doc.pcb.vias = count;
    }

    // --- arcs ---
    const std::vector<std::uint8_t> arcData = cf.stream("Arcs6/Data");
    if (!arcData.empty()) {
        std::size_t pos = 0;
        int count = 0;
        while (pos < arcData.size()) {
            const std::size_t before = pos;
            ++pos; // object id (1 = arc)
            if (pos + 4 > arcData.size()) break;
            const std::uint32_t size = rdU32(arcData, pos) & kBlockSizeMask;
            pos += 4;
            if (size < 56 || pos + size > arcData.size()) break;
            ArcFeature arc;
            arc.layer = rdB(arcData, pos);
            arc.cx = static_cast<double>(rdI32(arcData, pos + 13)) / 10000.0;
            arc.cy = static_cast<double>(rdI32(arcData, pos + 17)) / 10000.0;
            arc.radius = static_cast<double>(rdI32(arcData, pos + 21)) / 10000.0;
            arc.startAngle = rdF64(arcData, pos + 25);
            arc.endAngle = rdF64(arcData, pos + 33);
            arc.width = static_cast<double>(rdI32(arcData, pos + 41)) / 10000.0;
            doc.bounds.add(arc.cx - arc.radius, arc.cy - arc.radius);
            doc.bounds.add(arc.cx + arc.radius, arc.cy + arc.radius);
            doc.geometry.arcs.push_back(arc);
            pos = before + 5 + size;
            ++count;
            if (pos == before) break;
        }
        doc.pcb.arcs = count;
    }

    // --- remaining primitive counters ---
    struct Counter {
        const char* storage;
        int PcbCounts::*field;
    };
    static const Counter counters[] = {
        {"Regions6/Data", &PcbCounts::regions},
        {"BoardRegions/Data", &PcbCounts::boardRegions},
        {"ComponentBodies6/Data", &PcbCounts::componentBodies},
    };
    for (const auto& counter : counters) {
        const std::vector<std::uint8_t> data = cf.stream(counter.storage);
        if (data.empty()) continue;
        const int count = countSingleBlockRecords(data);
        if (count >= 0) doc.pcb.*(counter.field) = count;
    }

    // Nets, layer usage and stackup are derived once, from the geometry above.
    finalizeBoard(doc);
}

// ---------------------------------------------------------------------------
// SchDoc (binary / OLE2)
// ---------------------------------------------------------------------------

std::vector<detail::SchRecord> readBinarySchRecords(const std::vector<std::uint8_t>& header) {
    std::vector<detail::SchRecord> records;
    std::size_t pos = 0;
    int objectIndex = 0;
    while (pos + 4 <= header.size()) {
        const std::uint32_t sizeHeader = rdU32(header, pos);
        const std::uint32_t size = sizeHeader & kBlockSizeMask;
        const std::uint8_t flags = static_cast<std::uint8_t>((sizeHeader >> 24) & 0xFF);
        pos += 4;
        if (size == 0 || pos + size > header.size()) break;

        detail::SchRecord record;
        record.binary = (flags == 0x01);
        if (record.binary) {
            record.type = (size >= 4) ? rdI32(header, pos) : -1;
        } else {
            record.props = text::parseParameters(header.data() + pos, size);
            text::applyUnicodeOverrides(record.props);
            record.type = parseInt(valueOf(record.props, "RECORD"), -1);
        }
        pos += size;

        const bool isFileHeader = records.empty() && record.type < 0;
        record.index = isFileHeader ? -1 : objectIndex++;
        records.push_back(std::move(record));
    }
    return records;
}

// ---------------------------------------------------------------------------
// File sniffing
// ---------------------------------------------------------------------------

bool startsWith(const std::vector<std::uint8_t>& bytes, const char* signature, std::size_t length) {
    return bytes.size() >= length &&
           std::memcmp(bytes.data(), signature, length) == 0;
}

std::string printableSignature(const std::vector<std::uint8_t>& bytes, std::size_t limit = 24) {
    std::string out;
    for (std::size_t i = 0; i < bytes.size() && i < limit; ++i) {
        const unsigned char c = bytes[i];
        if (c >= 0x20 && c < 0x7F) {
            out += static_cast<char>(c);
        } else {
            static const char* hex = "0123456789ABCDEF";
            out += "\\x";
            out += hex[(c >> 4) & 0xF];
            out += hex[c & 0xF];
        }
    }
    return out;
}

bool looksLikeAsciiAltium(const std::vector<std::uint8_t>& head) {
    // Find the first non-whitespace/BOM byte and look for record markers nearby.
    std::size_t start = 0;
    while (start < head.size() && (head[start] == '\r' || head[start] == '\n' || head[start] == ' ' ||
                                   head[start] == '\t' || head[start] == 0xEF || head[start] == 0xBB ||
                                   head[start] == 0xBF))
        ++start;
    if (start >= head.size()) return false;
    if (head[start] != '|') return false;
    const std::string text(reinterpret_cast<const char*>(head.data() + start),
                           head.size() - start);
    return text.find("RECORD=") != std::string::npos || text.find("HEADER=") != std::string::npos;
}

bool asciiLooksLikePcb(const std::vector<std::uint8_t>& bytes) {
    const std::string text(reinterpret_cast<const char*>(bytes.data()),
                           std::min<std::size_t>(bytes.size(), 64 * 1024));
    if (text.find("RECORD=Board") != std::string::npos) return true;
    if (text.find("Protel_Advanced_PCB") != std::string::npos) return true;
    if (text.find("Schematic Capture Ascii") != std::string::npos) return false;
    if (text.find("RECORD=1|") != std::string::npos) return false;
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Schematic document assembly (shared binary + ASCII)
// ---------------------------------------------------------------------------

namespace detail {
namespace {

/// Altium indirection: a Comment of "=Part Number" means "use the Part Number parameter".
void resolveReference(std::string& value, const std::vector<Property>& properties) {
    if (value.size() < 2 || value[0] != '=') return;
    const std::string key = upper(text::trim(value.substr(1)));
    for (const auto& property : properties) {
        if (upper(property.name) == key && !property.value.empty()) {
            value = property.value;
            return;
        }
    }
}

} // namespace

void buildSchDocument(const std::vector<SchRecord>& records, Document& document) {
    // --- version string ---
    for (const auto& record : records) {
        if (record.type < 0) {
            const std::string header = valueOf(record.props, "HEADER");
            if (!header.empty()) {
                document.formatVersion = header;
                break;
            }
        }
    }

    // --- children by OWNERINDEX ---
    //
    // Newer files always carry OWNERINDEX. Very old exports sometimes omit it; in that
    // case primitives belong to the component that precedes them, so we fall back to the
    // most recent component record (this mirrors how Altium itself reads such files).
    std::map<int, std::vector<const SchRecord*>> children;
    int lastComponentIndex = -1;
    for (const auto& record : records) {
        if (record.index < 0) continue;
        if (record.type == 1) {
            lastComponentIndex = record.index;
            continue;
        }
        const Property* owner = text::find(record.props, "OWNERINDEX");
        int ownerIndex = owner != nullptr ? parseInt(owner->value, -1) : -1;
        if (ownerIndex < 0 && lastComponentIndex >= 0) {
            const bool componentChild = record.type == 2 || record.type == 34 || record.type == 41 ||
                                        record.type == 45 || record.type == 47;
            if (componentChild) ownerIndex = lastComponentIndex;
        }
        if (ownerIndex >= 0) children[ownerIndex].push_back(&record);
    }

    int pinCount = 0;
    for (const auto& record : records) {
        switch (record.type) {
            case 27: ++document.sch.wires; break;
            case 26: ++document.sch.buses; break;
            case 25: ++document.sch.netLabels; break;
            case 18: ++document.sch.ports; break;
            case 17: ++document.sch.powerPorts; break;
            case 29: ++document.sch.junctions; break;
            case 28: ++document.sch.texts; break;
            case 2: ++pinCount; break;
            default: break;
        }

        if (record.type != 1) continue;
        Component c;
        c.designator = valueOf(record.props, "DESIGNATOR");
        c.comment = valueOf(record.props, "COMMENT");
        c.name = valueOf(record.props, "LIBREFERENCE");
        if (c.name.empty()) c.name = valueOf(record.props, "DESIGNITEMID");
        c.description = valueOf(record.props, "COMPONENTDESCRIPTION");
        c.sourceLibrary = valueOf(record.props, "SOURCELIBRARYNAME");
        c.uniqueId = valueOf(record.props, "UNIQUEID");
        c.partCount = std::max(1, parseInt(valueOf(record.props, "PARTCOUNT", "2"), 2) - 1);
        for (const auto& property : record.props) c.properties.push_back(property);

        // Schematic units are 10 mil.
        c.x = parseMil(valueOf(record.props, "LOCATION.X")) * 10.0;
        c.y = parseMil(valueOf(record.props, "LOCATION.Y")) * 10.0;

        const auto it = children.find(record.index);
        if (it != children.end()) {
            for (const SchRecord* child : it->second) {
                if (child->type == 41) {
                    const std::string name = valueOf(child->props, "NAME");
                    const std::string value = valueOf(child->props, "TEXT");
                    if (name.empty()) continue;
                    c.properties.push_back(Property{name, value});
                    const std::string key = upper(name);
                    if (key == "DESIGNATOR" && c.designator.empty()) c.designator = value;
                    else if (key == "COMMENT" && c.comment.empty()) c.comment = value;
                    else if (key == "MANUFACTURER" && c.manufacturer.empty()) c.manufacturer = value;
                    else if ((key == "PART NUMBER" || key == "PARTNUMBER" || key == "MPN") &&
                             c.partNumber.empty())
                        c.partNumber = value;
                    else if ((key == "SUPPLIER 1" || key == "SUPPLIER") && c.supplier.empty())
                        c.supplier = value;
                    else if ((key == "SUPPLIER PART NUMBER 1" || key == "SUPPLIER PART NUMBER") &&
                             c.supplierPartNumber.empty())
                        c.supplierPartNumber = value;
                    else if ((key == "COMPONENTLINK1URL" || key == "DATASHEET" || key == "DATASHEET URL") &&
                             c.datasheet.empty())
                        c.datasheet = value;
                } else if (child->type == 34) {
                    // RECORD=34 is the designator text object; ASCII exports omit NAME.
                    const std::string name = upper(valueOf(child->props, "NAME"));
                    const std::string value = valueOf(child->props, "TEXT");
                    if ((name.empty() || name == "DESIGNATOR") && c.designator.empty())
                        c.designator = value;
                    else if (name == "COMMENT" && c.comment.empty())
                        c.comment = value;
                } else if (child->type == 2) {
                    Pin pin;
                    pin.name = valueOf(child->props, "NAME");
                    pin.designator = valueOf(child->props, "DESIGNATOR");
                    pin.electrical =
                        text::pinElectricalName(parseInt(valueOf(child->props, "ELECTRICAL", "0")));
                    pin.ownerPart = parseInt(valueOf(child->props, "OWNERPARTID", "1"), 1);
                    c.pins.push_back(pin);
                } else if (child->type == 44) {
                    // ImplementationList -> Implementation (RECORD=45) with MODELTYPE=PCBLIB.
                    const auto implIt = children.find(child->index);
                    if (implIt == children.end()) continue;
                    for (const SchRecord* implRecord : implIt->second) {
                        if (implRecord->type != 45) continue;
                        const std::string modelName = valueOf(implRecord->props, "MODELNAME");
                        const std::string modelType = upper(valueOf(implRecord->props, "MODELTYPE"));
                        const bool isCurrent =
                            upper(valueOf(implRecord->props, "ISCURRENT")) == "T";
                        if (modelName.empty() || modelType != "PCBLIB") continue;
                        if (c.footprint.empty() || isCurrent) c.footprint = modelName;
                    }
                } else if (child->type == 45) {
                    const std::string modelType = upper(valueOf(child->props, "MODELTYPE"));
                    if (modelType == "PCBLIB" && c.footprint.empty())
                        c.footprint = valueOf(child->props, "MODELNAME");
                }
            }
        }

        resolveReference(c.comment, c.properties);
        if (c.partNumber.empty()) resolveReference(c.partNumber, c.properties);

        c.pinCount = static_cast<int>(c.pins.size());
        document.components.push_back(std::move(c));
    }

    document.sch.components = static_cast<int>(document.components.size());
    document.sch.pins = pinCount;
}

void sortComponentPads(Document& document) {
    for (auto& component : document.components) {
        std::sort(component.pads.begin(), component.pads.end(),
                  [](const Pad& a, const Pad& b) { return text::naturalLess(a.designator, b.designator); });
    }
}

} // namespace detail

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::string docTypeName(DocType type) {
    switch (type) {
        case DocType::PcbDoc: return "PCB Document (.PcbDoc)";
        case DocType::SchDoc: return "Schematic Document (.SchDoc)";
        case DocType::PcbLib: return "PCB Library (.PcbLib)";
        case DocType::SchLib: return "Schematic Library (.SchLib)";
        default: return "Unknown";
    }
}

std::string containerName(Container container) {
    switch (container) {
        case Container::CompoundBinary: return "OLE2 compound file (binary Altium)";
        case Container::AsciiText: return "Altium ASCII text export";
        default: return "unknown container";
    }
}

void Bounds::add(double x, double y) {
    if (!valid) {
        minX = maxX = x;
        minY = maxY = y;
        valid = true;
        return;
    }
    minX = std::min(minX, x);
    maxX = std::max(maxX, x);
    minY = std::min(minY, y);
    maxY = std::max(maxY, y);
}

DocType detectType(const std::string& path) {
    if (text::endsWithNoCase(path, ".pcbdoc")) return DocType::PcbDoc;
    if (text::endsWithNoCase(path, ".schdoc")) return DocType::SchDoc;
    if (text::endsWithNoCase(path, ".pcblib")) return DocType::PcbLib;
    if (text::endsWithNoCase(path, ".schlib")) return DocType::SchLib;
    return DocType::Unknown;
}

ParseOutcome parseFile(const std::string& path) {
    ParseOutcome outcome;
    try {
        // ---- read the whole file ----
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            outcome.error = "cannot open file: " + path;
            return outcome;
        }
        in.seekg(0, std::ios::end);
        const std::streamoff size = in.tellg();
        in.seekg(0, std::ios::beg);
        if (size <= 0) {
            outcome.error = "file is empty: " + path;
            return outcome;
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        in.read(reinterpret_cast<char*>(bytes.data()), size);
        in.close();

        // ---- sniff the container ----
        std::vector<std::uint8_t> head(bytes.begin(),
                                       bytes.begin() + static_cast<std::ptrdiff_t>(
                                                           std::min<std::size_t>(bytes.size(), 65536)));
        outcome.signature = printableSignature(head);

        const bool isOle = startsWith(head, "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8);
        const bool isZip = startsWith(head, "PK\x03\x04", 4);
        const bool isPdf = startsWith(head, "%PDF", 4);

        Document& doc = outcome.document;
        doc.path = path;
        doc.fileName = [&path]() {
            const std::size_t slash = path.find_last_of("/\\");
            return slash == std::string::npos ? path : path.substr(slash + 1);
        }();
        doc.type = detectType(path);
        doc.fileSize = static_cast<std::uint64_t>(size);
        doc.signature = outcome.signature;

        if (isZip) {
            outcome.detected = "ZIP archive";
            outcome.error =
                "This file is a ZIP archive, not an Altium document. Extract it first, then open the "
                "contained .PcbDoc/.SchDoc.";
            return outcome;
        }
        if (isPdf) {
            outcome.detected = "PDF document";
            outcome.error = "This file is a PDF, not an Altium .PcbDoc/.SchDoc document.";
            return outcome;
        }

        if (doc.type == DocType::PcbLib || doc.type == DocType::SchLib) {
            outcome.detected = isOle ? "OLE2 compound file" : "unknown";
            outcome.error =
                "Library files (.PcbLib/.SchLib) are not supported yet — open a .PcbDoc or .SchDoc.";
            return outcome;
        }

        if (isOle) {
            outcome.detected = "OLE2 compound file (binary Altium)";
            CompoundFile cf;
            std::string error;
            if (!cf.openFromMemory(bytes, error)) {
                outcome.error = error;
                return outcome;
            }
            doc.structure = cf.root().children;

            if (doc.type == DocType::PcbDoc) {
                parseBinaryPcb(cf, doc);
            } else if (doc.type == DocType::SchDoc) {
                doc.container = Container::CompoundBinary;
                const std::vector<std::uint8_t> header = cf.stream("FileHeader");
                if (header.empty()) throw std::runtime_error("SchDoc has no FileHeader stream");
                const std::vector<detail::SchRecord> records = readBinarySchRecords(header);
                detail::buildSchDocument(records, doc);
            } else {
                outcome.error =
                    "Unknown file extension. Altium documents are .PcbDoc (board) or .SchDoc "
                    "(schematic).";
                return outcome;
            }
        } else if (looksLikeAsciiAltium(head)) {
            outcome.detected = "Altium ASCII export (plain text records)";
            const bool isPcb = asciiLooksLikePcb(bytes);
            if (isPcb) {
                doc.type = DocType::PcbDoc;
                detail::parseAsciiPcb(bytes, doc);
            } else {
                doc.type = DocType::SchDoc;
                detail::parseAsciiSch(bytes, doc);
            }
        } else {
            outcome.detected = "unrecognised";
            outcome.error =
                "This file is neither an Altium binary document (OLE2) nor an Altium ASCII export.\n"
                "File starts with: " + outcome.signature + "\n"
                "Make sure the file was really saved by Altium as .PcbDoc / .SchDoc.";
            return outcome;
        }

        if (doc.container == Container::AsciiText && doc.recordTypes.empty()) {
            outcome.error =
                "The ASCII file was recognised but contains no records (|RECORD=... lines).";
            return outcome;
        }

        outcome.ok = true;
    } catch (const std::exception& ex) {
        outcome.error = ex.what();
        outcome.ok = false;
    }
    return outcome;
}

} // namespace pcbv
