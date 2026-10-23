#include "CompoundFile.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace pcbv {
namespace {

constexpr std::uint32_t kEndOfChain = 0xFFFFFFFEu;
constexpr std::uint32_t kFreeSector = 0xFFFFFFFFu;
constexpr std::uint32_t kMaxRegSector = 0xFFFFFFFAu; // >= this means "special"

std::uint16_t readU16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
std::uint32_t readU32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint64_t readU64(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(readU32(p)) |
           (static_cast<std::uint64_t>(readU32(p + 4)) << 32);
}

std::string toLowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::string utf16ToUtf8(const std::uint8_t* bytes, std::size_t lenBytes) {
    std::string out;
    out.reserve(lenBytes);
    for (std::size_t i = 0; i + 1 < lenBytes; i += 2) {
        std::uint32_t cp = readU16(bytes + i);
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < lenBytes) {
            const std::uint32_t low = readU16(bytes + i + 2);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            }
        }
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (!current.empty()) parts.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) parts.push_back(current);
    return parts;
}

} // namespace

const FileEntry* FileEntry::find(const std::string& childName) const {
    const std::string needle = toLowerAscii(childName);
    for (const auto& child : children) {
        if (toLowerAscii(child.name) == needle) return &child;
    }
    return nullptr;
}

bool CompoundFile::openFromFile(const std::string& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open file: " + path;
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    if (size <= 0) {
        error = "file is empty: " + path;
        return false;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!in && !in.eof()) {
        error = "failed to read file: " + path;
        return false;
    }
    return openFromMemory(bytes, error);
}

bool CompoundFile::openFromMemory(const std::vector<std::uint8_t>& bytes, std::string& error) {
    data_ = bytes;
    entries_.clear();
    fat_.clear();
    miniFat_.clear();
    miniStream_.clear();
    tree_ = FileEntry{};
    return parse(error);
}

const CompoundFile::RawEntry* CompoundFile::rawEntryAt(std::uint32_t index) const {
    if (index >= entries_.size()) return nullptr;
    return &entries_[index];
}

std::uint64_t CompoundFile::sectorOffset(std::uint32_t sector) const {
    return (static_cast<std::uint64_t>(sector) + 1) * sectorSize_;
}

