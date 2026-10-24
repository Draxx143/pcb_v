// AltiumInternal.h — helpers shared between the binary (.PcbDoc/.SchDoc OLE2) reader
// and the ASCII ("PCB ASCII File" / "Schematic Capture Ascii File") reader.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Model.h"

namespace pcbv {
namespace detail {

/// One schematic record, as produced by the binary or the ASCII reader.
struct SchRecord {
    int index = -1;   ///< OWNERINDEX space (header/licence records are excluded)
    int type = -1;    ///< RECORD value, -1 when the record has none
    bool binary = false;
    std::vector<Property> props;
};

/// Reads the FileHeader stream of a binary SchDoc into records.
std::vector<SchRecord> readBinarySchRecords(const std::vector<std::uint8_t>& stream);

/// Reads a schematic record stream (SchDoc FileHeader or one .SchLib symbol) into records.
std::vector<SchRecord> readSchRecordStream(const std::vector<std::uint8_t>& stream);

/// Splits an ASCII schematic export into records, with the same OWNERINDEX numbering a
/// binary file uses (the first "|HEADER=" line is not an object).
std::vector<SchRecord> readAsciiSchRecords(const std::vector<std::uint8_t>& bytes);

/// Populates components/pins/statistics of `document` from schematic records.
void buildSchDocument(const std::vector<SchRecord>& records, Document& document);

/// Splits Altium ASCII text into property lists (records). Records are separated by
/// CR, LF or CRLF; every record starts with "|RECORD=".
std::vector<std::vector<Property>> readAsciiRecords(const std::vector<std::uint8_t>& bytes,
                                                    bool* wasUtf8 = nullptr);

/// ASCII readers.
void parseAsciiPcb(const std::vector<std::uint8_t>& bytes, Document& document);
void parseAsciiSch(const std::vector<std::uint8_t>& bytes, Document& document);

/// Sorts pads of every component and resolves a human readable layer name.
void sortComponentPads(Document& document);

} // namespace detail
} // namespace pcbv
