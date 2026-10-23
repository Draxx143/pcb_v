// Compare.cpp — see Compare.h.
#include "Compare.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>

#include "AltiumText.h"
#include "Bom.h"
#include "Export.h"

namespace pcbv {
namespace {

std::string upper(std::string value) {
    for (char& c : value) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return value;
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

/// Designators are the identity of a part; fall back to the unique id when empty.
std::string keyOf(const Component& component) {
    std::string key = upper(text::trim(component.designator));
    if (!key.empty()) return key;
    if (!component.uniqueId.empty()) return "UID:" + component.uniqueId;
    return "?" + component.comment + "|" + component.footprint;
}

std::string position(const Component& component) {
    return formatMm(component.x) + ", " + formatMm(component.y) + " mm";
}

std::string padSummary(const Component& component) {
    std::string text = std::to_string(component.padCount);
    if (component.pinCount > 0) text += " pads / " + std::to_string(component.pinCount) + " pins";
    else text += " pads";
    return text;
}

void compareField(std::vector<FieldChange>& changes, const char* field, const std::string& before,
                  const std::string& after) {
    if (text::trim(before) == text::trim(after)) return;
    changes.push_back(FieldChange{field, before, after});
}

std::string describe(const Component& component) {
    std::string text = component.comment;
    if (text.empty()) text = component.partNumber;
    if (text.empty()) text = component.description;
    if (text.empty()) text = component.footprint;
    return text;
}

} // namespace

CompareResult compareDocuments(const Document& left, const Document& right) {
    CompareResult result;
    result.leftName = left.fileName.empty() ? left.path : left.fileName;
    result.rightName = right.fileName.empty() ? right.path : right.fileName;
    result.leftCount = left.components.size();
    result.rightCount = right.components.size();

    std::map<std::string, const Component*> leftByKey;
    for (const auto& component : left.components) leftByKey[keyOf(component)] = &component;

    std::set<std::string> seen;
    for (const auto& component : right.components) {
        const std::string key = keyOf(component);
        seen.insert(key);
        auto it = leftByKey.find(key);
        if (it == leftByKey.end()) {
            result.added.push_back(component);
            continue;
        }

        const Component& before = *it->second;
        ChangedPart part;
        part.designator = component.designator.empty() ? before.designator : component.designator;
        part.label = describe(component);

        compareField(part.changes, "Comment", before.comment, component.comment);
        compareField(part.changes, "Footprint", before.footprint.empty() ? before.name : before.footprint,
                     component.footprint.empty() ? component.name : component.footprint);
        compareField(part.changes, "Part number", before.partNumber, component.partNumber);
        compareField(part.changes, "Manufacturer", before.manufacturer, component.manufacturer);
        compareField(part.changes, "Layer", before.layer, component.layer);
        compareField(part.changes, "Pads", padSummary(before), padSummary(component));
        if (std::fabs(before.rotation - component.rotation) > 0.01) {
            part.changes.push_back(FieldChange{"Rotation", formatMil(before.rotation) + "°",
                                               formatMil(component.rotation) + "°"});
        }
        if (std::fabs(before.x - component.x) > 1.0 || std::fabs(before.y - component.y) > 1.0) {
            part.changes.push_back(FieldChange{"Position", position(before), position(component)});
        }
        if (isDoNotPopulate(before) != isDoNotPopulate(component)) {
            part.changes.push_back(FieldChange{"Fitted", isDoNotPopulate(before) ? "no (DNP)" : "yes",
                                               isDoNotPopulate(component) ? "no (DNP)" : "yes"});
        }

        if (!part.changes.empty()) result.changed.push_back(std::move(part));
    }

    for (const auto& component : left.components) {
        if (seen.find(keyOf(component)) == seen.end()) result.removed.push_back(component);
    }

    // Nets are only meaningful for boards.
    if (left.type == DocType::PcbDoc && right.type == DocType::PcbDoc) {
        std::set<std::string> leftNets;
        std::set<std::string> rightNets;
        for (const auto& net : left.netSummaries) leftNets.insert(net.name);
        for (const auto& net : right.netSummaries) rightNets.insert(net.name);
        for (const auto& name : rightNets) {
            if (leftNets.find(name) == leftNets.end()) result.netsAdded.push_back(name);
        }
        for (const auto& name : leftNets) {
            if (rightNets.find(name) == rightNets.end()) result.netsRemoved.push_back(name);
        }
    }

    auto byDesignator = [](const Component& a, const Component& b) {
        return text::naturalLess(a.designator, b.designator);
    };
    std::sort(result.added.begin(), result.added.end(), byDesignator);
    std::sort(result.removed.begin(), result.removed.end(), byDesignator);
    std::sort(result.changed.begin(), result.changed.end(),
              [](const ChangedPart& a, const ChangedPart& b) {
                  return text::naturalLess(a.designator, b.designator);
              });
    std::sort(result.netsAdded.begin(), result.netsAdded.end(), text::naturalLess);
    std::sort(result.netsRemoved.begin(), result.netsRemoved.end(), text::naturalLess);
    return result;
}

std::string compareToText(const CompareResult& result) {
    std::ostringstream out;
    out << result.leftName << "  →  " << result.rightName << "\n";
    out << "Components: " << result.leftCount << " → " << result.rightCount << "   +"
        << result.added.size() << "  -" << result.removed.size() << "  ~"
        << result.changed.size() << "\n";

    if (result.identical()) {
        out << "\nNo differences.\n";
        return out.str();
    }

    if (!result.added.empty()) {
        out << "\nAdded (" << result.added.size() << "):\n";
        for (const auto& component : result.added) {
            out << "  + " << component.designator << "  " << describe(component) << "  "
                << component.footprint << "\n";
        }
    }
    if (!result.removed.empty()) {
        out << "\nRemoved (" << result.removed.size() << "):\n";
        for (const auto& component : result.removed) {
            out << "  - " << component.designator << "  " << describe(component) << "  "
                << component.footprint << "\n";
        }
    }
    if (!result.changed.empty()) {
        out << "\nChanged (" << result.changed.size() << "):\n";
        for (const auto& part : result.changed) {
            out << "  ~ " << part.designator << "\n";
            for (const auto& change : part.changes) {
                out << "      " << change.field << ": " << change.before << " → "
                    << change.after << "\n";
            }
        }
    }
    if (!result.netsAdded.empty() || !result.netsRemoved.empty()) {
        out << "\nNets:";
        for (const auto& net : result.netsAdded) out << "  +" << net;
        for (const auto& net : result.netsRemoved) out << "  -" << net;
        out << "\n";
    }
    return out.str();
}

std::string compareToHtml(const CompareResult& result) {
    std::ostringstream out;
    out << "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>Comparison</title>\n<style>\n"
           "  body { background: #12161c; color: #dfe3e8; font-family: -apple-system, 'Segoe UI',"
           " Roboto, Arial, sans-serif; margin: 0; padding: 18px 22px; }\n"
           "  h2 { font-size: 17px; margin: 0 0 2px 0; color: #ffffff; }\n"
           "  .sub { color: #8a929e; font-size: 12.5px; margin-bottom: 14px; }\n"
           "  .badge { display: inline-block; border-radius: 9px; padding: 1px 9px; font-size: 12px;"
           " margin-right: 8px; }\n"
           "  .add { background: #17351f; color: #7ee2a8; }\n"
           "  .del { background: #3a1b1b; color: #ff9b9b; }\n"
           "  .mod { background: #33301a; color: #ffd479; }\n"
           "  h3 { font-size: 13px; text-transform: uppercase; letter-spacing: .06em; color: #8ab4f8;"
           " margin: 18px 0 6px 0; }\n"
           "  table { border-collapse: collapse; width: 100%; font-size: 12.5px; }\n"
           "  th, td { text-align: left; padding: 5px 8px; border-bottom: 1px solid #232a33; }\n"
           "  th { color: #9aa0a6; font-weight: 600; }\n"
           "  td.designator { font-family: ui-monospace, Consolas, monospace; color: #ffffff;"
           " white-space: nowrap; }\n"
           "  .before { color: #ff9b9b; text-decoration: line-through; }\n"
           "  .after { color: #7ee2a8; }\n"
           "  .ok { color: #7ee2a8; }\n"
           "</style></head>\n<body>\n";

    out << "<h2>" << htmlEscape(result.leftName) << " &nbsp;→&nbsp; "
        << htmlEscape(result.rightName) << "</h2>\n";
    out << "<div class=\"sub\">" << result.leftCount << " → " << result.rightCount
        << " components &nbsp;·&nbsp; <span class=\"badge add\">+" << result.added.size()
        << " added</span><span class=\"badge del\">-" << result.removed.size()
        << " removed</span><span class=\"badge mod\">~" << result.changed.size()
        << " changed</span></div>\n";

    if (result.identical()) {
        out << "<p class=\"ok\">No differences — the two documents describe the same parts.</p>\n";
        out << "</body></html>\n";
        return out.str();
    }

    if (!result.added.empty() || !result.removed.empty()) {
        out << "<h3>Designators</h3>\n<table>\n<tr><th>Designator</th><th>Value</th>"
               "<th>Footprint</th><th>Change</th></tr>\n";
        for (const auto& component : result.added) {
            out << "<tr><td class=\"designator\">" << htmlEscape(component.designator)
                << "</td><td>" << htmlEscape(describe(component)) << "</td><td>"
                << htmlEscape(component.footprint) << "</td><td class=\"after\">added</td></tr>\n";
        }
        for (const auto& component : result.removed) {
            out << "<tr><td class=\"designator\">" << htmlEscape(component.designator)
                << "</td><td>" << htmlEscape(describe(component)) << "</td><td>"
                << htmlEscape(component.footprint) << "</td><td class=\"before\">removed</td></tr>\n";
        }
        out << "</table>\n";
    }

    if (!result.changed.empty()) {
        out << "<h3>Changed parts</h3>\n<table>\n<tr><th>Designator</th><th>Field</th><th>Before</th>"
               "<th>After</th></tr>\n";
        for (const auto& part : result.changed) {
            bool first = true;
            for (const auto& change : part.changes) {
                out << "<tr>";
                if (first) {
                    out << "<td class=\"designator\" rowspan=\"" << part.changes.size() << "\">"
                        << htmlEscape(part.designator) << "</td>";
                    first = false;
                }
                out << "<td>" << htmlEscape(change.field) << "</td><td class=\"before\">"
                    << htmlEscape(change.before) << "</td><td class=\"after\">"
                    << htmlEscape(change.after) << "</td></tr>\n";
            }
        }
        out << "</table>\n";
    }

    if (!result.netsAdded.empty() || !result.netsRemoved.empty()) {
        out << "<h3>Nets</h3>\n<div style=\"font-size:12.5px;line-height:1.8\">";
        for (const auto& net : result.netsAdded) {
            out << "<span class=\"after\">+" << htmlEscape(net) << "</span>&nbsp;&nbsp;";
        }
        for (const auto& net : result.netsRemoved) {
            out << "<span class=\"before\">-" << htmlEscape(net) << "</span>&nbsp;&nbsp;";
        }
        out << "</div>\n";
    }

    out << "</body></html>\n";
    return out.str();
}

} // namespace pcbv
