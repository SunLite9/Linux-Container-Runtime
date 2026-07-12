#pragma once

#include <string>

namespace cr::fs {

// Pivots the process's root filesystem to newRoot and mounts a fresh
// /proc. Must run in a process that already has CLONE_NEWNS in effect.
class PivotRoot {
public:
    explicit PivotRoot(const std::string& newRoot);
    ~PivotRoot() = default;

    PivotRoot(const PivotRoot&) = delete;
    PivotRoot& operator=(const PivotRoot&) = delete;
};

} // namespace cr::fs
