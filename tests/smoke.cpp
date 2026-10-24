// smoke.cpp — sanity checks for the Altium reader.
//
//   pcbv-smoke --self-test          run the internal checks (no files needed)
//   pcbv-smoke file.PcbDoc ...      parse real documents and verify invariants
//
// Exit code 0 = everything passed.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/AltiumParser.h"
#include "core/BoardRender.h"
#include "core/BoardIndex.h"
#include "core/Bom.h"
#include "core/Compare.h"
#include "core/Netlist.h"
#include "core/Recent.h"
#include "core/Library.h"
#include "core/Search.h"
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
        // --- netlist / connectivity ---------------------------------------------
        const std::vector<pcbv::NetConnection> netlist =
            pcbv::buildNetlist(ascii.document);
        check(netlist.size() == 1, "netlist groups the pads by net");
        if (!netlist.empty()) {
            check(netlist.front().name == "GND", "netlist keeps the net name");
            check(netlist.front().nodes.size() == 1, "netlist lists the connected pad");
            check(netlist.front().nodes.front().label == "R1-1", "netlist labels designator-pad");
        }
        check(pcbv::danglingNetCount(netlist) == 1, "a single-pad net is reported as dangling");
        check(pcbv::netlistToCsv(ascii.document).find("GND,R1,1,R1-1") != std::string::npos,
              "netlist CSV carries net, part, pad and label");
        check(pcbv::netlistToText(ascii.document).find("GND") != std::string::npos,
              "netlist text lists the net");

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
        // Several files can be open at once (one tab each): a document has to behave as an
        // independent value, otherwise switching tabs would share (and corrupt) state.
        {
            pcbv::Document copy = ascii.document;
            copy.components.clear();
            copy.geometry.tracks.clear();
            check(ascii.document.components.size() == 1 && ascii.document.geometry.tracks.size() == 1,
                  "each open tab owns an independent document copy");
        }
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

    // --- recent files list --------------------------------------------------
    {
        auto list = pcbv::pushRecent({}, "/a.PcbDoc");
        check(list.size() == 1 && list.front() == "/a.PcbDoc", "recent: the first file is remembered");
        list = pcbv::pushRecent(list, "/b.PcbDoc");
        check(list.size() == 2 && list[0] == "/b.PcbDoc", "recent: the newest file goes first");
        list = pcbv::pushRecent(list, "/a.PcbDoc");
        check(list.size() == 2 && list[0] == "/a.PcbDoc" && list[1] == "/b.PcbDoc",
              "recent: reopening a file moves it up instead of duplicating it");
        check(pcbv::pushRecent(list, "").size() == 2, "recent: an empty path is ignored");
        std::vector<std::string> longList;
        for (int i = 0; i < 14; ++i) longList.push_back("/f" + std::to_string(i) + ".PcbDoc");
        const auto trimmed = pcbv::pushRecent(longList, "/new.PcbDoc", 10);
        check(trimmed.size() == 10 && trimmed.front() == "/new.PcbDoc",
              "recent: the list never grows past ten entries");
    }

    // --- libraries (.PcbLib / .SchLib) --------------------------------------
    {
        const std::string libPath = "/tmp/pcbv-smoke-footprints.PcbLib";
        const std::string libText =
            "|RECORD=Library|FILENAME=test.PcbLib|KIND=Protel_Advanced_PCB_Library\r\n"
            "|RECORD=Component|ID=0|PATTERN=SOIC-8|HEIGHT=60mil|DESCRIPTION=8-pin SOIC\r\n"
            "|RECORD=Pad|COMPONENT=0|NAME=1|LAYER=TOP|X=0mil|Y=0mil|XSIZE=20mil|YSIZE=10mil|"
            "HOLESIZE=0mil\r\n"
            "|RECORD=Pad|COMPONENT=0|NAME=2|LAYER=TOP|X=50mil|Y=0mil|XSIZE=20mil|YSIZE=10mil|"
            "HOLESIZE=0mil\r\n"
            "|RECORD=Track|COMPONENT=0|LAYER=TOP|X1=-20mil|Y1=5mil|X2=70mil|Y2=5mil|WIDTH=6mil\r\n"
            "|RECORD=Text|COMPONENT=0|LAYER=TOPOVERLAY|X=10mil|Y=20mil|TEXT=SOIC-8\r\n";
        if (FILE* f = std::fopen(libPath.c_str(), "wb")) {
            std::fwrite(libText.data(), 1, libText.size(), f);
            std::fclose(f);
        }
        const pcbv::ParseOutcome lib = pcbv::parseFile(libPath);
        check(lib.ok, "ASCII footprint library is parsed");
        check(lib.library.type == pcbv::DocType::PcbLib, "ASCII library keeps the PcbLib type");
        check(lib.library.footprints.size() == 1, "one footprint is listed");
        if (!lib.library.footprints.empty()) {
            const pcbv::LibraryFootprint& footprint = lib.library.footprints.front();
            check(footprint.name == "SOIC-8", "footprint keeps its pattern name");
            check(footprint.description == "8-pin SOIC", "footprint description is read");
            check(footprint.padCount == 2, "footprint pads are counted");
            check(footprint.trackCount == 1, "footprint tracks are counted");
            check(footprint.geometry.pads.size() == 2 &&
                      footprint.geometry.pads[1].x > 49.0 && footprint.geometry.pads[1].x < 51.0,
                  "footprint pad positions are read in mil");
            check(footprint.bounds.valid && footprint.bounds.width() > 84.0,
                  "footprint extents are computed");
        }
        check(lib.document.components.size() == 1, "a library becomes one component row per item");
        check(pcbv::libraryToCsv(lib.library).find("Footprint,SOIC-8") != std::string::npos,
              "library CSV lists the footprint");
        std::remove(libPath.c_str());

        const std::string schLibPath = "/tmp/pcbv-smoke-symbols.SchLib";
        const std::string schLibText =
            "|HEADER=Protel for Windows - Schematic Capture Ascii File Version 5.0|WEIGHT=3\r\n"
            "|RECORD=1|LIBREFERENCE=NE555|COMPONENTDESCRIPTION=Timer\r\n"
            "|RECORD=34|OWNERINDEX=0|TEXT=U?\r\n"
            "|RECORD=2|OWNERINDEX=0|DESIGNATOR=1|NAME=GND|ELECTRICAL=7\r\n"
            "|RECORD=2|OWNERINDEX=0|DESIGNATOR=2|NAME=TRIG|ELECTRICAL=0\r\n"
            "|RECORD=41|OWNERINDEX=0|NAME=Part Number|TEXT=NE555P\r\n";
        if (FILE* f = std::fopen(schLibPath.c_str(), "wb")) {
            std::fwrite(schLibText.data(), 1, schLibText.size(), f);
            std::fclose(f);
        }
        const pcbv::ParseOutcome schLib = pcbv::parseFile(schLibPath);
        check(schLib.ok, "ASCII symbol library is parsed");
        check(schLib.library.symbols.size() == 1, "one symbol is listed");
        if (!schLib.library.symbols.empty()) {
            const pcbv::LibrarySymbol& symbol = schLib.library.symbols.front();
            check(symbol.name == "NE555", "symbol keeps its library reference");
            check(symbol.pins.size() == 2, "symbol pins are read");
            check(symbol.description == "Timer", "symbol description is read");
            bool hasPartNumber = false;
            for (const pcbv::Property& property : symbol.parameters)
                if (property.name == "Part Number" && property.value == "NE555P") hasPartNumber = true;
            check(hasPartNumber, "symbol parameters are kept");
        }
        // Symbols are drawn: pins with geometry and the symbol's own graphics.
        const std::string drawPath = "/tmp/pcbv-smoke-draw.SchLib";
        const std::string drawText =
            "|HEADER=Protel for Windows - Schematic Capture Ascii File Version 5.0|WEIGHT=3\r\n"
            "|RECORD=1|LIBREFERENCE=OPAMP|COMPONENTDESCRIPTION=Op-amp\r\n"
            "|RECORD=14|OWNERINDEX=0|LOCATION.X=-100|LOCATION.Y=-100|CORNER.X=100|CORNER.Y=100|"
            "FILLED=T\r\n"
            // PINCONGLOMERATE: bits 0-1 orientation (1 = left), 0x08 = show name,
            // 0x10 = show designator.
            "|RECORD=2|OWNERINDEX=0|DESIGNATOR=1|NAME=+IN|ELECTRICAL=0|LOCATION.X=-200|LOCATION.Y=-50|"
            "PINLENGTH=100|PINCONGLOMERATE=26\r\n"
            "|RECORD=2|OWNERINDEX=0|DESIGNATOR=2|NAME=-IN|ELECTRICAL=0|LOCATION.X=-200|LOCATION.Y=50|"
            "PINLENGTH=100|PINCONGLOMERATE=26\r\n"
            "|RECORD=2|OWNERINDEX=0|DESIGNATOR=3|NAME=OUT|ELECTRICAL=2|LOCATION.X=200|LOCATION.Y=0|"
            "PINLENGTH=100|PINCONGLOMERATE=24\r\n";
        if (FILE* f = std::fopen(drawPath.c_str(), "wb")) {
            std::fwrite(drawText.data(), 1, drawText.size(), f);
            std::fclose(f);
        }
        const pcbv::ParseOutcome drawLib = pcbv::parseFile(drawPath);
        check(drawLib.ok && drawLib.library.symbols.size() == 1, "drawing fixture parses");
        if (drawLib.ok && !drawLib.library.symbols.empty()) {
            const pcbv::LibrarySymbol& symbol = drawLib.library.symbols.front();
            check(symbol.shapes.size() == 1, "symbol graphics are read");
            check(symbol.pinGeometry.size() == 3, "pin geometry is read");
            if (symbol.pinGeometry.size() == 3) {
                check(symbol.pinGeometry[0].x < -1900.0 && symbol.pinGeometry[0].y > -600.0,
                      "pin positions are converted from 10 mil units");
                check(symbol.pinGeometry[0].length > 990.0 && symbol.pinGeometry[0].length < 1010.0,
                      "pin length is converted");
                check(symbol.pinGeometry[0].rotation == 180 && symbol.pinGeometry[2].rotation == 0 &&
                          symbol.pinGeometry[2].showDesignator,
                      "pin orientation and flags are decoded from PINCONGLOMERATE");
            }
            const pcbv::Document drawn = pcbv::symbolDocument(symbol, "OPAMP");
            check(!drawn.geometry.tracks.empty(), "symbol drawing produces strokes");
            check(!drawn.geometry.fills.empty(), "a filled symbol body becomes a filled rectangle");
            check(drawn.bounds.valid && drawn.bounds.width() > 5000.0,
                  "symbol drawing has real extents");
            check(pcbv::toSvg(drawn).find("<svg") != std::string::npos,
                  "a symbol can be exported as SVG");

            // Pin names are anchored at the body edge (1 = left at the left edge, 2 = right
            // at the right edge) so the board view and the SVG export draw them inwards.
            bool leftAnchored = false;
            bool rightAnchored = false;
            for (const pcbv::TextFeature& label : drawn.geometry.texts) {
                if (label.text == "+IN" || label.text == "-IN") leftAnchored = label.anchor == 1;
                if (label.text == "OUT") rightAnchored = label.anchor == 2;
            }
            check(leftAnchored && rightAnchored,
                  "pin names carry their text alignment");
            pcbv::BoardRenderOptions options;
            options.hiddenLayers = pcbv::defaultHiddenLayers();
            const pcbv::BoardRenderList rendered = pcbv::buildBoardRender(drawn, options);
            bool anchorReachedRenderer = false;
            for (const pcbv::RenderPrimitive& primitive : rendered.primitives) {
                if (primitive.text == "OUT" && primitive.textAnchor == 2) anchorReachedRenderer = true;
            }
            check(anchorReachedRenderer, "text alignment reaches the renderer");
        }

        // A compound (multi-part) symbol keeps every part in one coordinate space; the parts
        // must not be stacked on top of each other, and Altium's "\x" escapes decode.
        const std::string multiPath = "/tmp/pcbv-smoke-multi.SchLib";
        const std::string multiText =
            "|HEADER=Protel for Windows - Schematic Capture Ascii File Version 5.0|WEIGHT=3\r\n"
            "|RECORD=1|LIBREFERENCE=MULTI|PARTCOUNT=3|COMPONENTDESCRIPTION=Dual op-amp\r\n"
            "|RECORD=14|OWNERINDEX=0|OWNERPARTID=1|LOCATION.X=-100|LOCATION.Y=-100|"
            "CORNER.X=100|CORNER.Y=100|FILLED=T\r\n"
            "|RECORD=14|OWNERINDEX=0|OWNERPARTID=2|LOCATION.X=-100|LOCATION.Y=-100|"
            "CORNER.X=100|CORNER.Y=100|FILLED=T\r\n"
            "|RECORD=2|OWNERINDEX=0|OWNERPARTID=1|DESIGNATOR=1|NAME=S\\P\\I\\2\\_\\N\\S\\S|"
            "ELECTRICAL=0|LOCATION.X=-200|LOCATION.Y=0|PINLENGTH=100|PINCONGLOMERATE=18\r\n"
            "|RECORD=2|OWNERINDEX=0|OWNERPARTID=2|DESIGNATOR=2|NAME=OUT|ELECTRICAL=2|"
            "LOCATION.X=200|LOCATION.Y=0|PINLENGTH=100|PINCONGLOMERATE=16\r\n";
        if (FILE* f = std::fopen(multiPath.c_str(), "wb")) {
            std::fwrite(multiText.data(), 1, multiText.size(), f);
            std::fclose(f);
        }
        const pcbv::ParseOutcome multi = pcbv::parseFile(multiPath);
        check(multi.ok && multi.library.symbols.size() == 1, "compound symbol fixture parses");
        if (multi.ok && !multi.library.symbols.empty()) {
            const pcbv::LibrarySymbol& symbol = multi.library.symbols.front();
            check(symbol.partCount == 2, "sub-part count is read");
            check(symbol.shapes.size() == 2 && symbol.shapes[0].ownerPart == 1 &&
                      symbol.shapes[1].ownerPart == 2,
                  "each body rectangle knows its part");
            check(!symbol.pinGeometry.empty() && symbol.pinGeometry[0].name == "SPI2_NSS",
                  "escaped pin names are decoded");
            const pcbv::Document drawn = pcbv::symbolDocument(symbol, "MULTI");
            check(drawn.geometry.fills.size() == 2, "both parts are drawn");
            if (drawn.geometry.fills.size() == 2) {
                const pcbv::FillFeature& first = drawn.geometry.fills[0];
                const pcbv::FillFeature& second = drawn.geometry.fills[1];
                const double firstMax = std::max(first.x1, first.x2);
                const double secondMin = std::min(second.x1, second.x2);
                check(secondMin - firstMax > 100.0,
                      "multi-part symbols are laid out side by side");
            }
            bool captioned = false;
            for (const pcbv::TextFeature& label : drawn.geometry.texts) {
                if (label.text == "Part 2 / 2") captioned = true;
            }
            check(captioned, "each part of a compound symbol is labelled");
        }
        std::remove(multiPath.c_str());
        std::remove(drawPath.c_str());
        std::remove(schLibPath.c_str());
    }

    // --- PcbLib pad records -------------------------------------------------
    // A pad record always ends with a per-layer sub-record. Even an empty one costs four
    // bytes (its zero size prefix); skipping it makes the reader eat the first byte of the
    // next record, which used to lose every pad after the first one.
    {
        std::vector<std::uint8_t> stream;
        auto putU32 = [&stream](std::uint32_t value) {
            for (int i = 0; i < 4; ++i) stream.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
        };
        auto putString = [&stream, &putU32](const std::string& value) {
            putU32(static_cast<std::uint32_t>(value.size() + 1));
            stream.push_back(static_cast<std::uint8_t>(value.size()));
            stream.insert(stream.end(), value.begin(), value.end());
        };
        auto putPad = [&](const std::string& name, double xMil, double yMil) {
            stream.push_back(2);  // PcbPrimitiveObjectId::Pad
            putString(name);
            putString("");
            putString("");
            putU32(1);            // padding block
            stream.push_back(0);
            std::vector<std::uint8_t> block(64, 0);
            block[0] = 1;         // TOP layer
            const auto put = [&block](std::size_t offset, std::int32_t value) {
                const std::uint32_t raw = static_cast<std::uint32_t>(value);
                for (int i = 0; i < 4; ++i) block[offset + i] = static_cast<std::uint8_t>((raw >> (8 * i)) & 0xFF);
            };
            put(13, static_cast<std::int32_t>(xMil * 10000.0));
            put(17, static_cast<std::int32_t>(yMil * 10000.0));
            put(21, static_cast<std::int32_t>(20.0 * 10000.0));   // width
            put(25, static_cast<std::int32_t>(20.0 * 10000.0));   // height
            put(45, 0);                                           // hole size
            block[60] = 1;                                        // plated
            putU32(static_cast<std::uint32_t>(block.size()));
            stream.insert(stream.end(), block.begin(), block.end());
            putU32(0);            // empty per-layer sub-record (still four bytes)
        };

        putPad("1", 10.0, 20.0);
        putPad("2", 30.0, 40.0);
        stream.push_back(4);      // the next record: a track

        std::size_t pos = 0;
        pcbv::Pad first;
        pcbv::Pad second;
        const bool firstOk = pcbv::detail::parsePadRecord(stream, pos, first);
        const bool secondOk = pcbv::detail::parsePadRecord(stream, pos, second);
        check(firstOk && secondOk && first.designator == "1" && second.designator == "2",
              "both pads of a stream are read");
        check(std::abs(first.x - 10.0) < 0.01 && std::abs(second.y - 40.0) < 0.01,
              "pad positions are converted to mil");
        check(pos < stream.size() && stream[pos] == 4,
              "an empty pad sub-record does not swallow the next record");
    }

    // --- numeric layer names ------------------------------------------------
    // Real exports write layers either by name ("TOP") or by number ("1"); a numeric
    // layer that is not understood ends up grey and effectively invisible.
    {
        const std::string path = "/tmp/pcbv-smoke-layers.PcbDoc";
        const std::string text =
            "|RECORD=Board|KIND=Protel_Advanced_PCB|VERSION=5.00|KIND0=0|VX0=0mil|VY0=0mil\r\n"
            "|RECORD=Track|LAYER=1|X1=0mil|Y1=0mil|X2=100mil|Y2=0mil|WIDTH=10mil\r\n"
            "|RECORD=Track|LAYER=32|X1=0mil|Y1=10mil|X2=100mil|Y2=10mil|WIDTH=10mil\r\n"
            "|RECORD=Track|LAYER=33|X1=0mil|Y1=20mil|X2=100mil|Y2=20mil|WIDTH=10mil\r\n"
            "|RECORD=Track|X1=0mil|Y1=30mil|X2=100mil|Y2=30mil|WIDTH=10mil\r\n";
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(text.data(), 1, text.size(), f);
            std::fclose(f);
        }
        const pcbv::ParseOutcome parsed = pcbv::parseFile(path);
        check(parsed.ok && parsed.document.geometry.tracks.size() == 4,
              "numeric layer fixture parses");
        if (parsed.document.geometry.tracks.size() == 4) {
            const auto& tracks = parsed.document.geometry.tracks;
            check(tracks[0].layer == 1 && tracks[1].layer == 32 && tracks[2].layer == 33,
                  "numeric layer ids are understood");
            check(tracks[3].layer == 1, "a track without a layer falls back to top copper");
        }
        std::remove(path.c_str());
    }

    // --- board outline on the display list -----------------------------------
    // The 3D view draws the board from list.boardOutline and the parts from the
    // primitives, so both have to live in the same device space — otherwise the parts
    // float away from the board (and get mirrored) when the view is tilted.
    {
        const std::string path = "/tmp/pcbv-smoke-outline.PcbDoc";
        const std::string text =
            "|RECORD=Board|KIND=Protel_Advanced_PCB|VERSION=5.00|"
            "KIND0=0|VX0=1000mil|VY0=800mil|KIND1=0|VX1=3350mil|VY1=800mil|"
            "KIND2=0|VX2=3350mil|VY2=2750mil|KIND3=0|VX3=1000mil|VY3=2750mil\r\n"
            "|RECORD=Component|ID=0|LAYER=TOP|X=2100mil|Y=1800mil|PATTERN=QFN|"
            "SOURCEDESIGNATOR=U1\r\n"
            "|RECORD=Pad|COMPONENT=0|LAYER=TOP|NAME=1|X=2100mil|Y=1800mil|XSIZE=40mil|"
            "YSIZE=40mil\r\n";
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(text.data(), 1, text.size(), f);
            std::fclose(f);
        }
        const pcbv::ParseOutcome parsed = pcbv::parseFile(path);
        check(parsed.ok && parsed.document.geometry.outline.size() == 4,
              "ASCII board outline is read");
        pcbv::BoardRenderOptions renderOptions;
        renderOptions.hiddenLayers = pcbv::defaultHiddenLayers();
        const pcbv::BoardRenderList list =
            pcbv::buildBoardRender(parsed.document, renderOptions);
        check(list.boardOutline.size() >= 3, "the display list carries the board outline");

        const double unitsPerMm = list.unitsPerMil / 0.0254;
        auto toBoardMm = [unitsPerMm](const pcbv::Point& point) {
            return pcbv::Point{point.x / unitsPerMm, -point.y / unitsPerMm};
        };
        std::vector<pcbv::Point> outlineMm;
        for (const pcbv::Point& point : list.boardOutline) outlineMm.push_back(toBoardMm(point));

        // A simple even-odd test: nothing may stick out of the board.
        auto inside = [&outlineMm](const pcbv::Point& point) {
            bool in = false;
            for (std::size_t i = 0, j = outlineMm.size() - 1; i < outlineMm.size(); j = i++) {
                const pcbv::Point& a = outlineMm[i];
                const pcbv::Point& b = outlineMm[j];
                if ((a.y > point.y) != (b.y > point.y) &&
                    point.x < (b.x - a.x) * (point.y - a.y) / (b.y - a.y) + a.x) {
                    in = !in;
                }
            }
            return in;
        };
        // Every primitive except the board edge itself must fall inside the outline.
        bool sawPart = false;
        bool partOutside = false;
        for (const pcbv::RenderPrimitive& primitive : list.primitives) {
            if (primitive.background) continue;
            // The board edge itself is not a part.
            bool isBoardEdge = primitive.points.size() == list.boardOutline.size() &&
                               !primitive.points.empty();
            for (std::size_t i = 0; isBoardEdge && i < primitive.points.size(); ++i) {
                isBoardEdge = primitive.points[i].x == list.boardOutline[i].x &&
                              primitive.points[i].y == list.boardOutline[i].y;
            }
            if (isBoardEdge) continue;
            pcbv::Point sample{primitive.x, primitive.y};
            if (primitive.kind == pcbv::RenderPrimitive::Kind::Circle) {
                sample = primitive.center;
            } else if (!primitive.points.empty()) {
                sample = primitive.points.front();
            }
            sawPart = true;
            if (!inside(toBoardMm(sample))) partOutside = true;
        }
        check(sawPart && !partOutside,
              "every primitive of the 3D view lands inside the board outline");
        std::remove(path.c_str());
    }

    // --- global search across documents --------------------------------------
    {
        const std::string path = "/tmp/pcbv-smoke-search.PcbDoc";
        const std::string text =
            "|RECORD=Board|KIND=Protel_Advanced_PCB|VERSION=5.00|VX0=0mil|VY0=0mil\r\n"
            "|RECORD=Net|ID=0|NAME=VCC_3V3\r\n"
            "|RECORD=Net|ID=1|NAME=GND\r\n"
            "|RECORD=Component|ID=0|LAYER=TOP|X=100mil|Y=200mil|SOURCEDESIGNATOR=U7|COMMENT=STM32|"
            "PATTERN=LQFP48|MANUFACTURER=ST\r\n"
            "|RECORD=Pad|COMPONENT=0|NET=0|LAYER=TOP|NAME=1|X=90mil|Y=200mil|XSIZE=20mil|YSIZE=20mil\r\n"
            "|RECORD=Track|NET=0|LAYER=TOP|X1=0mil|Y1=0mil|X2=100mil|Y2=0mil|WIDTH=10mil\r\n";
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(text.data(), 1, text.size(), f);
            std::fclose(f);
        }
        const pcbv::ParseOutcome doc = pcbv::parseFile(path);
        check(doc.ok, "search fixture parses");
        // A part carries its own pads, so a 3D body can be derived from them.
        check(!doc.document.components.empty() &&
                  doc.document.components.front().pads.size() == 1,
              "a PCB part keeps its own pads for the 3D body");
        const pcbv::Library emptyLibrary;

        const auto byDesignator = pcbv::searchDocument(doc.document, emptyLibrary, "u7");
        check(byDesignator.size() == 1 && byDesignator.front().kind == pcbv::SearchHit::Kind::Component,
              "search finds a component by designator (case insensitive)");
        const auto byValue = pcbv::searchDocument(doc.document, emptyLibrary, "stm32");
        check(!byValue.empty() && byValue.front().name == "U7", "search finds a component by value");
        const auto byFootprint = pcbv::searchDocument(doc.document, emptyLibrary, "lqfp");
        check(!byFootprint.empty(), "search finds a component by footprint");
        const auto byNet = pcbv::searchDocument(doc.document, emptyLibrary, "gnd");
        check(byNet.size() == 1 && byNet.front().kind == pcbv::SearchHit::Kind::Net,
              "search finds a net");
        check(pcbv::searchDocument(doc.document, emptyLibrary, "vcc").size() == 1,
              "search finds a net by a partial name");
        check(!pcbv::searchDocument(doc.document, emptyLibrary, "vcc_3").empty(),
              "search also finds a net that carries no copper yet");
        check(pcbv::searchDocument(doc.document, emptyLibrary, "nothing-here").empty(),
              "search reports no hits for unknown text");
        check(pcbv::searchDocument(doc.document, emptyLibrary, "  ").empty(),
              "search ignores blank input");
        check(pcbv::searchDocument(doc.document, emptyLibrary, "top").size() >= 1,
              "search finds a layer");
        std::remove(path.c_str());
    }

    // --- grouped BOM and revision comparison --------------------------------
    {
        const std::string beforePath = "/tmp/pcbv-smoke-bom-before.PcbDoc";
        const std::string afterPath = "/tmp/pcbv-smoke-bom-after.PcbDoc";
        const std::string header =
            "|RECORD=Board|KIND=Protel_Advanced_PCB|VERSION=5.00|VX0=0mil|VY0=0mil\r\n";
        const std::string before =
            header +
            "|RECORD=Component|ID=0|LAYER=TOP|X=100mil|Y=200mil|SOURCEDESIGNATOR=R1|COMMENT=10k|"
            "PATTERN=0402\r\n"
            "|RECORD=Component|ID=1|LAYER=TOP|X=300mil|Y=200mil|SOURCEDESIGNATOR=R2|COMMENT=10k|"
            "PATTERN=0402\r\n"
            "|RECORD=Component|ID=2|LAYER=TOP|X=500mil|Y=200mil|SOURCEDESIGNATOR=C1|COMMENT=100n|"
            "PATTERN=0603\r\n";
        const std::string after =
            header +
            "|RECORD=Component|ID=0|LAYER=TOP|X=100mil|Y=200mil|SOURCEDESIGNATOR=R1|COMMENT=12k|"
            "PATTERN=0402\r\n"
            "|RECORD=Component|ID=1|LAYER=TOP|X=300mil|Y=200mil|SOURCEDESIGNATOR=R2|COMMENT=10k|"
            "PATTERN=0402\r\n"
            "|RECORD=Component|ID=2|LAYER=TOP|X=700mil|Y=200mil|SOURCEDESIGNATOR=R3|COMMENT=1k|"
            "PATTERN=0402\r\n";
        auto write = [](const std::string& path, const std::string& text) {
            if (FILE* f = std::fopen(path.c_str(), "wb")) {
                std::fwrite(text.data(), 1, text.size(), f);
                std::fclose(f);
            }
        };
        write(beforePath, before);
        write(afterPath, after);

        const pcbv::ParseOutcome left = pcbv::parseFile(beforePath);
        const pcbv::ParseOutcome right = pcbv::parseFile(afterPath);
        check(left.ok && right.ok, "BOM fixtures parse");
        check(left.document.components.size() == 3, "BOM fixture has three parts");

        const std::vector<pcbv::BomEntry> bom = pcbv::buildBom(left.document);
        check(bom.size() == 2, "identical parts collapse into one BOM line");
        if (bom.size() == 2) {
            const pcbv::BomEntry* resistors = nullptr;
            for (const auto& entry : bom) {
                if (!entry.designators.empty() && entry.designators.front() == "R1") resistors = &entry;
            }
            check(resistors != nullptr && resistors->quantity == 2,
                  "BOM line counts its designators");
            check(resistors != nullptr && resistors->designatorList() == "R1, R2",
                  "designators are listed naturally");
            check(bom.front().designators.front() == "C1", "BOM lines are ordered by designator");
        }
        check(pcbv::bomSummary(left.document) == "2 lines · 3 parts", "BOM summary is correct");
        check(pcbv::bomToCsv(left.document).find("\"R1, R2\"") != std::string::npos,
              "BOM CSV quotes the designator list");
        check(pcbv::bomToHtml(left.document).find("<table") != std::string::npos,
              "BOM HTML contains a table");

        pcbv::Component dnp;
        dnp.designator = "R9";
        dnp.properties.push_back(pcbv::Property{"DNP", "TRUE"});
        check(pcbv::isDoNotPopulate(dnp), "DNP parameter is recognised");
        dnp.properties.back().value = "FALSE";
        check(!pcbv::isDoNotPopulate(dnp), "DNP=FALSE is fitted");

        const pcbv::CompareResult same =
            pcbv::compareDocuments(left.document, left.document);
        check(same.identical(), "comparing a document with itself finds no differences");

        const pcbv::CompareResult diff = pcbv::compareDocuments(left.document, right.document);
        check(diff.added.size() == 1 && diff.added.front().designator == "R3", "R3 reported as added");
        check(diff.removed.size() == 1 && diff.removed.front().designator == "C1",
              "C1 reported as removed");
        check(diff.changed.size() == 1 && diff.changed.front().designator == "R1",
              "R1 reported as changed");
        if (diff.changed.size() == 1 && !diff.changed.front().changes.empty()) {
            const pcbv::FieldChange& change = diff.changed.front().changes.front();
            check(change.field == "Comment" && change.before == "10k" && change.after == "12k",
                  "the changed field carries both values");
        }
        check(pcbv::compareToText(diff).find("+ R3") != std::string::npos,
              "text report lists the added part");
        check(pcbv::compareToHtml(diff).find("R3") != std::string::npos,
              "HTML report lists the added part");

        std::remove(beforePath.c_str());
        std::remove(afterPath.c_str());
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

        // The 3D view puts every primitive on its own layer, so the display list has to
        // carry the layer id for all of them.
        bool sawTop = false;
        bool sawBottom = false;
        for (const auto& primitive : render.primitives) {
            if (primitive.layer == 1) sawTop = true;
            if (primitive.layer == 32) sawBottom = true;
        }
        check(sawTop, "display list keeps the top layer id");
        bool boardHasBottom = false;
        for (const auto& track : doc.geometry.tracks) {
            if (track.layer == 32) boardHasBottom = true;
        }
        if (boardHasBottom) check(sawBottom, "display list keeps the bottom layer id");

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

        std::size_t netlistPads = 0;
        for (const auto& net : pcbv::buildNetlist(doc)) netlistPads += net.nodes.size();
        check(netlistPads == doc.geometry.pads.size(),
              "the netlist contains every pad exactly once");
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
