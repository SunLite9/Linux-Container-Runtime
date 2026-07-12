#pragma once

#include <string>
#include <vector>

namespace cr::registry {

// Layers ordered topmost-first, ready for direct use as OverlayFS
// lowerdir entries.
struct PulledImage {
    std::vector<std::string> layerDirs;
};

// Pulls imageRef (e.g. "alpine:latest") from Docker Hub into cacheDir,
// extracting each layer into its own subdirectory. Cached layers are
// reused rather than re-downloaded.
PulledImage pull(const std::string& imageRef, const std::string& cacheDir);

} // namespace cr::registry
