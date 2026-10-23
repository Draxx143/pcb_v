// pcbv-cli — command line front end for the Altium document reader.
//
//   pcbv-cli --list    board.PcbDoc            component table in the terminal
//   pcbv-cli --summary sheet.SchDoc            document statistics
//   pcbv-cli --csv  out.csv  board.PcbDoc      export the BOM
//   pcbv-cli --json out.json board.PcbDoc      export JSON
//   pcbv-cli --html report.html board.PcbDoc   export an HTML report
//   pcbv-cli --svg  board.svg  board.PcbDoc    export the PCB drawing as SVG
//   pcbv-cli --nets  board.PcbDoc             net table + copper stackup
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "core/AltiumParser.h"
#include "core/BoardIndex.h"
#include "core/Export.h"

namespace {

void printUsage() {
    std::cout <<
        "pcbv-cli — Altium .PcbDoc / .SchDoc reader (step 1: component extraction)\n"
        "\n"
        "Usage:\n"
        "  pcbv-cli --list    <file.PcbDoc|file.SchDoc>\n"
        "  pcbv-cli --summary <file>\n"
        "  pcbv-cli --csv  <out.csv>  <file>\n"
        "  pcbv-cli --json <out.json> <file>\n"
        "  pcbv-cli --html <out.html> <file>\n"
        "  pcbv-cli --svg  <out.svg>  <file.PcbDoc>  board drawing (2D, scalable)\n"
        "  pcbv-cli --nets  <file.PcbDoc>            net table, track lengths and the stackup\n"
        "  pcbv-cli --details <designator> <file>   print every parameter of one part\n"
        "  pcbv-cli --inspect <file>                show how the file was detected/decoded\n"
        "\n"
        "Options:\n"
        "  -h, --help      show this help\n"
        "\n"
        "Notes:\n"
        "  * Coordinates are reported in mil and mm (1 mil = 0.0254 mm).\n"
        "  * .PcbLib / .SchLib support is planned for a later step.\n";
}

std::size_t utf8Length(const std::string& text) {
    std::size_t count = 0;
    for (unsigned char c : text) {
        if ((c & 0xC0) != 0x80) ++count; // count non-continuation bytes
    }
    return count;
}

/// Appends spaces until the text occupies `width` terminal columns (UTF-8 aware).
std::string padRight(const std::string& text, std::size_t width) {
    const std::size_t length = utf8Length(text);
    if (length >= width) return text;
    return text + std::string(width - length, ' ');
}

/// Truncates to `width` characters (never splitting a UTF-8 sequence) and adds an ellipsis.
std::string shorten(const std::string& text, std::size_t width) {
    const std::size_t length = utf8Length(text);
    if (length <= width) return text;
    if (width <= 1) width = 2;
    std::size_t kept = 0;
    std::size_t bytes = 0;
    while (bytes < text.size() && kept < width - 1) {
        ++bytes;
        while (bytes < text.size() && (static_cast<unsigned char>(text[bytes]) & 0xC0) == 0x80) {
            ++bytes;
        }
        ++kept;
    }
    return text.substr(0, bytes) + "\xE2\x80\xA6"; // …
}

void printTable(const pcbv::Document& doc) {
    const bool pcb = doc.type == pcbv::DocType::PcbDoc;
    std::cout << doc.fileName << "  —  " << pcbv::docTypeName(doc.type) << "  —  "
              << doc.components.size() << " components\n\n";

    auto cell = [](const std::string& text, std::size_t width) {
        return padRight(text, width) + " ";
    };

    std::cout << cell("#", 4) << cell("DESIGNATOR", 14) << cell("COMMENT", 22)
              << cell("FOOTPRINT", 26) << cell("MANUFACTURER", 18)
              << cell(pcb ? "PADS" : "PINS", 6) << "PART NUMBER\n";
    std::cout << std::string(112, '-') << "\n";

    int index = 1;
    for (const auto& c : doc.components) {
        std::cout << cell(std::to_string(index++), 4) << cell(shorten(c.designator, 13), 14)
                  << cell(shorten(c.comment, 21), 22) << cell(shorten(c.footprint, 25), 26)
                  << cell(shorten(c.manufacturer, 17), 18)
                  << cell(std::to_string(pcb ? c.padCount : c.pinCount), 6)
                  << shorten(c.partNumber, 40) << "\n";
    }
    std::cout << "\n" << pcbv::toSummaryText(doc);
}

int fail(const std::string& message) {
    std::cerr << "error: " << message << "\n";
    return 1;
}

void printInspection(const pcbv::ParseOutcome& outcome) {
    const pcbv::Document& doc = outcome.document;
    std::cout << "file        : " << doc.fileName << "\n";
    std::cout << "size        : " << doc.fileSize << " bytes\n";
    std::cout << "extension   : " << pcbv::docTypeName(pcbv::detectType(doc.path)) << "\n";
    std::cout << "detected    : " << outcome.detected << "\n";
    std::cout << "signature   : " << outcome.signature << "\n";
    std::cout << "container   : " << pcbv::containerName(doc.container) << "\n";
    std::cout << "format      : " << doc.formatVersion << "\n";
    if (!doc.boardVersion.empty()) std::cout << "board fmt   : " << doc.boardVersion << "\n";
    if (!doc.lastSaved.empty()) std::cout << "last saved  : " << doc.lastSaved << "\n";
    std::cout << "parsed ok   : " << (outcome.ok ? "yes" : "no") << "\n";
    if (!outcome.error.empty()) std::cout << "error       : " << outcome.error << "\n";

    if (!doc.structure.empty()) {
        std::cout << "\nOLE2 streams (" << doc.structure.size() << " root entries):\n";
        for (const auto& entry : doc.structure) {
            std::cout << "  " << (entry.isStorage ? "[dir] " : "[str] ") << entry.name;
            if (!entry.isStorage) std::cout << "  " << entry.size << " bytes";
            std::cout << "\n";
        }
    }
    if (!doc.recordTypes.empty()) {
        std::cout << "\nASCII record types:\n";
        for (const auto& entry : doc.recordTypes) {
            std::cout << "  " << padRight(entry.first, 16) << entry.second << "\n";
        }
    }
    if (outcome.ok) {
        std::cout << "\ncomponents  : " << doc.components.size() << "\n";
        for (const auto& warning : doc.warnings) std::cout << "warning     : " << warning << "\n";
    }
}

bool writeFile(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return static_cast<bool>(out);
}

/// Pads a cell to `width` columns with a two space gap.
std::string cellText(const std::string& text, std::size_t width) {
    return padRight(shorten(text, width - 2), width);
}

/// Net table plus the copper stackup: what the GUI shows in its left panels.
void printNets(const pcbv::Document& doc) {
    if (doc.netSummaries.empty()) {
        std::cout << "no nets in this document\n";
        return;
    }
    std::cout << doc.fileName << "  —  " << doc.netSummaries.size() << " nets\n\n";
    std::cout << cellText("#", 4) << cellText("NET", 24) << cellText("PADS", 6)
              << cellText("TRACKS", 8) << cellText("VIAS", 6) << cellText("LENGTH", 12)
              << "CONNECTIONS\n";
    std::cout << std::string(110, '-') << "\n";
    int index = 1;
    for (const auto& net : doc.netSummaries) {
        std::string connections;
        for (std::size_t i = 0; i < net.connections.size() && i < 6; ++i) {
            if (i > 0) connections += ", ";
            connections += net.connections[i];
        }
        if (net.connections.size() > 6) connections += ", …";
        std::cout << cellText(std::to_string(index++), 4) << cellText(net.name, 24)
                  << cellText(std::to_string(net.pads), 6) << cellText(std::to_string(net.tracks), 8)
                  << cellText(std::to_string(net.vias), 6)
                  << cellText(pcbv::formatMm(net.trackLength), 12) << connections << "\n";
    }

    const std::string stackup = pcbv::copperStackup(doc);
    if (!stackup.empty()) {
        std::cout << "\nCopper stackup: " << stackup << "\n";
    }
    std::cout << "Layers in use (" << doc.layerUsage.size() << "):\n";
    for (const auto& usage : doc.layerUsage) {
        std::cout << "  " << padRight(usage.name, 20) << padRight(std::to_string(usage.layer), 5)
                  << (usage.copper ? "copper" : "other") << "  " << usage.primitives << " items\n";
    }
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "-h" || args[0] == "--help") {
        printUsage();
        return args.empty() ? 1 : 0;
    }

