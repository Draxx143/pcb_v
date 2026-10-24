#include "Recent.h"

#include <algorithm>

namespace pcbv {

std::vector<std::string> pushRecent(std::vector<std::string> list, const std::string& path,
                                    std::size_t maxItems) {
    if (path.empty()) return list;
    list.erase(std::remove(list.begin(), list.end(), path), list.end());
    list.insert(list.begin(), path);
    if (maxItems > 0 && list.size() > maxItems) list.resize(maxItems);
    return list;
}

}  // namespace pcbv
