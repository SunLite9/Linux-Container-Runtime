#include "cgroups.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <fstream>
#include <system_error>

namespace cr::cgroups {

namespace {

constexpr const char* kBaseGroup = "/sys/fs/cgroup/container-runtime";

void writeFile(const std::string& path, const std::string& content) {
    std::ofstream ofs(path);
    if (!ofs) {
        throw std::system_error(errno, std::generic_category(), "open " + path);
    }
    ofs << content;
    ofs.flush();
    if (!ofs) {
        throw std::system_error(errno, std::generic_category(), "write " + path);
    }
}

void ensureDir(const std::string& path) {
    if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
        throw std::system_error(errno, std::generic_category(), "mkdir " + path);
    }
}

// Enables `controllers` (e.g. "+cpu +memory") in `group`'s subtree_control
// so its child cgroups can themselves set cpu.max/memory.max. Idempotent:
// enabling an already-enabled controller is a harmless no-op to the kernel.
void enableControllers(const std::string& group, const std::string& controllers) {
    writeFile(group + "/cgroup.subtree_control", controllers);
}

} // namespace

CGroup::CGroup(std::string containerId, const Limits& limits)
    : path_(std::string(kBaseGroup) + "/" + containerId) {
    // Base hierarchy shared across all containers this runtime creates.
    // cpu/memory are already enabled in the root cgroup's subtree_control
    // on this host, so this directory inherits them automatically; we
    // still need to enable them in *its* subtree_control so our
    // per-container child directories (below) can use them.
    ensureDir(kBaseGroup);
    enableControllers(kBaseGroup, "+cpu +memory");

    ensureDir(path_);

    // cpu.max is "<quota> <period>" in microseconds: quota microseconds
    // of CPU time allowed per period. cpuCores * period gives the quota
    // for a fraction of one core.
    constexpr long kPeriodUs = 100000;
    const long quotaUs = static_cast<long>(limits.cpuCores * kPeriodUs);
    writeFile(path_ + "/cpu.max", std::to_string(quotaUs) + " " + std::to_string(kPeriodUs));

    // memory.max is a hard cap in bytes; the kernel OOM-kills processes in
    // this cgroup that try to exceed it, rather than letting them consume
    // unbounded host memory.
    const long memoryBytes = limits.memoryMb * 1024L * 1024L;
    writeFile(path_ + "/memory.max", std::to_string(memoryBytes));
}

CGroup::~CGroup() {
    // Removable only once empty of member processes; by the time this
    // destructs, the container's process has already been waitpid()'d in
    // Container::run(), so it should be empty. Not fatal to the caller if
    // this fails (the container has already finished running) — just
    // report it rather than throwing from a destructor.
    if (rmdir(path_.c_str()) != 0) {
        perror(("rmdir " + path_).c_str());
    }
}

void CGroup::addProcess(pid_t pid) const {
    writeFile(path_ + "/cgroup.procs", std::to_string(pid));
}

} // namespace cr::cgroups
