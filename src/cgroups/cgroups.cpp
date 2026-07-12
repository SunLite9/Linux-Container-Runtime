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

void enableControllers(const std::string& group, const std::string& controllers) {
    writeFile(group + "/cgroup.subtree_control", controllers);
}

} // namespace

CGroup::CGroup(std::string containerId, const Limits& limits)
    : path_(std::string(kBaseGroup) + "/" + containerId) {
    ensureDir(kBaseGroup);
    enableControllers(kBaseGroup, "+cpu +memory +pids");
    ensureDir(path_);

    constexpr long kPeriodUs = 100000;
    const long quotaUs = static_cast<long>(limits.cpuCores * kPeriodUs);
    writeFile(path_ + "/cpu.max", std::to_string(quotaUs) + " " + std::to_string(kPeriodUs));

    const long memoryBytes = limits.memoryMb * 1024L * 1024L;
    writeFile(path_ + "/memory.max", std::to_string(memoryBytes));

    writeFile(path_ + "/pids.max", std::to_string(limits.pidsMax));
}

CGroup::~CGroup() {
    if (rmdir(path_.c_str()) != 0) {
        perror(("rmdir " + path_).c_str());
    }
}

void CGroup::addProcess(pid_t pid) const {
    writeFile(path_ + "/cgroup.procs", std::to_string(pid));
}

} // namespace cr::cgroups
