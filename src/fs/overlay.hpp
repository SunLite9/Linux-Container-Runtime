#pragma once

#include <string>
#include <vector>

namespace cr::fs {

// Mounts an OverlayFS combining read-only lower layers with a fresh
// writable upper layer, at overlay-data/<containerId>/merged. Must be
// mounted before clone(CLONE_NEWNS, ...) so the container inherits it.
class Overlay {
public:
    Overlay(std::vector<std::string> lowerDirs, std::string containerId);
    ~Overlay();

    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    const std::string& mergedPath() const { return merged_; }

private:
    std::string containerDir_;
    std::string upper_;
    std::string work_;
    std::string merged_;
    bool mounted_ = false;
};

} // namespace cr::fs
