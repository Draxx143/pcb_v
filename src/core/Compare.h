// Compare.h — diff two documents (a revision against the previous one).
#pragma once

#include <string>
#include <vector>

#include "Model.h"

namespace pcbv {

/// One field that differs between the two revisions of a component.
struct FieldChange {
    std::string field;   ///< "Comment", "Footprint", "Position", …
    std::string before;
    std::string after;
};

/// A component that exists in both documents but is not identical.
struct ChangedPart {
    std::string designator;
    std::string label;    ///< value/comment, so the row is recognisable
    std::vector<FieldChange> changes;
};

/// Result of comparing a document against a newer (or older) revision of it.
struct CompareResult {
    std::string leftName;    ///< the document already open
    std::string rightName;   ///< the document it was compared with
    std::size_t leftCount = 0;
    std::size_t rightCount = 0;

    std::vector<Component> added;     ///< present only in the right document
    std::vector<Component> removed;   ///< present only in the left document
    std::vector<ChangedPart> changed;
    std::vector<std::string> netsAdded;    ///< PCB documents only
    std::vector<std::string> netsRemoved;

    bool identical() const {
        return added.empty() && removed.empty() && changed.empty() && netsAdded.empty() &&
               netsRemoved.empty();
    }
    std::size_t differenceCount() const {
        return added.size() + removed.size() + changed.size() + netsAdded.size() +
               netsRemoved.size();
    }
};

/// Matches components by designator and reports added / removed / changed parts plus
/// the nets that came or went. Works for boards and schematics alike.
CompareResult compareDocuments(const Document& left, const Document& right);

/// Plain text report (CLI).
std::string compareToText(const CompareResult& result);

/// Self contained HTML report (GUI tab, printable / savable).
std::string compareToHtml(const CompareResult& result);

} // namespace pcbv
