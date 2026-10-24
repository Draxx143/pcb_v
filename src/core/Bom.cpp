// Bom.cpp — see Bom.h.
#include "Bom.h"

#include <algorithm>
#include <map>
#include <sstream>

#include "AltiumText.h"

namespace pcbv {
namespace {

std::string lower(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return value;
}

bool isTrue(const std::string& value) {
    const std::string text = lower(text::trim(value));
    return text == "1" || text == "y" || text == "yes" || text == "true" || text == "x" ||
           text == "dnp" || text == "np" || text == "not fitted" || text == "nofit";
}

bool isFalse(const std::string& value) {
    const std::string text = lower(text::trim(value));
    return text == "0" || text == "n" || text == "no" || text == "false" || text == "none";
}

/// Parameter names that mean "this part is not assembled".
bool isDnpParameter(const std::string& name) {
    static const char* kNames[] = {"dnp", "dni", "do not populate", "do not place",
                                   "do not fit", "don't populate", "not fitted", "nofit",
                                   "no fit", "not populated", "bom ignore", "exclude from bom",
                                   "exclude from bom ", "assembly excluded"};
    const std::string key = lower(text::trim(name));
    for (const char* candidate : kNames) {
        if (key == candidate) return true;
    }
    return false;
}

/// Parameter names whose *false* value means "not assembled".
bool isFittedParameter(const std::string& name) {
    const std::string key = lower(text::trim(name));
    return key == "fitted" || key == "populate" || key == "populated" || key == "assembly" ||
           key == "assembled";
}

std::string escapeCsv(const std::string& value) {
    if (value.find_first_of(",\"\r\n") == std::string::npos) return value;
    std::string out = "\"";
    for (char c : value) {
        if (c == '"') out += '"';  // CSV escapes a quote by doubling it
        out += c;
    }
    out += '"';
    return out;
}

std::string htmlEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

/// Two components belong to the same BOM line when these fields match.
std::string groupKey(const Component& component) {
    std::string key = lower(text::trim(component.comment));
    key += '\x1f';
    key += lower(text::trim(component.footprint.empty() ? component.name : component.footprint));
    key += '\x1f';
    key += lower(text::trim(component.partNumber));
    key += '\x1f';
    key += lower(text::trim(component.manufacturer));
    return key;
}

} // namespace

std::string BomEntry::designatorList() const {
    std::string text;
    for (std::size_t i = 0; i < designators.size(); ++i) {
        if (i > 0) text += ", ";
        text += designators[i];
    }
    return text;
}

std::string BomEntry::label() const {
    if (!comment.empty()) return comment;
    if (!partNumber.empty()) return partNumber;
    if (!description.empty()) return description;
    return designators.empty() ? std::string("(unnamed)") : designators.front();
}

bool isDoNotPopulate(const Component& component) {
    for (const auto& property : component.properties) {
        if (isDnpParameter(property.name) && isTrue(property.value)) return true;
        if (isFittedParameter(property.name) && isFalse(property.value)) return true;
    }
    // Many boards simply write "DNP" into the value field.
    const std::string comment = lower(text::trim(component.comment));
    return comment == "dnp" || comment == "dni" || comment == "do not populate";
}

std::string componentLayers(const Component& component) {
    const std::string layer = text::trim(component.layer);
    if (layer.empty()) return std::string();
    const std::string key = lower(layer);
    if (key == "top" || key == "toplayer" || key == "top layer") return "Top";
    if (key == "bottom" || key == "bottomlayer" || key == "bottom layer") return "Bottom";
    return layer;
}

std::vector<BomEntry> buildBom(const Document& document) {
    std::map<std::string, std::size_t> byKey;
    std::vector<BomEntry> entries;

    for (const auto& component : document.components) {
        const std::string key = groupKey(component);
        auto it = byKey.find(key);
        if (it == byKey.end()) {
            BomEntry entry;
            entry.comment = component.comment;
            entry.footprint = component.footprint.empty() ? component.name : component.footprint;
            entry.partNumber = component.partNumber;
            entry.manufacturer = component.manufacturer;
            entry.description = component.description;
            entry.supplier = component.supplier;
            entry.supplierPartNumber = component.supplierPartNumber;
            entry.pins = component.pinCount;
            entry.pads = component.padCount;
            entry.throughHole = component.throughHolePads > 0;
            entry.dnp = isDoNotPopulate(component);
            it = byKey.emplace(key, entries.size()).first;
            entries.push_back(std::move(entry));
        }

        BomEntry& entry = entries[it->second];
        entry.quantity += 1;
        entry.pins = std::max(entry.pins, component.pinCount);
        entry.pads = std::max(entry.pads, component.padCount);
        entry.throughHole = entry.throughHole || component.throughHolePads > 0;
        entry.dnp = entry.dnp || isDoNotPopulate(component);
        if (entry.description.empty()) entry.description = component.description;
        if (entry.supplier.empty()) entry.supplier = component.supplier;
        if (entry.supplierPartNumber.empty()) entry.supplierPartNumber = component.supplierPartNumber;

        if (!component.designator.empty()) entry.designators.push_back(component.designator);

        const std::string layers = componentLayers(component);
        if (!layers.empty() && entry.layers.find(layers) == std::string::npos) {
            entry.layers += entry.layers.empty() ? layers : " + " + layers;
        }
    }

    for (auto& entry : entries) {
        std::sort(entry.designators.begin(), entry.designators.end(),
                  [](const std::string& a, const std::string& b) { return text::naturalLess(a, b); });
        if (entry.quantity == 0) entry.quantity = static_cast<int>(entry.designators.size());
    }

    // Order the lines by their first designator (so R1 comes before R10 before C1 only
    // when that is what the natural order of the designators says).
    std::sort(entries.begin(), entries.end(), [](const BomEntry& a, const BomEntry& b) {
        const std::string left = a.designators.empty() ? std::string("\xff") : a.designators.front();
        const std::string right = b.designators.empty() ? std::string("\xff") : b.designators.front();
        if (text::naturalLess(left, right)) return true;
        if (text::naturalLess(right, left)) return false;
        return a.label() < b.label();
    });
    return entries;
}

std::string bomToCsv(const Document& document) {
    const std::vector<BomEntry> entries = buildBom(document);
    std::ostringstream out;
    out << "Qty,Designators,Comment,Footprint,Manufacturer,PartNumber,Supplier,SupplierPartNumber,"
           "Description,Pins,Pads,Layers,DNP,File\r\n";
    for (const auto& entry : entries) {
        out << entry.quantity << ',' << escapeCsv(entry.designatorList()) << ','
            << escapeCsv(entry.comment) << ',' << escapeCsv(entry.footprint) << ','
            << escapeCsv(entry.manufacturer) << ',' << escapeCsv(entry.partNumber) << ','
            << escapeCsv(entry.supplier) << ',' << escapeCsv(entry.supplierPartNumber) << ','
            << escapeCsv(entry.description) << ',' << entry.pins << ',' << entry.pads << ','
            << escapeCsv(entry.layers) << ',' << (entry.dnp ? "DNP" : "") << ','
            << escapeCsv(document.fileName) << "\r\n";
    }
    return out.str();
}

std::string bomToHtml(const Document& document) {
    const std::vector<BomEntry> entries = buildBom(document);
    std::ostringstream out;
    out << "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
           "<title>BOM — "
        << htmlEscape(document.fileName)
        << "</title>\n<style>\n"
           "  body { font-family: -apple-system, 'Segoe UI', Roboto, Arial, sans-serif;"
           " margin: 32px; color: #16181d; }\n"
           "  h1 { font-size: 20px; margin: 0 0 4px 0; }\n"
           "  .meta { color: #5f6672; font-size: 13px; margin-bottom: 18px; }\n"
           "  table { border-collapse: collapse; width: 100%; font-size: 12.5px; }\n"
           "  th, td { text-align: left; padding: 6px 8px; border-bottom: 1px solid #e3e6ea;"
           " vertical-align: top; }\n"
           "  th { background: #f2f4f7; font-weight: 600; }\n"
           "  td.qty, th.qty { text-align: right; width: 48px; }\n"
           "  tr.dnp td { color: #8a929e; text-decoration: line-through; }\n"
           "  tr.dnp td:last-child { color: #c0392b; text-decoration: none; }\n"
           "  @media print { body { margin: 12mm; } th { background: #eee; } }\n"
           "</style>\n</head>\n<body>\n";
    out << "<h1>Bill of materials</h1>\n<div class=\"meta\">" << htmlEscape(document.fileName)
        << " · " << htmlEscape(bomSummary(document)) << "</div>\n";
    out << "<table>\n<thead><tr><th class=\"qty\">Qty</th><th>Designators</th><th>Value</th>"
           "<th>Footprint</th><th>Manufacturer</th><th>Part number</th><th>Description</th>"
           "<th>Note</th></tr></thead>\n<tbody>\n";
    for (const auto& entry : entries) {
        out << (entry.dnp ? "<tr class=\"dnp\">" : "<tr>");
        out << "<td class=\"qty\">" << entry.quantity << "</td>";
        out << "<td>" << htmlEscape(entry.designatorList()) << "</td>";
        out << "<td>" << htmlEscape(entry.comment) << "</td>";
        out << "<td>" << htmlEscape(entry.footprint) << "</td>";
        out << "<td>" << htmlEscape(entry.manufacturer) << "</td>";
        out << "<td>" << htmlEscape(entry.partNumber) << "</td>";
        out << "<td>" << htmlEscape(entry.description) << "</td>";
        out << "<td>" << (entry.dnp ? "DNP" : "") << "</td>";
        out << "</tr>\n";
    }
    out << "</tbody>\n</table>\n</body>\n</html>\n";
    return out.str();
}

std::string bomSummary(const Document& document) {
    const std::vector<BomEntry> entries = buildBom(document);
    int parts = 0;
    int dnp = 0;
    for (const auto& entry : entries) {
        parts += entry.quantity;
        if (entry.dnp) dnp += entry.quantity;
    }
    std::ostringstream out;
    out << entries.size() << (entries.size() == 1 ? " line" : " lines") << " · " << parts
        << (parts == 1 ? " part" : " parts");
    if (dnp > 0) out << " · " << dnp << " DNP";
    return out.str();
}

} // namespace pcbv
