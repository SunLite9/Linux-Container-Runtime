#include "rootfs.hpp"

#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>

namespace cr::fs {

namespace {

void throwErrno(const std::string& what) {
    throw std::system_error(errno, std::generic_category(), what);
}

// glibc has no pivot_root() wrapper.
int pivotRootSyscall(const char* newRoot, const char* putOld) {
    return static_cast<int>(syscall(SYS_pivot_root, newRoot, putOld));
}

} // namespace

PivotRoot::PivotRoot(const std::string& newRoot) {
    const std::string oldRootDir = newRoot + "/.old_root";

    bool bindMounted = false;
    try {
        // Ubuntu mounts "/" MS_SHARED by default, which makes pivot_root()
        // fail with EINVAL unless the tree is made private first.
        if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
            throwErrno("mount(make-rprivate /)");
        }

        if (mount(newRoot.c_str(), newRoot.c_str(), nullptr, MS_BIND | MS_REC, nullptr) != 0) {
            throwErrno("mount(bind self)");
        }
        bindMounted = true;

        if (mkdir(oldRootDir.c_str(), 0700) != 0 && errno != EEXIST) {
            throwErrno("mkdir(.old_root)");
        }
    } catch (...) {
        if (bindMounted) {
            umount2(newRoot.c_str(), MNT_DETACH);
        }
        throw;
    }

    if (pivotRootSyscall(newRoot.c_str(), oldRootDir.c_str()) != 0) {
        throwErrno("pivot_root");
    }
    if (chdir("/") != 0) {
        throwErrno("chdir(/)");
    }
    if (umount2("/.old_root", MNT_DETACH) != 0) {
        throwErrno("umount2(.old_root)");
    }
    if (rmdir("/.old_root") != 0) {
        throwErrno("rmdir(.old_root)");
    }
    if (mount("proc", "/proc", "proc", 0, nullptr) != 0) {
        throwErrno("mount(/proc)");
    }
}

} // namespace cr::fs