bool CompoundFile::parse(std::string& error) {
    static const std::uint8_t kSignature[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
    if (data_.size() < 512 || std::memcmp(data_.data(), kSignature, 8) != 0) {
        error = "not a Microsoft Compound File (OLE2) — Altium .PcbDoc/.SchDoc expected";
        data_.clear();
        return false;
    }

    const std::uint16_t sectorShift = readU16(data_.data() + 0x1E);
    const std::uint16_t miniSectorShift = readU16(data_.data() + 0x20);
    if (sectorShift != 9 && sectorShift != 12) {
        error = "unsupported CFB sector size";
        data_.clear();
        return false;
    }
    sectorSize_ = 1u << sectorShift;
    miniSectorSize_ = (miniSectorShift >= 1 && miniSectorShift <= 12) ? (1u << miniSectorShift) : 64u;
    miniCutoff_ = readU32(data_.data() + 0x38);

    const std::uint32_t numFatSectors = readU32(data_.data() + 0x2C);
    const std::uint32_t dirStart = readU32(data_.data() + 0x30);
    const std::uint32_t miniFatStart = readU32(data_.data() + 0x3C);
    const std::uint32_t numMiniFat = readU32(data_.data() + 0x40);
    const std::uint32_t difatStart = readU32(data_.data() + 0x44);
    const std::uint32_t numDifat = readU32(data_.data() + 0x48);

    // ---- DIFAT (list of FAT sector numbers) ----
    std::vector<std::uint32_t> difat;
    difat.reserve(109 + static_cast<std::size_t>(numDifat) * (sectorSize_ / 4));
    for (int i = 0; i < 109; ++i) {
        difat.push_back(readU32(data_.data() + 0x4C + i * 4));
    }
    std::uint32_t difatSector = difatStart;
    const std::uint32_t perDifat = sectorSize_ / 4;
    std::uint32_t guard = 0;
    for (std::uint32_t i = 0; i < numDifat && difatSector < kMaxRegSector; ++i) {
        const std::uint64_t off = sectorOffset(difatSector);
        if (off + sectorSize_ > data_.size() || ++guard > 1000000) break;
        for (std::uint32_t j = 0; j + 1 < perDifat; ++j) {
            difat.push_back(readU32(data_.data() + off + j * 4));
        }
        difatSector = readU32(data_.data() + off + (perDifat - 1) * 4);
    }

    // ---- FAT ----
    const std::uint32_t entriesPerSector = sectorSize_ / 4;
    for (std::uint32_t i = 0; i < numFatSectors && i < difat.size(); ++i) {
        const std::uint32_t sec = difat[i];
        if (sec >= kMaxRegSector) continue;
        const std::uint64_t off = sectorOffset(sec);
        if (off + sectorSize_ > data_.size()) continue;
        for (std::uint32_t j = 0; j < entriesPerSector; ++j) {
            fat_.push_back(readU32(data_.data() + off + j * 4));
        }
    }
    if (fat_.empty()) {
        error = "corrupt CFB: empty allocation table";
        data_.clear();
        return false;
    }

    // ---- MiniFAT ----
    std::uint32_t miniSector = miniFatStart;
    guard = 0;
    for (std::uint32_t i = 0; i < numMiniFat && miniSector < kMaxRegSector; ++i) {
        const std::uint64_t off = sectorOffset(miniSector);
        if (off + sectorSize_ > data_.size()) break;
        for (std::uint32_t j = 0; j < entriesPerSector; ++j) {
            miniFat_.push_back(readU32(data_.data() + off + j * 4));
        }
        if (miniSector >= fat_.size()) break;
        miniSector = fat_[miniSector];
        if (++guard > 1000000) break;
    }

    // ---- Directory ----
    const std::vector<std::uint8_t> dirData = readChain(dirStart, 0, false);
    if (dirData.empty()) {
        error = "corrupt CFB: empty directory";
        data_.clear();
        return false;
    }
    const std::size_t entryCount = dirData.size() / 128;
    entries_.reserve(entryCount);
    for (std::size_t i = 0; i < entryCount; ++i) {
        const std::uint8_t* e = dirData.data() + i * 128;
        RawEntry entry;
        std::uint16_t nameLen = readU16(e + 0x40);
        if (nameLen > 64) nameLen = 64;
        if (nameLen >= 2) {
            entry.name = utf16ToUtf8(e, static_cast<std::size_t>(nameLen - 2));
        }
        entry.type = e[0x42];
        entry.left = readU32(e + 0x44);
        entry.right = readU32(e + 0x48);
        entry.child = readU32(e + 0x4C);
        entry.startSector = readU32(e + 0x74);
        entry.size = readU64(e + 0x78);
        entries_.push_back(std::move(entry));
    }
    if (entries_.empty()) {
        error = "corrupt CFB: no directory entries";
        data_.clear();
        return false;
    }

    // ---- Mini stream (owned by the root entry) ----
    if (entries_[0].size > 0 && entries_[0].startSector < kMaxRegSector) {
        miniStream_ = readChain(entries_[0].startSector, entries_[0].size, false);
    }

    // ---- Tree ----
    tree_ = FileEntry{};
    tree_.name = entries_[0].name.empty() ? "Root Entry" : entries_[0].name;
    tree_.isStorage = true;
    tree_.path.clear();
    std::vector<std::uint32_t> visited;
    std::uint32_t firstChild = entries_[0].child;
    if (firstChild < entries_.size()) {
        buildTree(tree_, firstChild, "", visited, 0);
    }
    return true;
}

void CompoundFile::buildTree(FileEntry& node, std::uint32_t entryIndex, std::string path,
                             std::vector<std::uint32_t>& visited, int depth) const {
    // The directory is a red-black tree: walk left/right siblings plus each sibling's child.
    if (depth > 64) return;
    std::vector<std::uint32_t> stack;
    stack.push_back(entryIndex);
    while (!stack.empty()) {
        const std::uint32_t idx = stack.back();
        stack.pop_back();
        if (idx >= entries_.size()) continue;
        if (std::find(visited.begin(), visited.end(), idx) != visited.end()) continue;
        visited.push_back(idx);

        const RawEntry& raw = entries_[idx];
        if (raw.type != 1 && raw.type != 2) continue;

        FileEntry child;
        child.name = raw.name;
        child.isStorage = (raw.type == 1);
        child.size = child.isStorage ? 0 : raw.size;
        child.dirIndex = idx;
        child.path = path.empty() ? raw.name : path + "/" + raw.name;

        if (raw.type == 1) {
            if (raw.child < entries_.size()) {
                buildTree(child, raw.child, child.path, visited, depth + 1);
            }
        }

        node.children.push_back(std::move(child));

        if (raw.left < entries_.size()) stack.push_back(raw.left);
        if (raw.right < entries_.size()) stack.push_back(raw.right);
    }
}

std::vector<std::uint8_t> CompoundFile::readChain(std::uint32_t start, std::uint64_t size,
                                                  bool mini) const {
    std::vector<std::uint8_t> out;
    if (start >= kMaxRegSector) return out;

    const std::uint32_t unit = mini ? miniSectorSize_ : sectorSize_;
    const std::vector<std::uint32_t>& table = mini ? miniFat_ : fat_;
    const std::vector<std::uint8_t>& source = mini ? miniStream_ : data_;

    std::uint32_t sector = start;
    std::size_t guard = 0;
    // Every sector contributes `unit` bytes; cap iterations at the source size.
    const std::size_t maxIterations = source.size() / (unit == 0 ? 1 : unit) + 8;
    while (sector < kMaxRegSector && guard++ < maxIterations) {
        std::uint64_t off = mini ? static_cast<std::uint64_t>(sector) * unit
                                 : sectorOffset(sector);
        if (off >= source.size()) break;
        std::size_t available = static_cast<std::size_t>(
            std::min<std::uint64_t>(unit, source.size() - off));
        if (size > 0) {
            const std::size_t produced = out.size();
            if (produced >= size) break;
            available = std::min<std::size_t>(available, static_cast<std::size_t>(size - produced));
        }
        out.insert(out.end(), source.begin() + static_cast<std::ptrdiff_t>(off),
                   source.begin() + static_cast<std::ptrdiff_t>(off + available));
        if (size > 0 && out.size() >= size) break;
        if (sector >= table.size()) break;
        const std::uint32_t next = table[sector];
        if (next == sector) break;                 // self reference: corrupt
        if (next == kFreeSector) break;
        if (next >= kMaxRegSector) break;          // ENDOFCHAIN / FATSECT / DIFSECT
        sector = next;
    }
    if (size > 0 && out.size() > size) out.resize(static_cast<std::size_t>(size));
    return out;
}

std::vector<std::uint8_t> CompoundFile::stream(const std::string& path) const {
    const std::vector<std::string> parts = splitPath(path);
    const FileEntry* node = &tree_;
    for (const std::string& part : parts) {
        node = node->find(part);
        if (node == nullptr) return {};
    }
    if (node->isStorage || node->size == 0) return {};

    const RawEntry* raw = rawEntryAt(node->dirIndex);
    if (raw == nullptr) return {};

    const bool useMini = raw->size < miniCutoff_ && !miniStream_.empty();
    std::vector<std::uint8_t> data = readChain(raw->startSector, raw->size, useMini);
    // Defensive fallback for containers written by tools that ignore the mini-stream cutoff.
    if (data.empty() && !useMini) data = readChain(raw->startSector, raw->size, false);
    return data;
}

std::string CompoundFile::streamText(const std::string& path) const {
    const std::vector<std::uint8_t> bytes = stream(path);
    return std::string(bytes.begin(), bytes.end());
}

bool CompoundFile::exists(const std::string& path) const {
    const std::vector<std::string> parts = splitPath(path);
    const FileEntry* node = &tree_;
    for (const std::string& part : parts) {
        node = node->find(part);
        if (node == nullptr) return false;
    }
    return true;
}

namespace {
void collectStreams(const FileEntry& node, std::vector<std::string>& out) {
    if (!node.isStorage) {
        if (!node.path.empty()) out.push_back(node.path);
        return;
    }
    for (const auto& child : node.children) collectStreams(child, out);
}
} // namespace

std::vector<std::string> CompoundFile::allStreamPaths() const {
    std::vector<std::string> out;
    collectStreams(tree_, out);
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace pcbv
