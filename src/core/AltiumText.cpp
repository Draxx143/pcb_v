#include "AltiumText.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace pcbv {
namespace text {
namespace {

/// Windows-1252 code points for 0x80..0x9F (everything else maps 1:1 to Latin-1).
const std::uint32_t kCp1252High[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

struct Span {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;

    bool empty() const { return size == 0; }
    std::string ascii() const {
        return std::string(reinterpret_cast<const char*>(data), size);
    }
};

void setProperty(std::vector<Property>& properties, const std::string& name,
                 const std::string& value) {
    const std::string key = upper(name);
    for (auto& property : properties) {
        if (upper(property.name) == key) {
            property.value = value;
            return;
        }
    }
    properties.push_back(Property{name, value});
}

void appendToken(std::vector<Property>& properties, const Span& token, bool bytesAreUtf8) {
    if (token.empty()) return;
    std::size_t equals = token.size;
    for (std::size_t i = 0; i < token.size; ++i) {
        if (token.data[i] == '=') {
            equals = i;
            break;
        }
    }
    if (equals == 0 || equals == token.size) return; // no "KEY=" or empty key

    std::string key = fromCp1252(token.data, equals);
    std::string keyUpper = upper(key);
    const Span valueSpan{token.data + equals + 1, token.size - equals - 1};

    // Altium writes "%UTF8%<KEY>" companions carrying the real UTF-8 text.
    if (keyUpper.rfind("%UTF8%", 0) == 0) {
        const std::string baseName = key.substr(7);
        setProperty(properties, baseName, fromUtf8(valueSpan.data, valueSpan.size));
        return;
    }

    const std::string decoded =
        bytesAreUtf8 ? fromUtf8(valueSpan.data, valueSpan.size)
                     : fromCp1252(valueSpan.data, valueSpan.size);
    properties.push_back(Property{key, decoded});
}

} // namespace

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

std::string fromCp1252(const std::uint8_t* data, std::size_t length) {
    std::string out;
    out.reserve(length + length / 4);
    for (std::size_t i = 0; i < length; ++i) {
        const std::uint32_t b = data[i];
        if (b >= 0x80 && b <= 0x9F) {
            appendUtf8(out, kCp1252High[b - 0x80]);
        } else {
            appendUtf8(out, b);
        }
    }
    return out;
}

std::string fromCp1252(const std::string& raw) {
    return fromCp1252(reinterpret_cast<const std::uint8_t*>(raw.data()), raw.size());
}

std::string fromUtf8(const std::uint8_t* data, std::size_t length) {
    return std::string(reinterpret_cast<const char*>(data), length);
}

std::string upper(std::string value) {
    for (char& c : value) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return value;
}

std::string lower(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return value;
}

std::string trim(const std::string& value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    auto isSpace = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (begin < end && isSpace(value[begin])) ++begin;
    while (end > begin && isSpace(value[end - 1])) --end;
    return value.substr(begin, end - begin);
}

bool endsWithNoCase(const std::string& value, const std::string& suffix) {
    if (value.size() < suffix.size()) return false;
    return equalsNoCase(value.substr(value.size() - suffix.size()), suffix);
}

bool equalsNoCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

std::vector<Property> parseParameters(const std::uint8_t* data, std::size_t length,
                                      bool bytesAreUtf8) {
    std::vector<Property> properties;
    if (data == nullptr || length == 0) return properties;

    // Records are NUL terminated; only the part before the first NUL is meaningful.
    std::size_t limit = length;
    for (std::size_t i = 0; i < length; ++i) {
        if (data[i] == 0) {
            limit = i;
            break;
        }
    }

    std::size_t tokenStart = 0;
    for (std::size_t i = 0; i <= limit; ++i) {
        if (i == limit || data[i] == '|') {
            if (i > tokenStart) {
                appendToken(properties, Span{data + tokenStart, i - tokenStart}, bytesAreUtf8);
            }
            tokenStart = i + 1;
        }
    }
    return properties;
}

void applyUnicodeOverrides(std::vector<Property>& properties) {
    const Property* flag = find(properties, "UNICODE");
    if (flag == nullptr || upper(trim(flag->value)) != "EXISTS") return;

    std::vector<Property> decoded;
    std::vector<Property> overrides;
    for (const auto& property : properties) {
        const std::string key = upper(property.name);
        if (key == "UNICODE") continue;
        if (key.rfind("UNICODE__", 0) == 0) {
            overrides.push_back(Property{property.name.substr(std::strlen("UNICODE__")),
                                         property.value});
        } else {
            decoded.push_back(property);
        }
    }
    for (const auto& entry : overrides) {
        std::string decodedText;
        std::size_t start = 0;
        while (start <= entry.value.size()) {
            const std::size_t comma = entry.value.find(',', start);
            const std::string number =
                entry.value.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            const long codeUnit = std::strtol(trim(number).c_str(), nullptr, 10);
            if (codeUnit > 0 && codeUnit <= 0xFFFF)
                appendUtf8(decodedText, static_cast<std::uint32_t>(codeUnit));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        setProperty(decoded, entry.name, decodedText);
    }
    properties = std::move(decoded);
}

const Property* find(const std::vector<Property>& properties, const std::string& name) {
    const std::string needle = upper(name);
    for (const auto& property : properties) {
        if (upper(property.name) == needle) return &property;
    }
    return nullptr;
}

std::string valueOf(const std::vector<Property>& properties, const std::string& name,
                    const std::string& fallback) {
    const Property* property = find(properties, name);
    return property != nullptr ? property->value : fallback;
}

bool hasKey(const std::vector<Property>& properties, const std::string& name) {
    return find(properties, name) != nullptr;
}

double parseNumber(const std::string& input) {
    const std::string value = trim(input);
    if (value.empty()) return 0.0;
    char* end = nullptr;
    const double result = std::strtod(value.c_str(), &end);
    return end == value.c_str() ? 0.0 : result;
}

double parseMil(const std::string& input) {
    const std::string value = trim(input);
    if (value.empty()) return 0.0;

    // Strip a known unit suffix, then convert to mil.
    static const struct {
        const char* suffix;
        double factor; // -> mil
    } units[] = {{"mil", 1.0}, {"mm", 1000.0 / 25.4}, {"in", 1000.0}};

    const std::string lowered = lower(value);
    for (const auto& unit : units) {
        if (endsWithNoCase(lowered, unit.suffix)) {
            const std::string number = trim(value.substr(0, value.size() - std::strlen(unit.suffix)));
            return parseNumber(number) * unit.factor;
        }
    }
    // No unit: Altium's PCB ASCII writer uses mil for coordinates.
    return parseNumber(value);
}

int parseInt(const std::string& input, int fallback) {
    const std::string value = trim(input);
    if (value.empty()) return fallback;
    char* end = nullptr;
    const long result = std::strtol(value.c_str(), &end, 10);
    return end == value.c_str() ? fallback : static_cast<int>(result);
}

bool parseBool(const std::string& input, bool fallback) {
    const std::string value = upper(trim(input));
    if (value == "TRUE" || value == "T" || value == "1" || value == "YES") return true;
    if (value == "FALSE" || value == "F" || value == "0" || value == "NO") return false;
    return fallback;
}

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
        case 74: return "Multi-Layer";
        default: break;
    }
    if (layer >= 39 && layer <= 54) return "Internal Plane " + std::to_string(layer - 38);
    if (layer >= 57 && layer <= 72) return "Mechanical " + std::to_string(layer - 56);
    return "Layer " + std::to_string(layer);
}

int pcbLayerIdFromName(const std::string& input) {
    const std::string name = upper(trim(input));
    if (name.empty()) return 0;
    // Some exports write the layer as a plain number ("LAYER=1"); without this every such
    // primitive would end up on layer 0 and be drawn in the neutral "unknown" colour.
    bool numeric = !name.empty();
    for (const char c : name) {
        if (c < '0' || c > '9') {
            numeric = false;
            break;
        }
    }
    if (numeric) {
        const int layer = parseInt(name, -1);
        if (layer >= 0 && layer <= 74) return layer;
    }
    if (name == "TOP" || name == "TOP LAYER" || name == "TOPLAYER" || name == "TOPCOPPER") return 1;
    if (name == "BOTTOM" || name == "BOTTOM LAYER" || name == "BOTTOMLAYER" ||
        name == "BOTTOMCOPPER")
        return 32;
    if (name == "MULTILAYER" || name == "MULTI-LAYER" || name == "MULTI LAYER") return 74;
    if (name == "TOPOVERLAY" || name == "TOP OVERLAY") return 33;
    if (name == "BOTTOMOVERLAY" || name == "BOTTOM OVERLAY") return 34;
    if (name == "TOPPASTE" || name == "TOP PASTE") return 35;
    if (name == "BOTTOMPASTE" || name == "BOTTOM PASTE") return 36;
    if (name == "TOPSOLDER" || name == "TOP SOLDER") return 37;
    if (name == "BOTTOMSOLDER" || name == "BOTTOM SOLDER") return 38;
    if (name == "KEEPOUT" || name == "KEEP-OUT" || name == "KEEPOUTLAYER") return 56;
    if (name == "DRILLGUIDE") return 55;
    if (name == "DRILLDRAWING") return 57;

    auto numericSuffix = [&name](const char* prefix) -> int {
        const std::size_t length = std::strlen(prefix);
        if (name.rfind(prefix, 0) != 0) return -1;
        return parseInt(name.substr(length), -1);
    };
    if (const int mid = numericSuffix("MIDLAYER"); mid > 0) return mid + 1;
    if (const int plane = numericSuffix("INTERNALPLANE"); plane > 0) return plane + 38;
    if (const int mechanical = numericSuffix("MECHANICAL"); mechanical >= 0) return 56 + mechanical;
    return 0;
}

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

bool naturalLess(const std::string& a, const std::string& b) {
    std::size_t i = 0;
    std::size_t j = 0;
    auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
    while (i < a.size() && j < b.size()) {
        if (isDigit(a[i]) && isDigit(b[j])) {
            std::size_t endA = i;
            std::size_t endB = j;
            while (endA < a.size() && isDigit(a[endA])) ++endA;
            while (endB < b.size() && isDigit(b[endB])) ++endB;
            const long long valueA = std::strtoll(a.substr(i, endA - i).c_str(), nullptr, 10);
            const long long valueB = std::strtoll(b.substr(j, endB - j).c_str(), nullptr, 10);
            if (valueA != valueB) return valueA < valueB;
            i = endA;
            j = endB;
        } else {
            const unsigned char ca = static_cast<unsigned char>(a[i]);
            const unsigned char cb = static_cast<unsigned char>(b[j]);
            if (ca != cb) return ca < cb;
            ++i;
            ++j;
        }
    }
    return (a.size() - i) < (b.size() - j);
}

bool isValidUtf8(const std::uint8_t* data, std::size_t length) {
    std::size_t i = 0;
    while (i < length) {
        const std::uint8_t c = data[i];
        int extra = 0;
        if (c < 0x80) {
            ++i;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
        } else {
            return false;
        }
        if (i + static_cast<std::size_t>(extra) >= length + 0 && i + static_cast<std::size_t>(extra) > length - 1) {
            // truncated sequence at the very end: tolerate it (we may be sniffing a prefix)
            return true;
        }
        for (int k = 1; k <= extra; ++k) {
            if ((data[i + static_cast<std::size_t>(k)] & 0xC0) != 0x80) return false;
        }
        i += static_cast<std::size_t>(extra) + 1;
    }
    return true;
}

} // namespace text
} // namespace pcbv
