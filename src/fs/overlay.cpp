#include "overlay.hpp"

#include <sys/mount.h>

#include <cerrno>
#include <filesystem>
#include <system_error>

namespace cr::fs {

namespace {

namespace stdfs = std::filesystem;

constexpr const char* kOverlayBase = "overlay-data";

std::string joinLowerDirs(const std::vector<std::string>& lowerDirs) {
    std::string joined;
    for (size_t i = 0; i < lowerDirs.size(); ++i) {
        if (i != 0) {
            joined += ':';
        }
        joined += stdfs::absolute(lowerDirs[i]).string();
    }
    return joined;
}

} // namespace

Overlay::Overlay(std::vector<std::string> lowerDirs, std::string containerId)
    : containerDir_(std::string(kOverlayBase) + "/" + containerId),
      upper_(containerDir_ + "/upper"),
      work_(containerDir_ + "/work"),
      merged_(containerDir_ + "/merged") {
    stdfs::create_directories(upper_);
    stdfs::create_directories(work_);
    stdfs::create_directories(merged_);

    const std::string options = "lowerdir=" + joinLowerDirs(lowerDirs) +
                                 ",upperdir=" + stdfs::absolute(upper_).string() +
                                 ",workdir=" + stdfs::absolute(work_).string();

    if (mount("overlay", merged_.c_str(), "overlay", 0, options.c_str()) != 0) {
        throw std::system_error(errno, std::generic_category(), "mount(overlay): " + options);
    }
    mounted_ = true;
}

Overlay::~Overlay() {
    if (mounted_) {
        umount2(merged_.c_str(), MNT_DETACH);
    }
    std::error_code ec;
    stdfs::remove_all(upper_, ec);
    stdfs::remove_all(work_, ec);
    stdfs::remove_all(merged_, ec);
    stdfs::remove(containerDir_, ec);
}

} // namespace cr::fs
