// smoke.cpp — sanity checks for the Altium reader.
//
//   pcbv-smoke --self-test          run the internal checks (no files needed)
//   pcbv-smoke file.PcbDoc ...      parse real documents and verify invariants
//
// Exit code 0 = everything passed.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/AltiumParser.h"
#include "core/BoardRender.h"
#include "core/BoardIndex.h"
#include "core/Export.h"

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) {
        std::printf("  [ ok ] %s\n", what.c_str());
    } else {
        std::printf("  [FAIL] %s\n", what.c_str());
        ++g_failures;
    }
}

void selfTest() {
    std::printf("Internal checks\n");

    // Windows-1252 / UTF-8 decoding through a synthetic parameter list.
    // (The parser is only reachable through files, so this covers helpers indirectly.)

    // Type detection is case insensitive.
    check(pcbv::detectType("board.PcbDoc") == pcbv::DocType::PcbDoc, "detect .PcbDoc");
    check(pcbv::detectType("board.pcbdoc") == pcbv::DocType::PcbDoc, "detect lowercase .pcbdoc");
    check(pcbv::detectType("sheet.SchDoc") == pcbv::DocType::SchDoc, "detect .SchDoc");
    check(pcbv::detectType("sheet.schdoc") == pcbv::DocType::SchDoc, "detect lowercase .schdoc");
    check(pcbv::detectType("lib.PcbLib") == pcbv::DocType::PcbLib, "detect .PcbLib");
    check(pcbv::detectType("note.txt") == pcbv::DocType::Unknown, "reject unknown extension");

    // ASCII exports are recognised as Altium text containers.
    {
        const std::string pcbPath = "/tmp/pcbv-smoke-ascii.PcbDoc";
        const std::string pcbText =
            "|RECORD=Board|KIND=Protel_Advanced_PCB|VERSION=5.00|KIND0=0|VX0=0mil|VY0=0mil\r\n"
            "|RECORD=Net|ID=0|NAME=GND\r\n"
            "|RECORD=Component|ID=0|LAYER=TOP|X=100mil|Y=200mil|ROTATION=90|PATTERN=0402|"
            "SOURCEDESIGNATOR=R1\r\n"
            "|RECORD=Pad|COMPONENT=0|NET=0|LAYER=TOP|NAME=1|X=90mil|Y=200mil|XSIZE=20mil|"
            "YSIZE=20mil|HOLESIZE=0mil\r\n"
            "|RECORD=Track|NET=0|LAYER=TOP|X1=0mil|Y1=0mil|X2=100mil|Y2=0mil|WIDTH=10mil\r\n";
        if (FILE* f = std::fopen(pcbPath.c_str(), "wb")) {
            std::fwrite(pcbText.data(), 1, pcbText.size(), f);
            std::fclose(f);
        }
        const pcbv::ParseOutcome ascii = pcbv::parseFile(pcbPath);
        check(ascii.ok, "ASCII PCB export is parsed");
        check(ascii.document.container == pcbv::Container::AsciiText, "ASCII container detected");
        check(ascii.document.components.size() == 1, "ASCII PCB component found");
        if (!ascii.document.components.empty()) {
            const pcbv::Component& r1 = ascii.document.components.front();
            check(r1.designator == "R1", "ASCII designator read");
            check(r1.footprint == "0402", "ASCII footprint read");
            check(r1.pads.size() == 1 && r1.pads.front().net == "GND", "ASCII pad net resolved");
        }
        check(ascii.document.pcb.tracks == 1, "ASCII track counted");
        check(ascii.document.geometry.tracks.size() == 1, "ASCII track kept as geometry");
        check(ascii.document.geometry.pads.size() == 1, "ASCII pad kept as geometry");
        // The net/layer index that feeds the Nets and Layers panels.
        check(ascii.document.netSummaries.size() == 1, "net index has one net");
        if (!ascii.document.netSummaries.empty()) {
            const pcbv::NetSummary& gnd = ascii.document.netSummaries.front();
            check(gnd.name == "GND", "net index keeps the name");
            check(gnd.pads == 1 && gnd.tracks == 1, "net index counts pads and tracks");
            check(gnd.trackLength > 99.0 && gnd.trackLength < 101.0, "net index sums track length (mil)");
            check(!gnd.connections.empty() && gnd.connections.front() == "R1-1",
                  "net index labels pads as designator-pin");
        }
        check(!ascii.document.layerUsage.empty(), "layer usage is indexed");
        if (!ascii.document.layerUsage.empty()) {
            check(ascii.document.layerUsage.front().layer == 1, "top copper comes first");
            check(ascii.document.layerUsage.front().primitives == 2,
                  "layer usage counts the track and the pad");
        }
        check(pcbv::copperStackup(ascii.document) == "1 copper layer", "stackup describes one copper layer");

        const std::string svg = pcbv::toSvg(ascii.document);
        check(svg.find("<svg") != std::string::npos, "SVG drawing produced for an ASCII board");
        check(svg.find("<line") != std::string::npos, "SVG contains the track");
        std::remove(pcbPath.c_str());

        const std::string schPath = "/tmp/pcbv-smoke-ascii.SchDoc";
        const std::string schText =
            "|HEADER=Protel for Windows - Schematic Capture Ascii File Version 5.0|WEIGHT=3\r\n"
            "|RECORD=31|FONTIDCOUNT=1|SIZE1=10|FONTNAME1=Arial\r\n"
            "|RECORD=1|LIBREFERENCE=NE555|COMPONENTDESCRIPTION=Timer|\r\n"
            "|RECORD=34|OWNERINDEX=1|TEXT=U1\r\n"
            "|RECORD=2|OWNERINDEX=1|DESIGNATOR=1|NAME=GND|ELECTRICAL=7|\r\n"
            "|RECORD=41|OWNERINDEX=1|NAME=Part Number|TEXT=NE555P\r\n";
        if (FILE* f = std::fopen(schPath.c_str(), "wb")) {
            std::fwrite(schText.data(), 1, schText.size(), f);
            std::fclose(f);
        }
        const pcbv::ParseOutcome sch = pcbv::parseFile(schPath);
        check(sch.ok, "ASCII schematic export is parsed");
        check(sch.document.components.size() == 1, "ASCII schematic component found");
        if (!sch.document.components.empty()) {
            const pcbv::Component& u1 = sch.document.components.front();
            check(u1.designator == "U1", "ASCII schematic designator read");
            check(u1.pinCount == 1, "ASCII schematic pin read");
            check(u1.partNumber == "NE555P", "ASCII schematic parameter read");
        }
        std::remove(schPath.c_str());
    }

    // Parsing a non-existent file must fail cleanly instead of crashing.
    const pcbv::ParseOutcome missing = pcbv::parseFile("/definitely/not/here.PcbDoc");
    check(!missing.ok && !missing.error.empty(), "missing file returns an error");

    // A random text file is not a compound file.
    {
        const std::string path = "/tmp/pcbv-smoke-not-a-board.PcbDoc";
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            const char* text = "this is not an OLE2 container";
            std::fwrite(text, 1, std::strlen(text), f);
            std::fclose(f);
        }
        const pcbv::ParseOutcome bad = pcbv::parseFile(path);
        check(!bad.ok, "garbage file is rejected");
        check(!bad.error.empty(), "garbage file reports a reason");
        check(!bad.signature.empty(), "garbage file reports its signature");
        std::remove(path.c_str());

        // A ZIP archive renamed to .PcbDoc must produce a helpful message.
        const std::string zipPath = "/tmp/pcbv-smoke-archive.PcbDoc";
        if (FILE* f = std::fopen(zipPath.c_str(), "wb")) {
            const char* zip = "PK\x03\x04 not really a zip";
            std::fwrite(zip, 1, std::strlen(zip), f);
            std::fclose(f);
        }
        const pcbv::ParseOutcome zipped = pcbv::parseFile(zipPath);
        check(!zipped.ok && zipped.error.find("ZIP") != std::string::npos, "ZIP archive is diagnosed");
        std::remove(zipPath.c_str());
    }
}

