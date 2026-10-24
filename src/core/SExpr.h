// SExpr.h — a small, dependency-free reader for S-expression files.
//
// KiCad stores its footprints (.kicad_mod), symbols (.kicad_sym) and boards (.kicad_pcb)
// as S-expressions: a list is "(head arg arg …)" and an atom is a bare word, a quoted
// string or a number. This reader turns that text into a tree of `Node`s; it knows nothing
// about KiCad itself, so the same tree can serve any other Lisp-shaped format.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace pcbv {
namespace sexpr {

/// One atom or one list of a document.
struct Node {
    bool isList = false;
    std::string atom;            ///< set when `isList` is false (quotes are removed)
    std::vector<Node> children;  ///< set when `isList` is true

    /// The first atom of a list ("pad", "at", …), empty for an atom or an empty list.
    const std::string& head() const;
    /// First atom after the head, e.g. the designator of `(pad "1" …)`.
    std::string first() const;

    /// First list child whose head is `key` (nullptr when there is none).
    const Node* child(const std::string& key) const;
    /// Every list child whose head is `key`, in file order.
    std::vector<const Node*> childrenNamed(const std::string& key) const;
    /// All direct children that are atoms (the arguments of a list).
    std::vector<std::string> atoms() const;

    /// Child list named `key`, or nullptr.
    const Node* find(const std::string& key) const { return child(key); }

    /// Numbers of a child list, e.g. `at` of `(at 1.5 2 -90)`: number("at", 0) = 1.5.
    double number(const std::string& key, std::size_t index, double fallback = 0.0) const;
    /// Text of a child list, e.g. text("layer") of `(layer "F.Cu")` = "F.Cu".
    std::string text(const std::string& key, std::size_t index = 0,
                     const std::string& fallback = std::string()) const;

    /// Parse an atom as a number; returns `fallback` when it is not numeric.
    double asNumber(double fallback = 0.0) const;
};

/// Parses `text` into `roots`. Returns false (with a reason) on unbalanced parentheses.
bool parse(const std::string& text, std::vector<Node>& roots, std::string* error = nullptr);

/// Convenience: the first root list whose head is `head` (nullptr when absent).
const Node* findRoot(const std::vector<Node>& roots, const std::string& head);

} // namespace sexpr
} // namespace pcbv