    const std::string mode = args[0];

    std::string outputPath;
    std::string inputPath;
    std::string detailDesignator;

    if (mode == "--list" || mode == "--summary" || mode == "--nets") {
        if (args.size() < 2) return fail("missing input file");
        inputPath = args[1];
    } else if (mode == "--csv" || mode == "--json" || mode == "--html" || mode == "--svg") {
        if (args.size() < 3) return fail("usage: " + mode + " <output> <input>");
        outputPath = args[1];
        inputPath = args[2];
    } else if (mode == "--inspect") {
        if (args.size() < 2) return fail("usage: --inspect <file>");
        inputPath = args[1];
    } else if (mode == "--details") {
        if (args.size() < 3) return fail("usage: --details <designator> <file>");
        detailDesignator = args[1];
        inputPath = args[2];
    } else {
        // Convenience: `pcbv-cli file.PcbDoc` behaves like --list.
        inputPath = mode;
    }

    const pcbv::ParseOutcome result = pcbv::parseFile(inputPath);
    if (mode == "--inspect") {
        printInspection(result);
        return result.ok ? 0 : 1;
    }
    if (!result.ok) {
        std::cerr << "error: " << result.error << "\n";
        if (!result.detected.empty())
            std::cerr << "       detected: " << result.detected << "\n";
        if (!result.signature.empty())
            std::cerr << "       first bytes: " << result.signature << "\n";
        return 1;
    }
    const pcbv::Document& doc = result.document;