void verifyDocument(const std::string& path) {
    std::printf("\nChecking %s\n", path.c_str());
    const pcbv::ParseOutcome outcome = pcbv::parseFile(path);
    if (!outcome.ok) {
        std::printf("  [FAIL] cannot parse: %s\n", outcome.error.c_str());
        ++g_failures;
        return;
    }
    const pcbv::Document& doc = outcome.document;
    check(doc.type != pcbv::DocType::Unknown, "document type detected");
    check(!doc.formatVersion.empty(), "format version present");
    if (doc.container == pcbv::Container::CompoundBinary) {
        check(!doc.structure.empty(), "OLE container has entries");
    } else {
        check(!doc.recordTypes.empty(), "ASCII record histogram present");
    }

    int withDesignator = 0;
    int withFootprint = 0;
    int withParameters = 0;
    for (const auto& c : doc.components) {
        if (!c.designator.empty()) ++withDesignator;
        if (!c.footprint.empty()) ++withFootprint;
        if (!c.properties.empty()) ++withParameters;
    }
    std::printf("  components: %zu (with designator: %d)\n", doc.components.size(), withDesignator);
    if (!doc.components.empty()) {
        check(withDesignator == static_cast<int>(doc.components.size()),
              "every component has a designator");
        std::printf("  components with footprint: %d, with parameters: %d\n", withFootprint,
                    withParameters);
        check(withParameters == static_cast<int>(doc.components.size()),
              "every component exposes its raw parameters");
    }

    if (doc.type == pcbv::DocType::PcbDoc) {
        int assignedPads = 0;
        for (const auto& c : doc.components) assignedPads += static_cast<int>(c.pads.size());
        std::printf("  pads: %d (assigned to components: %d), tracks: %d, vias: %d, nets: %zu\n",
                    doc.pcb.pads, assignedPads, doc.pcb.tracks, doc.pcb.vias, doc.nets.size());
        check(doc.pcb.pads >= assignedPads, "pad total covers the pads assigned to components");
        for (const auto& c : doc.components) {
            if (!c.pads.empty()) {
                check(!c.pads.front().layerName.empty(), "pad layer name resolved");
                break;
            }
        }
    } else {
        int pins = 0;
        for (const auto& c : doc.components) pins += c.pinCount;
        std::printf("  pins: %d, wires: %d\n", pins, doc.sch.wires);
    }

    // Exports must not be empty.
    check(!pcbv::toCsv(doc).empty(), "CSV export produced output");
    check(!pcbv::toJson(doc).empty(), "JSON export produced output");
    check(!pcbv::toHtml(doc).empty(), "HTML export produced output");

    if (doc.type == pcbv::DocType::PcbDoc) {
        // The board drawing must contain every primitive we parsed.
        std::printf("  geometry: %zu tracks, %zu pads, %zu vias, %zu arcs, %zu regions\n",
                    doc.geometry.tracks.size(), doc.geometry.pads.size(),
                    doc.geometry.vias.size(), doc.geometry.arcs.size(),
                    doc.geometry.regions.size());
        check(doc.geometry.tracks.size() == static_cast<std::size_t>(doc.pcb.tracks),
              "every track reaches the drawing");
        check(doc.geometry.pads.size() == static_cast<std::size_t>(doc.pcb.pads),
              "every pad reaches the drawing");
        check(doc.geometry.vias.size() == static_cast<std::size_t>(doc.pcb.vias),
              "every via reaches the drawing");
        const pcbv::BoardRenderList render = pcbv::buildBoardRender(doc, pcbv::BoardRenderOptions());
        check(!render.empty(), "board display list is not empty");
        check(render.width > 0.0 && render.height > 0.0, "display list has a size");
        const std::string svg = pcbv::toSvg(doc);
        check(svg.find("<svg") != std::string::npos && svg.size() > 200,
              "SVG export produced a drawing");

        // --- net and layer index ------------------------------------------------
        check(!doc.netSummaries.empty(), "net index is built");
        check(!doc.layerUsage.empty(), "layer usage is built");
        std::size_t padsWithNet = 0;
        for (const auto& pad : doc.geometry.pads) {
            if (!pad.net.empty()) ++padsWithNet;
        }
        std::size_t indexedPads = 0;
        for (const auto& net : doc.netSummaries) {
            indexedPads += static_cast<std::size_t>(net.pads);
        }
        check(indexedPads == padsWithNet, "every pad with a net is indexed once");
        if (!doc.netSummaries.empty()) {
            std::printf("  nets: %zu, layers: %zu, stackup: %s\n", doc.netSummaries.size(),
                        doc.layerUsage.size(), pcbv::copperStackup(doc).c_str());
        }
    } else {
        check(pcbv::toSvg(doc).empty(), "schematic has no board drawing");
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc <= 1) {
        std::printf("usage: pcbv-smoke --self-test | <files...>\n");
        return 1;
    }
    if (std::strcmp(argv[1], "--self-test") == 0) {
        selfTest();
        std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASSED" : "FAILED", g_failures,
                    g_failures == 1 ? "" : "s");
        return g_failures == 0 ? 0 : 1;
    }
    for (int i = 1; i < argc; ++i) verifyDocument(argv[i]);
    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASSED" : "FAILED", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
