// Inflate.cpp — see Inflate.h. Derived from the well known "puff" decoder idea
// (Mark Adler's reference implementation), rewritten in C++ with bounds checks.
#include "Inflate.h"

#include <cstring>

namespace pcbv {
namespace {

constexpr int kMaxBits = 15;
constexpr int kMaxCodes = 286;
constexpr int kMaxLengthCodes = 29;
constexpr int kMaxDistanceCodes = 30;

struct Huffman {
    short count[kMaxBits + 1]{};
    short symbol[kMaxCodes]{};
};

struct State {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::size_t pos = 0;   // byte position
    int bitBuffer = 0;
    int bitCount = 0;
    std::vector<std::uint8_t>* out = nullptr;

    /// Reads `need` bits (LSB first) or returns -1 at the end of the stream.
    int bits(int need) {
        long value = bitBuffer;
        while (bitCount < need) {
            if (pos >= size) return -1;
            value |= static_cast<long>(data[pos++]) << bitCount;
            bitCount += 8;
        }
        bitBuffer = static_cast<int>(value >> need);
        bitCount -= need;
        return static_cast<int>(value & ((1L << need) - 1));
    }
};

int decodeSymbol(State& state, const Huffman& huffman) {
    int code = 0;
    int first = 0;
    int index = 0;
    for (int length = 1; length <= kMaxBits; ++length) {
        const int bit = state.bits(1);
        if (bit < 0) return -1;
        code |= bit;
        const int count = huffman.count[length];
        if (code - count < first) return huffman.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

/// Builds the decoding tables; returns 0 on success.
int construct(Huffman& huffman, const short* lengths, int symbols) {
    for (int length = 0; length <= kMaxBits; ++length) huffman.count[length] = 0;
    for (int symbol = 0; symbol < symbols; ++symbol) ++huffman.count[lengths[symbol]];
    if (huffman.count[0] == symbols) return 0;

    int left = 1;
    for (int length = 1; length <= kMaxBits; ++length) {
        left <<= 1;
        left -= huffman.count[length];
        if (left < 0) return left;
    }
    short offsets[kMaxBits + 1]{};
    for (int length = 1; length < kMaxBits; ++length) {
        offsets[length + 1] = static_cast<short>(offsets[length] + huffman.count[length]);
    }
    for (int symbol = 0; symbol < symbols; ++symbol) {
        if (lengths[symbol] != 0) huffman.symbol[offsets[lengths[symbol]]++] = static_cast<short>(symbol);
    }
    return left;
}

bool stored(State& state) {
    state.bitBuffer = 0;
    state.bitCount = 0;
    if (state.pos + 4 > state.size) return false;
    const unsigned length = state.data[state.pos] | (state.data[state.pos + 1] << 8);
    const unsigned complement = state.data[state.pos + 2] | (state.data[state.pos + 3] << 8);
    state.pos += 4;
    if (length != (~complement & 0xFFFF)) return false;
    if (state.pos + length > state.size) return false;
    state.out->insert(state.out->end(), state.data + state.pos, state.data + state.pos + length);
    state.pos += length;
    return true;
}

bool codes(State& state, const Huffman& lengthCode, const Huffman& distanceCode) {
    static const short lengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,
                                         15, 17, 19, 23, 27, 31, 35, 43, 51,  59,
                                         67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const short lengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                          2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const short distanceBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,
                                           17,   25,   33,   49,   65,   97,    129,   193,
                                           257,  385,  513,  769,  1025, 1537,  2049,  3073,
                                           4097, 6145, 8193, 12289, 16385, 24577};
    static const short distanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                            6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

    for (;;) {
        int symbol = decodeSymbol(state, lengthCode);
        if (symbol < 0) return false;
        if (symbol < 256) {
            state.out->push_back(static_cast<std::uint8_t>(symbol));
            continue;
        }
        if (symbol == 256) return true;
        symbol -= 257;
        if (symbol >= kMaxLengthCodes) return false;
        const int extra = state.bits(lengthExtra[symbol]);
        if (extra < 0) return false;
        const std::size_t length = static_cast<std::size_t>(lengthBase[symbol] + extra);

        const int distanceSymbol = decodeSymbol(state, distanceCode);
        if (distanceSymbol < 0 || distanceSymbol >= kMaxDistanceCodes) return false;
        const int distanceBits = state.bits(distanceExtra[distanceSymbol]);
        if (distanceBits < 0) return false;
        const std::size_t distance =
            static_cast<std::size_t>(distanceBase[distanceSymbol] + distanceBits);
        if (distance > state.out->size()) return false;

        const std::size_t start = state.out->size() - distance;
        for (std::size_t i = 0; i < length; ++i) {
            state.out->push_back((*state.out)[start + i]);
        }
    }
}

bool fixed(State& state) {
    Huffman lengthCode;
    Huffman distanceCode;
    short lengths[288]{};
    int symbol = 0;
    for (; symbol < 144; ++symbol) lengths[symbol] = 8;
    for (; symbol < 256; ++symbol) lengths[symbol] = 9;
    for (; symbol < 280; ++symbol) lengths[symbol] = 7;
    for (; symbol < 288; ++symbol) lengths[symbol] = 8;
    if (construct(lengthCode, lengths, 288) != 0) return false;
    for (symbol = 0; symbol < kMaxDistanceCodes; ++symbol) lengths[symbol] = 5;
    if (construct(distanceCode, lengths, kMaxDistanceCodes) != 0) return false;
    return codes(state, lengthCode, distanceCode);
}

bool dynamic(State& state) {
    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    short lengths[kMaxCodes + kMaxDistanceCodes]{};
    Huffman lengthCode;
    Huffman distanceCode;

    const int lengthCount = state.bits(5);
    const int distanceCount = state.bits(5);
    const int codeCount = state.bits(4);
    if (lengthCount < 0 || distanceCount < 0 || codeCount < 0) return false;
    const int nlen = lengthCount + 257;
    const int ndist = distanceCount + 1;
    const int ncode = codeCount + 4;
    if (nlen > kMaxCodes || ndist > kMaxDistanceCodes) return false;

    int index = 0;
    for (; index < ncode; ++index) {
        const int value = state.bits(3);
        if (value < 0) return false;
        lengths[order[index]] = static_cast<short>(value);
    }
    for (; index < 19; ++index) lengths[order[index]] = 0;
    if (construct(lengthCode, lengths, 19) != 0) return false;

    index = 0;
    while (index < nlen + ndist) {
        int symbol = decodeSymbol(state, lengthCode);
        if (symbol < 0) return false;
        if (symbol < 16) {
            lengths[index++] = static_cast<short>(symbol);
            continue;
        }
        short value = 0;
        int repeat = 0;
        if (symbol == 16) {
            if (index == 0) return false;
            value = lengths[index - 1];
            repeat = state.bits(2);
            if (repeat < 0) return false;
            repeat += 3;
        } else if (symbol == 17) {
            repeat = state.bits(3);
            if (repeat < 0) return false;
            repeat += 3;
        } else {
            repeat = state.bits(7);
            if (repeat < 0) return false;
            repeat += 11;
        }
        if (index + repeat > nlen + ndist) return false;
        while (repeat-- > 0) lengths[index++] = value;
    }

    if (lengths[256] == 0) return false;   // no end-of-block code
    const int error = construct(lengthCode, lengths, nlen);
    if (error < 0 || (error > 0 && nlen - lengthCode.count[0] != 1)) return false;
    const int distanceError = construct(distanceCode, lengths + nlen, ndist);
    if (distanceError < 0 || (distanceError > 0 && ndist - distanceCode.count[0] != 1)) return false;
    return codes(state, lengthCode, distanceCode);
}

bool inflateLoop(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out,
                 std::size_t startPos) {
    State state;
    state.data = data;
    state.size = size;
    state.pos = startPos;
    state.out = &out;

    int last = 0;
    while (!last) {
        last = state.bits(1);
        if (last < 0) return false;
        const int type = state.bits(2);
        if (type < 0) return false;
        bool ok = false;
        switch (type) {
            case 0: ok = stored(state); break;
            case 1: ok = fixed(state); break;
            case 2: ok = dynamic(state); break;
            default: return false;
        }
        if (!ok) return false;
    }
    return true;
}

} // namespace

bool inflateRaw(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out) {
    out.clear();
    return inflateLoop(data, size, out, 0);
}

bool inflateAny(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out) {
    if (size >= 2 && (data[0] & 0x0F) == 8 && ((data[0] << 8) + data[1]) % 31 == 0) {
        // zlib wrapper: two header bytes, optional dictionary, Adler-32 at the end.
        const std::size_t start = (data[1] & 0x20) != 0 ? 6 : 2;
        if (start < size) return inflateLoop(data, size, out, start);
    }
    return inflateRaw(data, size, out);
}

} // namespace pcbv
