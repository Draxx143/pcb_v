// Inflate.h — a tiny, dependency free DEFLATE decoder.
//
// Altium stores the embedded 3D models (STEP files) inside the library as zlib
// compressed streams. Linking zlib would drag a dependency into every build (and it is
// missing on some of the machines we run on), so the decoder lives here: it is the classic
// "puff" style bit reader, about 200 lines, and only needs to handle what Altium writes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pcbv {

/// Decodes a raw DEFLATE stream (RFC 1951, no zlib header/trailer).
bool inflateRaw(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out);

/// Decodes either a zlib stream (RFC 1950, 2 byte header + Adler-32) or a raw one.
bool inflateAny(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out);

} // namespace pcbv
