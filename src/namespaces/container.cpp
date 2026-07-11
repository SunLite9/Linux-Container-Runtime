#include "container.hpp"
#include "../fs/overlay.hpp"
#include "../fs/rootfs.hpp"

#include <sched.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

namespace cr {

namespace {
// clone() needs its own stack for the child since it does not copy the
// parent's like fork() does.
constexpr size_t kStackSize = 1024 * 1024; // 1 MB
} // namespace

Container::Container(std::string rootfsPath, std::string command, std::vector<std::string> args,
                      cgroups::Limits limits)
    : rootfsPath_(std::move(rootfsPath)),
      command_(std::move(command)),
      args_(std::move(args)),
      limits_(limits) {}

Container::~Container() {
    if (childPid_ > 0) {
        int status = 0;
        waitpid(childPid_, &status, 0);
    }
}

int Container::childEntry(void* arg) {
    auto* self = static_cast<Container*>(arg);
    return self->childMain();
}

int Container::childMain() {
    // Proves UTS namespace isolation: this hostname is not visible on the host.
    if (sethostname("container", 9) != 0) {
        perror("sethostname");
        return 1;
    }

    try {
        fs::PivotRoot pivot(pivotTarget_);
    } catch (const std::exception& e) {
        fprintf(stderr, "pivot_root into %s failed: %s\n", pivotTarget_.c_str(), e.what());
        return 1;
    }

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(command_.c_str()));
    for (auto& a : args_) {
        argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);

    execvp(command_.c_str(), argv.data());
    // Only reached if execvp failed.
    perror("execvp");
    return 1;
}

void Container::run() {
    // Own PID (this runtime process's, not the container's — that's not
    // known until after clone()) uniquely identifies this container
    // instance, so the overlay's upper/work/merged directories don't
    // collide with a concurrently-running container using the same lower
    // layer.
    fs::Overlay overlay({rootfsPath_}, std::to_string(getpid()));
    pivotTarget_ = overlay.mergedPath();

    void* stack = mmap(nullptr, kStackSize, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (stack == MAP_FAILED) {
        perror("mmap");
        std::exit(1);
    }
    void* stackTop = static_cast<char*>(stack) + kStackSize;

    // clone() over fork()+unshare(): a single syscall creates the child
    // already inside the new namespaces, so there's no window where the
    // child briefly shares PID/UTS/IPC/mount state with the parent before
    // unshare() takes effect. The overlay mounted above is already part
    // of this process's mount table, so CLONE_NEWNS's snapshot carries it
    // into the child automatically.
    const int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNS | SIGCHLD;
    childPid_ = clone(&Container::childEntry, stackTop, flags, this);
    if (childPid_ == -1) {
        perror("clone");
        munmap(stack, kStackSize);
        std::exit(1);
    }

    // childPid_ here is the PID as seen from this (parent/host) PID
    // namespace, which is what cgroup.procs expects — cgroup membership
    // is written from the writer's own namespace view, not the child's.
    try {
        cgroups::CGroup group(std::to_string(childPid_), limits_);
        group.addProcess(childPid_);

        int status = 0;
        waitpid(childPid_, &status, 0);
        childPid_ = -1; // already reaped, nothing left for the destructor to do
        // group's destructor removes the cgroup directory here, now that
        // the container process (its only member) has exited.
    } catch (const std::exception& e) {
        fprintf(stderr, "cgroup setup failed: %s\n", e.what());
        waitpid(childPid_, nullptr, 0);
        childPid_ = -1;
    }

    munmap(stack, kStackSize);
}

} // namespace cr