    if (mode == "--summary") {
        std::cout << pcbv::toSummaryText(doc);
        return 0;
    }
    if (mode == "--csv") {
        if (!writeFile(outputPath, pcbv::toCsv(doc))) return fail("cannot write " + outputPath);
        std::cout << "wrote " << outputPath << " (" << doc.components.size() << " components)\n";
        return 0;
    }
    if (mode == "--json") {
        if (!writeFile(outputPath, pcbv::toJson(doc))) return fail("cannot write " + outputPath);
        std::cout << "wrote " << outputPath << "\n";
        return 0;
    }
    if (mode == "--html") {
        if (!writeFile(outputPath, pcbv::toHtml(doc))) return fail("cannot write " + outputPath);
        std::cout << "wrote " << outputPath << "\n";
        return 0;
    }
    if (mode == "--svg") {
        pcbv::BoardRenderOptions renderOptions;
        renderOptions.hiddenLayers = pcbv::defaultHiddenLayers();  // same defaults as the GUI
        const std::string svg = pcbv::toSvg(doc, renderOptions);
        if (svg.empty()) {
            std::cerr << "error: no PCB geometry in this document (is it a schematic?)\n";
            return 1;
        }
        if (!writeFile(outputPath, svg)) return fail("cannot write " + outputPath);
        std::cout << "wrote " << outputPath << " (" << doc.geometry.tracks.size() << " tracks, "
                  << doc.geometry.pads.size() << " pads, " << doc.geometry.vias.size()
                  << " vias)\n";
        return 0;
    }
    if (mode == "--nets") {
        printNets(doc);
        return 0;
    }
    if (mode == "--details") {
        std::string needle = detailDesignator;
        std::transform(needle.begin(), needle.end(), needle.begin(), ::toupper);
        for (const auto& c : doc.components) {
            std::string designator = c.designator;
            std::transform(designator.begin(), designator.end(), designator.begin(), ::toupper);
            if (designator != needle) continue;
            std::cout << "Designator : " << c.designator << "\n"
                      << "Comment    : " << c.comment << "\n"
                      << "Name       : " << c.name << "\n"
                      << "Footprint  : " << c.footprint << "\n"
                      << "Description: " << c.description << "\n"
                      << "Layer      : " << c.layer << "\n"
                      << "Position   : " << pcbv::formatMm(c.x) << ", " << pcbv::formatMm(c.y)
                      << " mm  (" << pcbv::formatMil(c.x) << ", " << pcbv::formatMil(c.y) << " mil)\n"
                      << "Rotation   : " << c.rotation << " deg\n"
                      << "Pins/Pads  : " << c.pinCount << " / " << c.padCount << "\n";
            std::cout << "\nParameters:\n";
            for (const auto& p : c.properties) {
                std::cout << "  " << padRight(p.name, 32) << p.value << "\n";
            }
            if (!c.pins.empty()) {
                std::cout << "\nPins:\n";
                for (const auto& pin : c.pins) {
                    std::cout << "  " << padRight(pin.designator, 6) << padRight(pin.name, 24)
                              << pin.electrical << "\n";
                }
            }
            if (!c.pads.empty()) {
                std::cout << "\nPads:\n";
                for (const auto& pad : c.pads) {
                    std::cout << "  " << padRight(pad.designator, 8) << padRight(pad.layerName, 16)
                              << padRight(pad.net, 24) << pcbv::formatMm(pad.width) << " x "
                              << pcbv::formatMm(pad.height) << " mm";
                    if (pad.holeSize > 0) std::cout << "  hole " << pcbv::formatMm(pad.holeSize) << " mm";
                    std::cout << "\n";
                }
            }
            return 0;
        }
        return fail("component not found: " + detailDesignator);
    }

    printTable(doc);
    return 0;
}
