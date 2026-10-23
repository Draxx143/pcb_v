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
        std::remove(path.c_str());
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
    check(!doc.structure.empty(), "OLE container has entries");

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
