// PartIndex.cpp — see PartIndex.h.
#include "PartIndex.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace pcbv {
namespace {

// ---------------------------------------------------------------------------
// A very small JSON reader (objects, arrays, strings, numbers, true/false/null).
// The index is written by hand or by tools/import_kicad.py, so a full JSON library would
// be a dependency for nothing; this reader is enough and reports where it failed.
// ---------------------------------------------------------------------------

struct JsonValue {
    enum class Kind { Null, Bool, Number, String, Array, Object } kind = Kind::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<JsonValue> items;
    std::vector<std::pair<std::string, JsonValue>> members;

    const JsonValue* member(const std::string& key) const {
        for (const auto& entry : members) {
            if (entry.first == key) return &entry.second;
        }
        return nullptr;
    }
    std::string stringValue(const std::string& fallback = std::string()) const {
        return kind == Kind::String ? text : fallback;
    }
    std::vector<std::string> stringList() const {
        std::vector<std::string> out;
        if (kind != Kind::Array) return out;
        for (const JsonValue& item : items) {
            if (item.kind == Kind::String) out.push_back(item.text);
        }
        return out;
    }
};

class JsonReader {
public:
    JsonReader(const std::string& source, std::string& error) : text_(source), error_(error) {}

    bool readDocument(JsonValue& out) {
        skipSpace();
        if (!readValue(out)) return false;
        skipSpace();
        if (pos_ != text_.size()) return fail("trailing text after the JSON value");
        return true;
    }

private:
    const std::string& text_;
    std::string& error_;
    std::size_t pos_ = 0;

    bool fail(const std::string& why) {
        std::size_t line = 1, column = 1;
        for (std::size_t i = 0; i < pos_ && i < text_.size(); ++i) {
            if (text_[i] == '\n') { ++line; column = 1; } else { ++column; }
        }
        std::ostringstream message;
        message << why << " (line " << line << ", column " << column << ")";
        error_ = message.str();
        return false;
    }

    void skipSpace() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else if (c == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '/') {
                while (pos_ < text_.size() && text_[pos_] != '\n') ++pos_;
            } else {
                break;
            }
        }
    }

    bool readValue(JsonValue& out) {
        skipSpace();
        if (pos_ >= text_.size()) return fail("unexpected end of file");
        const char c = text_[pos_];
        switch (c) {
            case '{': return readObject(out);
            case '[': return readArray(out);
            case '"': out.kind = JsonValue::Kind::String; return readString(out.text);
            case 't':
                if (text_.compare(pos_, 4, "true") != 0) return fail("expected true");
                pos_ += 4;
                out.kind = JsonValue::Kind::Bool;
                out.boolean = true;
                return true;
            case 'f':
                if (text_.compare(pos_, 5, "false") != 0) return fail("expected false");
                pos_ += 5;
                out.kind = JsonValue::Kind::Bool;
                out.boolean = false;
                return true;
            case 'n':
                if (text_.compare(pos_, 4, "null") != 0) return fail("expected null");
                pos_ += 4;
                out.kind = JsonValue::Kind::Null;
                return true;
            default: return readNumber(out);
        }
    }

    bool readNumber(JsonValue& out) {
        const std::size_t start = pos_;
        if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
        bool digits = false;
        while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
            digits = true;
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                ++pos_;
                digits = true;
            }
        }
        if (digits && pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        }
        if (!digits) return fail("expected a value");
        out.kind = JsonValue::Kind::Number;
        out.number = std::atof(text_.substr(start, pos_ - start).c_str());
        return true;
    }

    bool readString(std::string& out) {
        if (pos_ >= text_.size() || text_[pos_] != '"') return fail("expected a string");
        ++pos_;
        out.clear();
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= text_.size()) return fail("unterminated string");
            const char escaped = text_[pos_++];
            switch (escaped) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (pos_ + 4 > text_.size()) return fail("truncated \\u escape");
                    unsigned code = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char hex = text_[pos_ + static_cast<std::size_t>(i)];
                        code <<= 4;
                        if (hex >= '0' && hex <= '9') code |= static_cast<unsigned>(hex - '0');
                        else if (hex >= 'a' && hex <= 'f') code |= static_cast<unsigned>(hex - 'a' + 10);
                        else if (hex >= 'A' && hex <= 'F') code |= static_cast<unsigned>(hex - 'A' + 10);
                        else return fail("bad \\u escape");
                    }
                    pos_ += 4;
                    // UTF-8 encode the code point (surrogate pairs are kept as-is).
                    if (code < 0x80) {
                        out.push_back(static_cast<char>(code));
                    } else if (code < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                    break;
                }
                default: return fail("unknown escape");
            }
        }
        return fail("unterminated string");
    }

    bool readArray(JsonValue& out) {
        ++pos_;   // '['
        out.kind = JsonValue::Kind::Array;
        skipSpace();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            JsonValue item;
            if (!readValue(item)) return false;
            out.items.push_back(std::move(item));
            skipSpace();
            if (pos_ >= text_.size()) return fail("unterminated array");
            if (text_[pos_] == ',') { ++pos_; continue; }
            if (text_[pos_] == ']') { ++pos_; return true; }
            return fail("expected ',' or ']'");
        }
    }

    bool readObject(JsonValue& out) {
        ++pos_;   // '{'
        out.kind = JsonValue::Kind::Object;
        skipSpace();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            skipSpace();
            std::string key;
            if (!readString(key)) return false;
            skipSpace();
            if (pos_ >= text_.size() || text_[pos_] != ':') return fail("expected ':'");
            ++pos_;
            JsonValue value;
            if (!readValue(value)) return false;
            out.members.emplace_back(std::move(key), std::move(value));
            skipSpace();
            if (pos_ >= text_.size()) return fail("unterminated object");
            if (text_[pos_] == ',') { ++pos_; continue; }
            if (text_[pos_] == '}') { ++pos_; return true; }
            return fail("expected ',' or '}'");
        }
    }
};

