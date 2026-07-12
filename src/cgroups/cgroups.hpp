#pragma once

#include <string>
#include <sys/types.h>

namespace cr::cgroups {

struct Limits {
    double cpuCores = 0.5; // fraction of one core
    long memoryMb = 100;
    long pidsMax = 128;
};

// Owns one cgroup v2 group at /sys/fs/cgroup/container-runtime/<containerId>
// enforcing cpu.max / memory.max / pids.max for a container's process tree.
class CGroup {
public:
    CGroup(std::string containerId, const Limits& limits);
    ~CGroup();

    CGroup(const CGroup&) = delete;
    CGroup& operator=(const CGroup&) = delete;

    void addProcess(pid_t pid) const;

private:
    std::string path_;
};

} // namespace cr::cgroups
