#pragma once

#include <string>
#include <sys/types.h>

namespace cr::cgroups {

struct Limits {
    // Fraction of one CPU core, e.g. 0.5 == half a core.
    double cpuCores = 0.5;
    long memoryMb = 100;
};

// Owns one cgroup v2 group (/sys/fs/cgroup/container-runtime/<containerId>)
// enforcing cpu.max / memory.max for a single container's process tree.
//
// RAII: the cgroup directory is created in the constructor and removed in
// the destructor, so a container's resource limits are cleaned up as soon
// as the CGroup object goes out of scope — no manual teardown call needed
// at the call site, and it still happens if an exception unwinds through
// here after construction.
class CGroup {
public:
    CGroup(std::string containerId, const Limits& limits);
    ~CGroup();

    CGroup(const CGroup&) = delete;
    CGroup& operator=(const CGroup&) = delete;

    // Adds pid to this cgroup (writes to cgroup.procs), applying the
    // configured limits to it and all of its children.
    void addProcess(pid_t pid) const;

private:
    std::string path_;
};

} // namespace cr::cgroups
