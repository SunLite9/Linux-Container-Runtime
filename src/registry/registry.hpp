#pragma once

#include <string>
#include <vector>

namespace cr::registry {

// Layers extracted from a pulled image, ordered for direct use as
// OverlayFS lowerdir entries: topmost (most recently applied) layer
// first — the reverse of the manifest's base-to-top layer order.
struct PulledImage {
    std::vector<std::string> layerDirs;
};

// Pulls `imageRef` (e.g. "alpine:latest", "python:3.11-slim") from Docker
// Hub's public registry API into `cacheDir`, extracting each layer into
// its own subdirectory. Idempotent: if a layer's directory already
// exists and is non-empty, it's reused rather than re-downloaded, so
// repeated `run`s of the same image are fast after the first pull.
PulledImage pull(const std::string& imageRef, const std::string& cacheDir);

} // namespace cr::registry
