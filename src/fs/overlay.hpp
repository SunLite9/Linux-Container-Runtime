#pragma once

#include <string>
#include <vector>

namespace cr::fs {

// Mounts an OverlayFS combining one or more read-only lower layers (image
// layers) with a fresh writable upper layer, at a per-container merged
// directory under overlay-data/<containerId>/.
//
// Mounted in the calling process's own mount namespace *before* the
// container's clone(CLONE_NEWNS, ...) — clone() takes a snapshot of the
// current mount table for the child, so the container inherits this
// overlay as an already-mounted point with no extra plumbing needed to
// get it into the container's namespace. PivotRoot then pivots into
// mergedPath() instead of a single flat rootfs directory.
//
// RAII: unmounts and removes the upper/work/merged directories on
// destruction (once the container process using them has exited). Lower
// layers are never touched — that's what lets multiple containers share
// them on disk without duplication.
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
