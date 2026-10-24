// Recent.h — the "last opened files" list, kept out of the GUI so it can be tested.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace pcbv {

/// Puts `path` at the front of `list`, removes duplicates and trims the list to
/// `maxItems` entries. An empty path leaves the list untouched.
std::vector<std::string> pushRecent(std::vector<std::string> list, const std::string& path,
                                    std::size_t maxItems = 10);

}  // namespace pcbv
