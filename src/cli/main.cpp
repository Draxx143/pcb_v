// pcbv-cli — command line front end for the Altium document reader.
//
//   pcbv-cli --list    board.PcbDoc            component table in the terminal
//   pcbv-cli --summary sheet.SchDoc            document statistics
//   pcbv-cli --csv  out.csv  board.PcbDoc      export the BOM
//   pcbv-cli --json out.json board.PcbDoc      export JSON
//   pcbv-cli --html report.html board.PcbDoc   export an HTML report
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "core/AltiumParser.h"
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
        "  pcbv-cli --details <designator> <file>   print every parameter of one part\n"
        "\n"
        "Options:\n"
        "  -h, --help      show this help\n"
        "\n"
        "Notes:\n"
        "  * Coordinates are reported in mil and mm (1 mil = 0.0254 mm).\n"
        "  * .PcbLib / .SchLib support is planned for a later step.\n";
}

std::string padRight(const std::string& text, std::size_t width) {
    if (text.size() >= width) return text;
    return text + std::string(width - text.size(), ' ');
}

std::string shorten(const std::string& text, std::size_t width) {
    if (text.size() <= width) return text;
    if (width <= 1) return text.substr(0, width);
    return text.substr(0, width - 1) + "\xE2\x80\xA6"; // … (UTF-8)
}

void printTable(const pcbv::Document& doc) {
    const bool pcb = doc.type == pcbv::DocType::PcbDoc;
    std::cout << doc.fileName << "  —  " << pcbv::docTypeName(doc.type) << "  —  "
              << doc.components.size() << " components\n\n";

    std::cout << padRight("#", 4) << padRight("DESIGNATOR", 14) << padRight("COMMENT", 22)
              << padRight("FOOTPRINT", 26) << padRight("MANUFACTURER", 18)
              << padRight(pcb ? "PADS" : "PINS", 6) << "PART NUMBER\n";
    std::cout << std::string(110, '-') << "\n";

    int index = 1;
    for (const auto& c : doc.components) {
        std::cout << padRight(std::to_string(index++), 4) << padRight(shorten(c.designator, 13), 14)
                  << padRight(shorten(c.comment, 21), 22) << padRight(shorten(c.footprint, 25), 26)
                  << padRight(shorten(c.manufacturer, 17), 18)
                  << padRight(std::to_string(pcb ? c.padCount : c.pinCount), 6)
                  << shorten(c.partNumber, 40) << "\n";
    }
    std::cout << "\n" << pcbv::toSummaryText(doc);
}

int fail(const std::string& message) {
    std::cerr << "error: " << message << "\n";
    return 1;
}

bool writeFile(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return static_cast<bool>(out);
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

    if (mode == "--list" || mode == "--summary") {
        if (args.size() < 2) return fail("missing input file");
        inputPath = args[1];
    } else if (mode == "--csv" || mode == "--json" || mode == "--html") {
        if (args.size() < 3) return fail("usage: " + mode + " <output> <input>");
        outputPath = args[1];
        inputPath = args[2];
    } else if (mode == "--details") {
        if (args.size() < 3) return fail("usage: --details <designator> <file>");
        detailDesignator = args[1];
        inputPath = args[2];
    } else {
        // Convenience: `pcbv-cli file.PcbDoc` behaves like --list.
        inputPath = mode;
    }

    const pcbv::ParseOutcome result = pcbv::parseFile(inputPath);
    if (!result.ok) return fail(result.error);
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