std::string lower(const std::string& text) {
    std::string out = text;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// Splits a query into words; "10k", "0603", "sot-23" all survive, punctuation is dropped.
std::vector<std::string> tokenise(const std::string& query) {
    std::vector<std::string> tokens;
    std::string current;
    for (const char c : query) {
        const unsigned char uc = static_cast<unsigned char>(c);
        // A UTF-8 continuation byte is part of the word, not a separator.
        if (std::isalnum(uc) || c == '-' || c == '_' || c == '.' || uc >= 0x80) {
            current.push_back(static_cast<char>(std::tolower(uc)));
        } else if (!current.empty()) {
            tokens.push_back(current);
            current.clear();
        }
    }
    if (!current.empty()) tokens.push_back(current);
    return tokens;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

int scoreToken(const PartEntry& part, const std::string& token, const std::string& searchable) {
    const std::string mpn = lower(part.mpn);
    const std::string package = lower(part.package);
    const std::string value = lower(part.value);
    const std::string id = lower(part.id);
    if (!mpn.empty() && mpn == token) return 100;
    if (!id.empty() && id == token) return 90;
    if (!package.empty() && package == token) return 60;
    if (!value.empty() && value == token) return 55;
    for (const std::string& tag : part.tags) {
        if (lower(tag) == token) return 45;
    }
    if (!mpn.empty() && contains(mpn, token)) return 35;
    if (!value.empty() && contains(value, token)) return 30;
    if (!package.empty() && contains(package, token)) return 28;
    for (const std::string& tag : part.tags) {
        if (contains(lower(tag), token)) return 25;
    }
    if (contains(searchable, token)) return 10;
    return 0;
}

} // namespace

std::string PartEntry::searchText() const {
    std::string all;
    for (const std::string* field : {&id, &mpn, &manufacturer, &value, &package, &description}) {
        all += lower(*field);
        all.push_back('\n');
    }
    for (const std::string& tag : tags) {
        all += lower(tag);
        all.push_back('\n');
    }
    return all;
}

std::string PartIndex::folder() const {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

bool PartIndex::load(const std::string& indexFile, std::string& error) {
    parts.clear();
    warnings.clear();
    path = indexFile;
    std::ifstream in(indexFile, std::ios::binary);
    if (!in) {
        error = "cannot open the part index: " + indexFile;
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string text = buffer.str();
    if (text.empty()) {
        error = "the part index is empty: " + indexFile;
        return false;
    }
    JsonValue root;
    JsonReader reader(text, error);
    if (!reader.readDocument(root)) {
        error = "the part index is not valid JSON: " + error;
        return false;
    }
    if (root.kind != JsonValue::Kind::Object) {
        error = "the part index must be a JSON object";
        return false;
    }
    if (const JsonValue* formatValue = root.member("format")) format = formatValue->stringValue();
    if (const JsonValue* versionValue = root.member("version")) {
        version = static_cast<int>(versionValue->number);
    }
    const JsonValue* list = root.member("parts");
    if (list == nullptr || list->kind != JsonValue::Kind::Array) {
        error = "the part index has no \"parts\" array";
        return false;
    }
    for (const JsonValue& item : list->items) {
        if (item.kind != JsonValue::Kind::Object) {
            warnings.push_back("a part row is not an object and was skipped");
            continue;
        }
        PartEntry part;
        auto field = [&item](const char* key) {
            const JsonValue* value = item.member(key);
            return value != nullptr ? value->stringValue() : std::string();
        };
        part.id = field("id");
        part.mpn = field("mpn");
        part.manufacturer = field("manufacturer");
        part.value = field("value");
        part.package = field("package");
        part.description = field("description");
        part.datasheet = field("datasheet");
        part.symbol = field("symbol");
        part.footprint = field("footprint");
        part.model = field("model");
        if (const JsonValue* tags = item.member("tags")) part.tags = tags->stringList();
        if (part.id.empty()) part.id = part.mpn;
        if (part.id.empty() && part.mpn.empty()) {
            warnings.push_back("a part row has neither id nor mpn and was skipped");
            continue;
        }
        parts.push_back(std::move(part));
    }
    if (parts.empty()) warnings.push_back("the part index holds no part yet");
    return true;
}

std::vector<PartMatch> searchParts(const PartIndex& index, const std::string& query,
                                   std::size_t limit) {
    const std::vector<std::string> tokens = tokenise(query);
    std::vector<PartMatch> matches;
    if (tokens.empty()) return matches;
    for (const PartEntry& part : index.parts) {
        const std::string searchable = part.searchText();
        PartMatch match;
        match.part = &part;
        int total = 0;
        bool all = true;
        for (const std::string& token : tokens) {
            const int score = scoreToken(part, token, searchable);
            if (score == 0) {
                all = false;
                break;
            }
            total += score;
            match.tokens.push_back(token);
        }
        if (!all) continue;
        match.score = total;
        matches.push_back(std::move(match));
    }
    std::stable_sort(matches.begin(), matches.end(), [](const PartMatch& a, const PartMatch& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.part->mpn < b.part->mpn;
    });
    if (matches.size() > limit) matches.resize(limit);
    return matches;
}

PartIndexCheck checkPartIndex(const PartIndex& index) {
    PartIndexCheck check;
    check.parts = index.parts.size();
    const std::filesystem::path base(index.folder());
    std::error_code error;
    for (const PartEntry& part : index.parts) {
        const std::pair<const char*, const std::string*> files[] = {
            {"symbol", &part.symbol}, {"footprint", &part.footprint},
            {"model", &part.model}, {"datasheet", &part.datasheet},
        };
        for (const auto& entry : files) {
            const std::string& value = *entry.second;
            if (value.empty()) continue;
            // An absolute path or a URL is not checked here; only local files are.
            if (value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0) continue;
            const std::filesystem::path given(value);
            const std::filesystem::path resolved = given.is_absolute() ? given : base / given;
            if (!std::filesystem::is_regular_file(resolved, error)) {
                ++check.missing;
                check.messages.push_back(part.id + ": missing " + entry.first + " file " +
                                         resolved.string());
            }
        }
    }
    return check;
}

std::string partIndexToText(const PartIndex& index) {
    std::ostringstream out;
    out << "Part index  " << (index.path.empty() ? "(none)" : index.path) << "\n";
    out << "Format      " << (index.format.empty() ? "pcbv-part-index" : index.format)
        << "  v" << (index.version > 0 ? index.version : 1) << "\n";
    out << "Parts       " << index.parts.size() << "\n\n";
    if (index.parts.empty()) {
        out << "No part yet. Add rows to libraries/index.json or run tools/import_kicad.py.\n";
        return out.str();
    }
    out << std::left << std::setw(22) << "ID" << std::setw(20) << "MPN" << std::setw(10)
        << "Package" << std::setw(10) << "Value" << "Description\n";
    out << std::string(96, '-') << "\n";
    for (const PartEntry& part : index.parts) {
        auto clip = [](const std::string& text, std::size_t width) {
            return text.size() <= width ? text : text.substr(0, width - 1) + "…";
        };
        out << std::left << std::setw(22) << clip(part.id, 21) << std::setw(20)
            << clip(part.mpn, 19) << std::setw(10) << clip(part.package, 9) << std::setw(10)
            << clip(part.value, 9) << clip(part.description, 48) << "\n";
    }
    return out.str();
}

std::string partMatchesToText(const PartIndex& index, const std::vector<PartMatch>& matches,
                              const std::string& query) {
    std::ostringstream out;
    out << "Search      \"" << query << "\" in " << index.parts.size() << " part(s)\n";
    if (matches.empty()) {
        out << "\nNo part matches every word of the query.\n";
        return out.str();
    }
    out << matches.size() << " match(es)\n\n";
    for (const PartMatch& match : matches) {
        const PartEntry& part = *match.part;
        out << std::left << std::setw(22) << part.id << std::setw(20) << part.mpn << std::setw(10)
            << part.package << std::setw(10) << part.value << " score " << match.score << "\n";
        if (!part.description.empty()) out << "    " << part.description << "\n";
        auto line = [&out](const char* label, const std::string& value) {
            if (!value.empty()) out << "    " << std::left << std::setw(10) << label << value << "\n";
        };
        line("footprint", part.footprint);
        line("symbol", part.symbol);
        line("model", part.model);
        line("datasheet", part.datasheet);
    }
    return out.str();
}

} // namespace pcbv
