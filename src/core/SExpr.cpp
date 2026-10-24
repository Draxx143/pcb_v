// SExpr.cpp — see SExpr.h.
#include "SExpr.h"

#include <cctype>
#include <cstdlib>
#include <stdexcept>

namespace pcbv {
namespace sexpr {
namespace {

struct Parser {
    const std::string& text;
    std::size_t pos = 0;

    explicit Parser(const std::string& source) : text(source) {}

    void skipSpace() {
        while (pos < text.size()) {
            const char c = text[pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
                ++pos;
            } else if (c == ';') {   // KiCad does not use comments, but be forgiving
                while (pos < text.size() && text[pos] != '\n') ++pos;
            } else {
                break;
            }
        }
    }

    /// Reads a quoted string, honouring backslash escapes, and returns it without quotes.
    std::string quoted() {
        ++pos;   // opening quote
        std::string out;
        while (pos < text.size()) {
            const char c = text[pos++];
            if (c == '\\' && pos < text.size()) {
                const char escaped = text[pos++];
                switch (escaped) {
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case 'r': out.push_back('\r'); break;
                    default: out.push_back(escaped); break;
                }
                continue;
            }
            if (c == '"') return out;
            out.push_back(c);
        }
        throw std::runtime_error("unterminated string");
    }

    /// Reads a bare atom; a `|quoted atom|` form is also accepted.
    std::string bare() {
        if (pos < text.size() && text[pos] == '|') {
            ++pos;
            std::string out;
            while (pos < text.size() && text[pos] != '|') out.push_back(text[pos++]);
            if (pos < text.size()) ++pos;
            return out;
        }
        const std::size_t start = pos;
        while (pos < text.size()) {
            const char c = text[pos];
            if (c == '(' || c == ')' || c == '"' || c == ' ' || c == '\t' || c == '\r' ||
                c == '\n' || c == '\f' || c == '\v') {
                break;
            }
            ++pos;
        }
        return text.substr(start, pos - start);
    }

    Node list() {
        ++pos;   // '('
        Node node;
        node.isList = true;
        while (true) {
            skipSpace();
            if (pos >= text.size()) throw std::runtime_error("missing closing parenthesis");
            if (text[pos] == ')') {
                ++pos;
                return node;
            }
            node.children.push_back(value());
        }
    }

    Node value() {
        skipSpace();
        if (pos >= text.size()) throw std::runtime_error("unexpected end of file");
        if (text[pos] == '(') return list();
        Node node;
        if (text[pos] == '"') {
            node.atom = quoted();
        } else {
            node.atom = bare();
        }
        return node;
    }
};

} // namespace

const std::string& Node::head() const {
    static const std::string kEmpty;
    if (!isList || children.empty() || children.front().isList) return kEmpty;
    return children.front().atom;
}

std::string Node::first() const {
    for (std::size_t i = 1; i < children.size(); ++i) {
        if (!children[i].isList) return children[i].atom;
    }
    return {};
}

std::vector<std::string> Node::atoms() const {
    std::vector<std::string> out;
    for (const Node& child : children) {
        if (!child.isList) out.push_back(child.atom);
    }
    return out;
}

const Node* Node::child(const std::string& key) const {
    if (!isList) return nullptr;
    for (const Node& child : children) {
        if (child.isList && child.head() == key) return &child;
    }
    return nullptr;
}

std::vector<const Node*> Node::childrenNamed(const std::string& key) const {
    std::vector<const Node*> out;
    if (!isList) return out;
    for (const Node& child : children) {
        if (child.isList && child.head() == key) out.push_back(&child);
    }
    return out;
}

double Node::number(const std::string& key, std::size_t index, double fallback) const {
    const Node* list = child(key);
    if (list == nullptr) return fallback;
    std::size_t seen = 0;
    for (std::size_t i = 1; i < list->children.size(); ++i) {
        const Node& argument = list->children[i];
        if (argument.isList) continue;
        if (seen++ == index) return argument.asNumber(fallback);
    }
    return fallback;
}

std::string Node::text(const std::string& key, std::size_t index, const std::string& fallback) const {
    const Node* list = child(key);
    if (list == nullptr) return fallback;
    std::size_t seen = 0;
    for (std::size_t i = 1; i < list->children.size(); ++i) {
        const Node& argument = list->children[i];
        if (argument.isList) continue;
        if (seen++ == index) return argument.atom;
    }
    return fallback;
}

double Node::asNumber(double fallback) const {
    if (isList || atom.empty()) return fallback;
    char* end = nullptr;
    const double value = std::strtod(atom.c_str(), &end);
    if (end == nullptr || *end != '\0') return fallback;
    return value;
}

bool parse(const std::string& text, std::vector<Node>& roots, std::string* error) {
    roots.clear();
    Parser parser(text);
    try {
        // Skip a UTF-8 byte order mark, which some editors add.
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
            static_cast<unsigned char>(text[1]) == 0xBB &&
            static_cast<unsigned char>(text[2]) == 0xBF) {
            parser.pos = 3;
        }
        while (true) {
            parser.skipSpace();
            if (parser.pos >= text.size()) break;
            if (text[parser.pos] == ')') throw std::runtime_error("unexpected ')'");
            roots.push_back(parser.value());
        }
    } catch (const std::exception& ex) {
        if (error != nullptr) *error = ex.what();
        roots.clear();
        return false;
    }
    if (roots.empty()) {
        if (error != nullptr) *error = "the file holds no S-expression";
        return false;
    }
    return true;
}

const Node* findRoot(const std::vector<Node>& roots, const std::string& head) {
    for (const Node& root : roots) {
        if (root.isList && root.head() == head) return &root;
    }
    return nullptr;
}

} // namespace sexpr
} // namespace pcbv
