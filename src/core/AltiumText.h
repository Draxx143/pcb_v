// AltiumText.h — text/parameter helpers shared by the binary and the ASCII readers.
//
// Altium stores every record as a "property list":  |KEY=VALUE|KEY=VALUE|...
// This module decodes those lists (Windows-1252 plus the %UTF8% / UNICODE__ overrides
// Altium uses for non Latin-1 text) and offers the small parsing helpers the readers
// need. No Qt, no third-party dependencies.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Model.h"

namespace pcbv {
namespace text {

void appendUtf8(std::string& out, std::uint32_t codePoint);

/// Windows-1252 bytes -> UTF-8.
std::string fromCp1252(const std::uint8_t* data, std::size_t length);
std::string fromCp1252(const std::string& raw);

/// Raw bytes that are already UTF-8 -> std::string (invalid sequences are kept byte-for-byte).
std::string fromUtf8(const std::uint8_t* data, std::size_t length);

std::string upper(std::string value);
std::string lower(std::string value);
std::string trim(const std::string& value);
bool endsWithNoCase(const std::string& value, const std::string& suffix);
bool equalsNoCase(const std::string& a, const std::string& b);

/// Parses a property list. `bytesAreUtf8` selects the decoding of plain values;
/// values tagged with `%UTF8%KEY` are always taken as UTF-8 (Altium's override for
/// characters outside the local code page).
std::vector<Property> parseParameters(const std::uint8_t* data, std::size_t length,
                                      bool bytesAreUtf8 = false);

/// Applies the binary-form Unicode overrides (|UNICODE=EXISTS|UNICODE__KEY=72,105|...).
void applyUnicodeOverrides(std::vector<Property>& properties);

const Property* find(const std::vector<Property>& properties, const std::string& name);
std::string valueOf(const std::vector<Property>& properties, const std::string& name,
                    const std::string& fallback = std::string());
bool hasKey(const std::vector<Property>& properties, const std::string& name);

/// Parses "1234.56", "1234.56mil", "12.5mm" or "1.2in" into mil.
double parseMil(const std::string& value);
double parseNumber(const std::string& value);
int parseInt(const std::string& value, int fallback = 0);
bool parseBool(const std::string& value, bool fallback = false);

std::string pcbLayerName(int layer);
int pcbLayerIdFromName(const std::string& name);
std::string pinElectricalName(int type);

/// Locale independent natural ordering ("A2" < "A10").
bool naturalLess(const std::string& a, const std::string& b);

/// True when the byte range is valid UTF-8 (used to sniff ASCII exports).
bool isValidUtf8(const std::uint8_t* data, std::size_t length);

} // namespace text
} // namespace pcbv
