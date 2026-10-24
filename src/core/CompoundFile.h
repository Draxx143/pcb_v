// CompoundFile.h — Minimal, dependency-free reader for the Microsoft Compound File
// Binary (OLE2 / CFB) container that Altium uses for .PcbDoc, .SchDoc, .PcbLib and .SchLib.
//
// Only reading is supported (that is all this application needs).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pcbv {

/// One node of the OLE container tree (a storage = folder, or a stream = file).
struct FileEntry {
    std::string name;
    bool isStorage = false;
    std::uint64_t size = 0;             // stream length in bytes (0 for storages)
    std::uint32_t dirIndex = 0xFFFFFFFF; // index in the OLE directory table
    std::vector<FileEntry> children;    // sub-nodes (storages only)

    /// Returns the child with the given name (case-insensitive) or nullptr.
    const FileEntry* find(const std::string& childName) const;
    /// Path of this entry relative to the root, e.g. "Pads6/Data" (\"/\" separated).
    std::string path;
};

/// Reader for the CFB/OLE2 container.
class CompoundFile {
public:
    CompoundFile() = default;

    /// Opens a file from disk. Returns false and fills `error` on failure.
    bool openFromFile(const std::string& path, std::string& error);
    /// Opens from an in-memory buffer (the buffer is copied).
    bool openFromMemory(const std::vector<std::uint8_t>& bytes, std::string& error);

    bool isOpen() const { return !data_.empty(); }

    /// Root node of the container tree.
    const FileEntry& root() const { return tree_; }

    /// True when a stream (or storage) exists at the given slash separated path.
    bool exists(const std::string& path) const;

    /// Reads a stream. Returns an empty buffer if the stream is missing or empty.
    std::vector<std::uint8_t> stream(const std::string& path) const;

    /// Convenience: stream contents as std::string (raw bytes, no transcoding).
    std::string streamText(const std::string& path) const;

    /// All stream paths, slash separated, sorted.
    std::vector<std::string> allStreamPaths() const;

private:
    struct RawEntry {
        std::string name;
        std::uint8_t type = 0;       // 0=empty, 1=storage, 2=stream, 5=root
        std::uint32_t left = 0xFFFFFFFF;
        std::uint32_t right = 0xFFFFFFFF;
        std::uint32_t child = 0xFFFFFFFF;
        std::uint32_t startSector = 0xFFFFFFFE;
        std::uint64_t size = 0;
    };

    bool parse(std::string& error);
    const RawEntry* rawEntryAt(std::uint32_t index) const;
    std::uint64_t sectorOffset(std::uint32_t sector) const;
    std::vector<std::uint8_t> readChain(std::uint32_t start, std::uint64_t size,
                                        bool mini) const;

    void buildTree(FileEntry& node, std::uint32_t entryIndex, std::string path,
                   std::vector<std::uint32_t>& visited, int depth) const;

    std::vector<std::uint8_t> data_;
    std::vector<RawEntry> entries_;
    std::vector<std::uint32_t> fat_;
    std::vector<std::uint32_t> miniFat_;
    std::vector<std::uint8_t> miniStream_;
    std::uint32_t sectorSize_ = 512;
    std::uint32_t miniSectorSize_ = 64;
    std::uint32_t miniCutoff_ = 4096;
    FileEntry tree_;
};

} // namespace pcbv
