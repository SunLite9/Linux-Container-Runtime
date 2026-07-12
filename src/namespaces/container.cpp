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
constexpr size_t kStackSize = 1024 * 1024;
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
    // Blocks until run() finishes network/cgroup setup, which needs this
    // process's PID and only happens after clone() returns.
    char go = 0;
    if (read(readyPipe_[0], &go, 1) != 1) {
        perror("read(readyPipe_)");
        return 1;
    }

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
    perror("execvp");
    return 1;
}

void Container::run() {
    const registry::PulledImage image = registry::pull(imageRef_, "image-cache");

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

    const int flags =
        CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNS | CLONE_NEWNET | SIGCHLD;
    childPid_ = clone(&Container::childEntry, stackTop, flags, this);
    if (childPid_ == -1) {
        perror("clone");
        munmap(stack, kStackSize);
        std::exit(1);
    }

    try {
        network::ensureBridge();
        network::Veth veth(childPid_, 2 + static_cast<int>(childPid_ % 250));

        cgroups::CGroup group(std::to_string(childPid_), limits_);
        group.addProcess(childPid_);

        char go = 1;
        (void)!write(readyPipe_[1], &go, 1);

        int status = 0;
        waitpid(childPid_, &status, 0);
        childPid_ = -1;
    } catch (const std::exception& e) {
        fprintf(stderr, "container setup failed: %s\n", e.what());
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
