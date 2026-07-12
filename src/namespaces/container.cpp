#include "container.hpp"
#include "../fs/overlay.hpp"
#include "../fs/rootfs.hpp"
#include "../network/network.hpp"
#include "../registry/registry.hpp"

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

Container::Container(std::string imageRef, std::string command, std::vector<std::string> args,
                      cgroups::Limits limits)
    : imageRef_(std::move(imageRef)),
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
    // Block until run() has finished network + cgroup setup on the
    // parent side (both need this process's PID, which the parent only
    // learns after clone() returns) — otherwise this process could exec
    // the target command before its network even exists.
    char go = 0;
    if (read(readyPipe_[0], &go, 1) != 1) {
        perror("read(readyPipe_)");
        return 1;
    }

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
    // Pulls from the local cache if already present, or Docker Hub if
    // not — see src/registry/. Layers come back ordered topmost-first,
    // ready to hand straight to Overlay as lowerdir entries.
    const registry::PulledImage image = registry::pull(imageRef_, "image-cache");

    // Own PID (this runtime process's, not the container's — that's not
    // known until after clone()) uniquely identifies this container
    // instance, so the overlay's upper/work/merged directories don't
    // collide with a concurrently-running container using the same lower
    // layer(s).
    fs::Overlay overlay(image.layerDirs, std::to_string(getpid()));
    pivotTarget_ = overlay.mergedPath();

    if (pipe(readyPipe_) != 0) {
        perror("pipe");
        std::exit(1);
    }

    void* stack = mmap(nullptr, kStackSize, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (stack == MAP_FAILED) {
        perror("mmap");
        std::exit(1);
    }
    void* stackTop = static_cast<char*>(stack) + kStackSize;

    // clone() over fork()+unshare(): a single syscall creates the child
    // already inside the new namespaces, so there's no window where the
    // child briefly shares PID/UTS/IPC/mount/net state with the parent
    // before unshare() takes effect. The overlay mounted above is already
    // part of this process's mount table, so CLONE_NEWNS's snapshot
    // carries it into the child automatically.
    const int flags =
        CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNS | CLONE_NEWNET | SIGCHLD;
    childPid_ = clone(&Container::childEntry, stackTop, flags, this);
    if (childPid_ == -1) {
        perror("clone");
        munmap(stack, kStackSize);
        std::exit(1);
    }

    // childPid_ here is the PID as seen from this (parent/host) PID
    // namespace, which is what cgroup.procs and /proc/<pid>/ns/net
    // expect — both are addressed from the writer's own namespace view,
    // not the child's.
    try {
        network::ensureBridge();
        // Host octet derived from the child's own PID: no real IPAM,
        // just enough to keep concurrently-running containers from
        // colliding on the same address (documented limitation).
        network::Veth veth(childPid_, 2 + static_cast<int>(childPid_ % 250));

        cgroups::CGroup group(std::to_string(childPid_), limits_);
        group.addProcess(childPid_);

        // Network and cgroup setup are both done — release the child
        // from its readiness barrier in childMain().
        char go = 1;
        (void)!write(readyPipe_[1], &go, 1);

        int status = 0;
        waitpid(childPid_, &status, 0);
        childPid_ = -1; // already reaped, nothing left for the destructor to do
        // veth's and group's destructors run here, now that the
        // container process (their only member/user) has exited.
    } catch (const std::exception& e) {
        fprintf(stderr, "container setup failed: %s\n", e.what());
        // Unblock the child regardless, so it can exit (with an error of
        // its own) instead of hanging forever on the readiness read().
        char go = 1;
        (void)!write(readyPipe_[1], &go, 1);
        waitpid(childPid_, nullptr, 0);
        childPid_ = -1;
    }

    close(readyPipe_[0]);
    close(readyPipe_[1]);
    munmap(stack, kStackSize);
}

} // namespace cr
