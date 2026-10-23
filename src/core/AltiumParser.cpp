#include "AltiumParser.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

namespace pcbv {
namespace {

// ---------------------------------------------------------------------------
// Text decoding
// ---------------------------------------------------------------------------

/// Windows-1252 code points for 0x80..0x9F (the rest maps 1:1 to Latin-1).
const std::uint32_t kCp1252High[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

/// Converts a Windows-1252 byte range (Altium's parameter encoding) to UTF-8.
std::string cp1252ToUtf8(const std::uint8_t* data, std::size_t len) {
    std::string out;
    out.reserve(len + len / 4);
    for (std::size_t i = 0; i < len; ++i) {
        const std::uint32_t b = data[i];
        if (b >= 0x80 && b <= 0x9F) {
            appendUtf8(out, kCp1252High[b - 0x80]);
        } else {
            appendUtf8(out, b);
        }
    }
    return out;
}

std::string upper(std::string s) {
    for (char& c : s) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return s;
}

std::string toLowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::string trim(const std::string& s) {
    std::size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    std::size_t b = s.size();
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

bool endsWithNoCase(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    return toLowerAscii(s.substr(s.size() - suffix.size())) == toLowerAscii(suffix);
}

std::string baseName(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

double parseDouble(const std::string& text) {
    std::string s = trim(text);
    if (s.empty()) return 0.0;
    // Altium writes values like "2133.8582mil" or " 1.80000000000000E+0002".
    const std::size_t unit = s.find_first_of("mMuUiI"); // mil / mm / in
    if (unit != std::string::npos && unit > 0) {
        char* end = nullptr;
        const double value = std::strtod(s.substr(0, unit).c_str(), &end);
        const std::string suffix = toLowerAscii(s.substr(unit));
        if (end != nullptr) {
            if (suffix.rfind("mil", 0) == 0) return value;
            if (suffix.rfind("mm", 0) == 0) return value / 0.0254;
            if (suffix.rfind("in", 0) == 0) return value * 1000.0;
        }
    }
    char* end = nullptr;
    const double value = std::strtod(s.c_str(), &end);
    return (end == s.c_str()) ? 0.0 : value;
}

int parseInt(const std::string& text) {
    const std::string s = trim(text);
    if (s.empty()) return 0;
    return static_cast<int>(std::strtol(s.c_str(), nullptr, 10));
}

// ---------------------------------------------------------------------------
// Parameter lists ("|KEY=VALUE|KEY=VALUE")
// ---------------------------------------------------------------------------

void decodeUnicodeProperties(std::vector<Property>& props);

std::vector<Property> parseParameterText(const std::uint8_t* data, std::size_t len) {
    // Records are NUL-terminated C strings; only the part before the first NUL counts.
    std::size_t effective = len;
    for (std::size_t i = 0; i < len; ++i) {
        if (data[i] == 0) {
            effective = i;
            break;
        }
    }
    const std::string text = cp1252ToUtf8(data, effective);

    std::vector<Property> props;
    std::string token;
    auto flush = [&]() {
        if (token.empty()) return;
        const std::size_t eq = token.find('=');
        if (eq != std::string::npos) {
            Property p;
            p.name = trim(token.substr(0, eq));
            p.value = token.substr(eq + 1);
            if (!p.name.empty()) props.push_back(std::move(p));
        }
        token.clear();
    };
    for (char c : text) {
        if (c == '|') {
            flush();
        } else {
            token += c;
        }
    }
    flush();
    decodeUnicodeProperties(props);
    return props;
}

const Property* findProp(const std::vector<Property>& props, const std::string& name) {
    const std::string needle = upper(name);
    for (const auto& p : props) {
        if (upper(p.name) == needle) return &p;
    }
    return nullptr;
}

std::string propValue(const std::vector<Property>& props, const std::string& name,
                      const std::string& fallback = std::string()) {
    const Property* p = findProp(props, name);
    return p != nullptr ? p->value : fallback;
}

/// Altium stores non-Latin-1 text as |UNICODE=EXISTS|UNICODE__KEY=1,2,3 (UTF-16 code units).
void decodeUnicodeProperties(std::vector<Property>& props) {
    const Property* flag = findProp(props, "UNICODE");
    if (flag == nullptr || upper(trim(flag->value)) != "EXISTS") return;

    std::vector<Property> decoded;
    std::vector<Property> unicodeValues;
    for (const auto& p : props) {
        const std::string key = upper(p.name);
        if (key == "UNICODE") continue;
        if (key.rfind("UNICODE__", 0) == 0) {
            unicodeValues.emplace_back(Property{p.name.substr(9), p.value});
        } else {
            decoded.push_back(p);
        }
    }
    for (auto& uv : unicodeValues) {
        std::string text;
        std::size_t start = 0;
        while (start <= uv.value.size()) {
            const std::size_t comma = uv.value.find(',', start);
            const std::string num =
                uv.value.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            const long cu = std::strtol(trim(num).c_str(), nullptr, 10);
            if (cu > 0 && cu <= 0xFFFF) appendUtf8(text, static_cast<std::uint32_t>(cu));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        bool replaced = false;
        for (auto& d : decoded) {
            if (upper(d.name) == upper(uv.name)) {
                d.value = text;
                replaced = true;
                break;
            }
        }
        if (!replaced) decoded.emplace_back(Property{uv.name, text});
    }
    props = std::move(decoded);
}

// ---------------------------------------------------------------------------
// Binary helpers
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
    double v = 0.0;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

/// Iterates over [uint32 size][payload] blocks inside a stream.
class BlockStream {
public:
    explicit BlockStream(const std::vector<std::uint8_t>& data) : data_(data) {}

    bool atEnd() const { return pos_ + 4 > data_.size(); }

    /// Reads the next block. Returns false at end of data or on a corrupt size.
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

    /// Reads a block and returns its payload as a vector, or empty vector.
    std::vector<std::uint8_t> nextRaw() {
        std::vector<std::uint8_t> payload;
        if (!next(payload)) return {};
        return payload;
    }

    std::size_t position() const { return pos_; }
    std::size_t size() const { return data_.size(); }
    void setPosition(std::size_t position) { pos_ = position; }
    const std::vector<std::uint8_t>& data() const { return data_; }

private:
    const std::vector<std::uint8_t>& data_;
    std::size_t pos_ = 0;
};

/// Reads a "string block": a size-prefixed block holding a 1-byte length + text.
///
/// Altium writes the length prefix equal to the string length but the block size equal to
/// that length as well (not length + 1), so the last character sits one byte past the block.
/// We therefore allow a one byte overrun when decoding, which matches Altium's own readers.
std::string readStringBlock(BlockStream& bs) {
    const std::vector<std::uint8_t>& data = bs.data();
    const std::size_t start = bs.position();
    if (start + 4 > data.size()) {
        bs.setPosition(data.size());
        return {};
    }
    const std::uint32_t size = rdU32(data, start) & kBlockSizeMask;
    std::size_t pos = start + 4;
    if (size == 0 || pos >= data.size()) {
        bs.setPosition(std::min<std::size_t>(pos + size, data.size()));
        return {};
    }
    const std::size_t declared = data[pos];
    const std::size_t available = std::min<std::size_t>(declared, data.size() - (pos + 1));
    const std::string text = cp1252ToUtf8(data.data() + pos + 1, available);
    bs.setPosition(std::min<std::size_t>(pos + size, data.size()));
    return text;
}

/// Counts records that consist of one object-id byte + one size-prefixed block each.
/// Returns -1 when the stream does not follow that layout (safety net).
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
// PCB layer names
// ---------------------------------------------------------------------------

std::string pcbLayerName(int layer) {
    if (layer == 1) return "Top Layer";
    if (layer == 32) return "Bottom Layer";
    if (layer >= 2 && layer <= 31) return "Mid Layer " + std::to_string(layer - 1);
    switch (layer) {
        case 33: return "Top Overlay";
        case 34: return "Bottom Overlay";
        case 35: return "Top Paste";
        case 36: return "Bottom Paste";
        case 37: return "Top Solder";
        case 38: return "Bottom Solder";
        case 55: return "Drill Guide";
        case 56: return "Keep-Out Layer";
        case 57: return "Drill Drawing";
        case 73: return "Mechanical 16";
        case 74: return "Multi-Layer";
        default: break;
    }
    if (layer >= 39 && layer <= 54) return "Internal Plane " + std::to_string(layer - 38);
    if (layer >= 57 && layer <= 72) return "Mechanical " + std::to_string(layer - 56);
    return "Layer " + std::to_string(layer);
}

// ---------------------------------------------------------------------------
// Schematic pin electrical types
// ---------------------------------------------------------------------------

std::string pinElectricalName(int type) {
    switch (type) {
        case 0: return "Input";
        case 1: return "I/O";
        case 2: return "Output";
        case 3: return "Open Collector";
        case 4: return "Passive";
        case 5: return "Hi-Z";
        case 6: return "Open Emitter";
        case 7: return "Power";
        default: return "Unknown";
    }
}

// ---------------------------------------------------------------------------
// PcbDoc
// ---------------------------------------------------------------------------

/// Reads one Pads6 record starting at *pos (which points at the object id byte).
bool readPadRecord(const std::vector<std::uint8_t>& data, std::size_t& pos, Pad& pad,
                   int& componentIndex, int& netIndex) {
    if (pos >= data.size()) return false;
    ++pos; // object id (always 2 for pads)

    // Three string blocks: designator, sub-record 2, net string.
    for (int i = 0; i < 3; ++i) {
        if (pos + 4 > data.size()) return false;
        const std::uint32_t size = rdU32(data, pos) & kBlockSizeMask;
        pos += 4;
        if (size == 0) continue;
        if (pos + size > data.size()) return false;
        if (i == 0 && size >= 1) {
            const std::size_t len = std::min<std::size_t>(data[pos], size - 1);
            pad.designator = cp1252ToUtf8(data.data() + pos + 1, len);
        }
        pos += size;
    }

    // One skipped block (pad cache / bga data).
    if (pos + 4 > data.size()) return false;
    const std::uint32_t skipSize = rdU32(data, pos) & kBlockSizeMask;
    pos += 4 + skipSize;

    // Main block (SubRecord 5).
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

    pad.x = static_cast<double>(rdI32(data, main + 13)) / 10000.0;   // 1/10000 mil units
    pad.y = static_cast<double>(rdI32(data, main + 17)) / 10000.0;
    pad.width = static_cast<double>(rdI32(data, main + 21)) / 10000.0;
    pad.height = static_cast<double>(rdI32(data, main + 25)) / 10000.0;
    pad.holeSize = static_cast<double>(rdI32(data, main + 45)) / 10000.0;
    pad.rotation = rdF64(data, main + 52);
    pad.plated = rdB(data, main + 60) != 0;
    pad.layerName = pcbLayerName(pad.layer);
    pos = main + mainSize;

    // SubRecord 6 (per-layer size/shape table) — nothing needed, just skip it.
    if (pos + 4 <= data.size()) {
        const std::uint32_t sr6 = rdU32(data, pos) & kBlockSizeMask;
        if (pos + 4 + sr6 <= data.size()) pos += 4 + sr6;
    }
    return true;
}

void parsePcbDoc(CompoundFile& cf, Document& doc) {
    // --- version stamps ---
    const std::vector<std::uint8_t> fh6 = cf.stream("FileHeaderSix");
    if (!fh6.empty()) {
        BlockStream bs(fh6);
        std::string version = readStringBlock(bs);
        while (!version.empty() && (version.back() == '\n' || version.back() == '\r')) version.pop_back();
        doc.formatVersion = version;
    }
    if (doc.formatVersion.empty()) {
        const std::string fh = cf.streamText("FileHeader");
        doc.formatVersion = fh.empty() ? "Altium PCB binary" : "Altium PCB binary (legacy)";
    }

    // --- board parameters ---
    const std::vector<std::uint8_t> boardData = cf.stream("Board6/Data");
    if (!boardData.empty()) {
        BlockStream bs(boardData);
        std::vector<std::uint8_t> payload;
        if (bs.next(payload)) {
            const std::vector<Property> props = parseParameterText(payload.data(), payload.size());
            doc.boardVersion = propValue(props, "VERSION");
            const std::string date = propValue(props, "DATE");
            const std::string time = propValue(props, "TIME");
            if (!date.empty() || !time.empty()) {
                doc.lastSaved = trim(date + " " + time);
            }
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
            const std::vector<Property> props = parseParameterText(payload.data(), payload.size());
            Net net;
            net.index = index;
            net.name = propValue(props, "NAME");
            if (net.name.empty()) net.name = propValue(props, "NETNAME");
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
            const std::vector<Property> props = parseParameterText(payload.data(), payload.size());
            if (props.empty()) {
                ++index;
                continue;
            }
            Component c;
            c.componentIndex = index;

            std::string designator = propValue(props, "SOURCEDESIGNATOR");
            if (designator.empty()) designator = propValue(props, "DESIGNATOR");
            c.designator = designator;
            c.name = propValue(props, "SOURCELIBREFERENCE");
            c.footprint = propValue(props, "PATTERN");
            c.comment = propValue(props, "COMMENT");
            if (c.comment.empty()) c.comment = propValue(props, "SOURCELIBREFERENCE");
            c.description = propValue(props, "DESCRIPTION");
            if (c.description.empty()) c.description = propValue(props, "FOOTPRINTDESCRIPTION");
            c.layer = propValue(props, "LAYER");
            c.sourceLibrary = propValue(props, "SOURCECOMPONENTLIBRARY");
            c.sourceFootprintLibrary = propValue(props, "SOURCEFOOTPRINTLIBRARY");
            c.uniqueId = propValue(props, "UNIQUEID");
            c.manufacturer = propValue(props, "MANUFACTURER");
            c.partNumber = propValue(props, "SOURCECOMPDESIGNITEMID");
            if (c.partNumber.empty()) c.partNumber = propValue(props, "PARTNUMBER");
            c.supplier = propValue(props, "SUPPLIER");
            c.supplierPartNumber = propValue(props, "SUPPLIERPARTNUMBER");
            c.datasheet = propValue(props, "DATASHEET");
            c.x = parseDouble(propValue(props, "X"));
            c.y = parseDouble(propValue(props, "Y"));
            c.rotation = parseDouble(propValue(props, "ROTATION"));
            c.height = parseDouble(propValue(props, "HEIGHT"));

            // Keep every raw parameter so nothing is hidden from the user.
            for (const auto& p : props) c.properties.push_back(p);

            doc.components.push_back(std::move(c));
            ++index;
        }
        doc.pcb.nets = static_cast<int>(doc.nets.size());
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
            if (componentIndex >= 0 && componentIndex < static_cast<int>(doc.components.size())) {
                Component& c = doc.components[static_cast<std::size_t>(componentIndex)];
                c.pads.push_back(pad);
                ++c.padCount;
                if (pad.holeSize > 0.0) ++c.throughHolePads;
            }
            ++parsed;
            if (pos == before) break; // safety net against malformed data
        }
        doc.pcb.pads = parsed;
        if (parsed == 0) doc.warnings.push_back("Pads6 stream could not be decoded.");
    }

    // --- tracks / vias: counts and board extents ---
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
            doc.bounds.add(static_cast<double>(rdI32(trackData, pos + 13)) / 10000.0,
                           static_cast<double>(rdI32(trackData, pos + 17)) / 10000.0);
            doc.bounds.add(static_cast<double>(rdI32(trackData, pos + 21)) / 10000.0,
                           static_cast<double>(rdI32(trackData, pos + 25)) / 10000.0);
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
            doc.bounds.add(static_cast<double>(rdI32(viaData, pos + 13)) / 10000.0,
                           static_cast<double>(rdI32(viaData, pos + 17)) / 10000.0);
            pos += size;
            ++count;
            if (pos == before) break;
        }
        doc.pcb.vias = count;
    }

    // --- remaining primitive counters (only accepted when the layout is fully consistent) ---
    struct Counter {
        const char* storage;
        int PcbCounts::*field;
    };
    static const Counter counters[] = {
        {"Arcs6/Data", &PcbCounts::arcs},
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
}

// ---------------------------------------------------------------------------
// SchDoc
// ---------------------------------------------------------------------------

struct SchRecord {
    int index = 0;                 // object index used by OWNERINDEX
    int type = -1;                 // RECORD value, -1 when the record has none
    bool binary = false;
    std::vector<Property> props;
};

void parseSchDoc(CompoundFile& cf, Document& doc) {
    const std::vector<std::uint8_t> header = cf.stream("FileHeader");
    if (header.empty()) throw std::runtime_error("SchDoc has no FileHeader stream");

    std::vector<SchRecord> records;
    std::size_t pos = 0;
    int objectIndex = 0;
    while (pos + 4 <= header.size()) {
        const std::uint32_t sizeHeader = rdU32(header, pos);
        const std::uint32_t size = sizeHeader & kBlockSizeMask;
        const std::uint8_t flags = static_cast<std::uint8_t>((sizeHeader >> 24) & 0xFF);
        pos += 4;
        if (size == 0 || pos + size > header.size()) break;

        SchRecord rec;
        rec.binary = (flags == 0x01);
        if (rec.binary) {
            // Binary pin record: [int32 type=2][...] — pins are decoded separately below.
            rec.type = (size >= 4) ? rdI32(header, pos) : -1;
        } else {
            rec.props = parseParameterText(header.data() + pos, size);
            const Property* typeProp = findProp(rec.props, "RECORD");
            rec.type = typeProp != nullptr ? parseInt(typeProp->value) : -1;
        }
        pos += size;

        // The first record is the file header and is not part of the primitive index space.
        const bool isFileHeader = records.empty() && rec.type < 0;
        rec.index = isFileHeader ? -1 : objectIndex++;
        records.push_back(std::move(rec));
    }

    // --- version string ---
    for (const auto& rec : records) {
        if (rec.type < 0) {
            const std::string headerText = propValue(rec.props, "HEADER");
            if (!headerText.empty()) {
                doc.formatVersion = headerText;
                break;
            }
        }
    }
    if (doc.formatVersion.empty()) doc.formatVersion = "Altium schematic binary";

    // --- collect components and their children ---
    std::map<int, std::vector<const SchRecord*>> children;
    for (const auto& rec : records) {
        if (rec.index < 0) continue;
        const Property* owner = findProp(rec.props, "OWNERINDEX");
        if (owner != nullptr) {
            children[parseInt(owner->value)].push_back(&rec);
        }
    }

    int pinCount = 0;
    for (const auto& rec : records) {
        switch (rec.type) {
            case 27: ++doc.sch.wires; break;
            case 26: ++doc.sch.buses; break;
            case 25: ++doc.sch.netLabels; break;
            case 18: ++doc.sch.ports; break;
            case 17: ++doc.sch.powerPorts; break;
            case 29: ++doc.sch.junctions; break;
            case 28: ++doc.sch.texts; break;
            case 2: ++pinCount; break;
            default: break;
        }

        if (rec.type != 1) continue;
        Component c;
        c.designator = propValue(rec.props, "DESIGNATOR");
        c.comment = propValue(rec.props, "COMMENT");
        c.name = propValue(rec.props, "LIBREFERENCE");
        if (c.name.empty()) c.name = propValue(rec.props, "DESIGNITEMID");
        c.description = propValue(rec.props, "COMPONENTDESCRIPTION");
        c.sourceLibrary = propValue(rec.props, "SOURCELIBRARYNAME");
        c.uniqueId = propValue(rec.props, "UNIQUEID");
        c.partCount = std::max(1, parseInt(propValue(rec.props, "PARTCOUNT", "2")) - 1);
        for (const auto& p : rec.props) c.properties.push_back(p);

        const double locationX = parseDouble(propValue(rec.props, "LOCATION.X"));
        const double locationY = parseDouble(propValue(rec.props, "LOCATION.Y"));
        c.x = locationX * 10.0; // schematic units are 10 mil
        c.y = locationY * 10.0;

        const auto it = children.find(rec.index);
        if (it != children.end()) {
            for (const SchRecord* child : it->second) {
                if (child->type == 41) {
                    const std::string name = propValue(child->props, "NAME");
                    const std::string value = propValue(child->props, "TEXT");
                    if (name.empty()) continue;
                    c.properties.push_back(Property{name, value});
                    const std::string key = upper(name);
                    if (key == "DESIGNATOR" && c.designator.empty()) c.designator = value;
                    else if (key == "COMMENT" && c.comment.empty()) c.comment = value;
                    else if (key == "MANUFACTURER" && c.manufacturer.empty()) c.manufacturer = value;
                    else if ((key == "PART NUMBER" || key == "PARTNUMBER" || key == "MPN") &&
                             c.partNumber.empty()) c.partNumber = value;
                    else if ((key == "SUPPLIER 1" || key == "SUPPLIER") && c.supplier.empty()) c.supplier = value;
                    else if ((key == "SUPPLIER PART NUMBER 1" || key == "SUPPLIER PART NUMBER") &&
                             c.supplierPartNumber.empty()) c.supplierPartNumber = value;
                    else if ((key == "COMPONENTLINK1URL" || key == "DATASHEET" || key == "DATASHEET URL") &&
                             c.datasheet.empty()) c.datasheet = value;
                } else if (child->type == 34) {
                    const std::string name = upper(propValue(child->props, "NAME"));
                    const std::string text = propValue(child->props, "TEXT");
                    if (name == "DESIGNATOR" && c.designator.empty()) c.designator = text;
                    else if (name == "COMMENT" && c.comment.empty()) c.comment = text;
                } else if (child->type == 2) {
                    Pin pin;
                    pin.name = propValue(child->props, "NAME");
                    pin.designator = propValue(child->props, "DESIGNATOR");
                    pin.electrical = pinElectricalName(parseInt(propValue(child->props, "ELECTRICAL", "0")));
                    pin.ownerPart = parseInt(propValue(child->props, "OWNERPARTID", "1"));
                    c.pins.push_back(pin);
                } else if (child->type == 44) {
                    // ImplementationList: its children (RECORD=45) carry the footprint model.
                    const auto implIt = children.find(child->index);
                    if (implIt != children.end()) {
                        for (const SchRecord* implRec : implIt->second) {
                            if (implRec->type != 45) continue;
                            const std::string modelName = propValue(implRec->props, "MODELNAME");
                            const std::string modelType = upper(propValue(implRec->props, "MODELTYPE"));
                            const bool isCurrent = upper(propValue(implRec->props, "ISCURRENT")) == "T";
                            if (modelName.empty() || modelType != "PCBLIB") continue;
                            if (c.footprint.empty() || isCurrent) c.footprint = modelName;
                        }
                    }
                } else if (child->type == 45) {
                    const std::string modelType = upper(propValue(child->props, "MODELTYPE"));
                    const std::string modelName = propValue(child->props, "MODELNAME");
                    if (modelType == "PCBLIB" && c.footprint.empty()) c.footprint = modelName;
                }
            }
        }

        // Resolve Altium "=Parameter" indirection (e.g. Comment "=Part Number").
        auto resolveReference = [&c](std::string& value) {
            if (value.size() < 2 || value[0] != '=') return;
            const std::string reference = trim(value.substr(1));
            const std::string key = upper(reference);
            for (const auto& p : c.properties) {
                if (upper(p.name) == key && !p.value.empty()) {
                    value = p.value;
                    return;
                }
            }
        };
        resolveReference(c.comment);
        if (c.partNumber.empty()) resolveReference(c.partNumber);

        c.pinCount = static_cast<int>(c.pins.size());
        doc.components.push_back(std::move(c));
    }

    doc.sch.components = static_cast<int>(doc.components.size());
    doc.sch.pins = pinCount;
}

} // namespace

std::string docTypeName(DocType type) {
    switch (type) {
        case DocType::PcbDoc: return "PCB Document (.PcbDoc)";
        case DocType::SchDoc: return "Schematic Document (.SchDoc)";
        case DocType::PcbLib: return "PCB Library (.PcbLib)";
        case DocType::SchLib: return "Schematic Library (.SchLib)";
        default: return "Unknown";
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
    if (endsWithNoCase(path, ".pcbdoc")) return DocType::PcbDoc;
    if (endsWithNoCase(path, ".schdoc")) return DocType::SchDoc;
    if (endsWithNoCase(path, ".pcblib")) return DocType::PcbLib;
    if (endsWithNoCase(path, ".schlib")) return DocType::SchLib;
    return DocType::Unknown;
}

ParseOutcome parseFile(const std::string& path) {
    ParseOutcome outcome;
    try {
        CompoundFile cf;
        std::string error;
        if (!cf.openFromFile(path, error)) {
            outcome.error = error;
            return outcome;
        }

        Document& doc = outcome.document;
        doc.path = path;
        doc.fileName = baseName(path);
        doc.type = detectType(path);
        doc.structure = cf.root().children;

        {
            std::ifstream sizeProbe(path, std::ios::binary | std::ios::ate);
            if (sizeProbe) doc.fileSize = static_cast<std::uint64_t>(sizeProbe.tellg());
        }

        switch (doc.type) {
            case DocType::PcbDoc:
                parsePcbDoc(cf, doc);
                break;
            case DocType::SchDoc:
                parseSchDoc(cf, doc);
                break;
            case DocType::PcbLib:
            case DocType::SchLib:
                outcome.error = "Library files (.PcbLib/.SchLib) are not supported yet — open a .PcbDoc or .SchDoc.";
                return outcome;
            default:
                outcome.error = "Unknown file type — expected .PcbDoc or .SchDoc.";
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
