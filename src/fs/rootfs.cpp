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

// glibc does not provide a pivot_root() wrapper; call the syscall directly.
int pivotRootSyscall(const char* newRoot, const char* putOld) {
    return static_cast<int>(syscall(SYS_pivot_root, newRoot, putOld));
}

} // namespace

PivotRoot::PivotRoot(const std::string& newRoot) {
    const std::string oldRootDir = newRoot + "/.old_root";

    // Reversible setup. Nothing below has touched the process's root
    // yet, so on failure we can cleanly unwind before rethrowing — a
    // throwing constructor never gets its destructor called, so this
    // unwind has to happen here rather than in ~PivotRoot().
    bool bindMounted = false;
    try {
        // Ubuntu mounts "/" MS_SHARED by default, which propagates mount
        // events back to the host and makes pivot_root() fail with
        // EINVAL. Make the whole mount tree private in this (already
        // separate, thanks to CLONE_NEWNS) mount namespace first — the
        // same "mount --make-rprivate /" step real container runtimes do.
        if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
            throwErrno("mount(make-rprivate /)");
        }

        // (a) pivot_root() requires its target to already be a mount
        // point; bind-mounting the rootfs onto itself makes it one.
        if (mount(newRoot.c_str(), newRoot.c_str(), nullptr, MS_BIND | MS_REC, nullptr) != 0) {
            throwErrno("mount(bind self)");
        }
        bindMounted = true;

        // (b) Directory pivot_root will relocate the old root onto.
        if (mkdir(oldRootDir.c_str(), 0700) != 0 && errno != EEXIST) {
            throwErrno("mkdir(.old_root)");
        }
    } catch (...) {
        if (bindMounted) {
            umount2(newRoot.c_str(), MNT_DETACH);
        }
        throw;
    }

    // From here on, failures are not meaningfully reversible.
    // pivot_root() atomically swaps the process's root; there's no
    // well-defined way to "undo" a partially completed pivot, so a
    // failure past this point is fatal to the container rather than
    // something to roll back (real container runtimes make the same
    // tradeoff).
    // (c) Swap the new root in; the old root ends up at newRoot/.old_root.
    if (pivotRootSyscall(newRoot.c_str(), oldRootDir.c_str()) != 0) {
        throwErrno("pivot_root");
    }

    // (e) chdir into the new root — required after pivot_root so
    // relative paths (like the ones below) resolve inside it.
    if (chdir("/") != 0) {
        throwErrno("chdir(/)");
    }

    // (d) Unmount and remove the old root reference so the container has
    // no path back to the host filesystem.
    if (umount2("/.old_root", MNT_DETACH) != 0) {
        throwErrno("umount2(.old_root)");
    }
    if (rmdir("/.old_root") != 0) {
        throwErrno("rmdir(.old_root)");
    }

    // (f) Mount a fresh /proc scoped to the new PID namespace. Without
    // this, /proc still reflects whatever PID namespace was active when
    // the previous /proc was mounted (the host's) — which is why `ps`
    // inside the container showed host processes before this phase.
    if (mount("proc", "/proc", "proc", 0, nullptr) != 0) {
        throwErrno("mount(/proc)");
    }
}

} // namespace cr::fs
