#include "container.hpp"
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

Container::Container(std::string rootfsPath, std::string command, std::vector<std::string> args)
    : rootfsPath_(std::move(rootfsPath)), command_(std::move(command)), args_(std::move(args)) {}

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
        fs::PivotRoot pivot(rootfsPath_);
    } catch (const std::exception& e) {
        fprintf(stderr, "pivot_root into %s failed: %s\n", rootfsPath_.c_str(), e.what());
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
    // unshare() takes effect.
    const int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNS | SIGCHLD;
    childPid_ = clone(&Container::childEntry, stackTop, flags, this);
    if (childPid_ == -1) {
        perror("clone");
        munmap(stack, kStackSize);
        std::exit(1);
    }

    int status = 0;
    waitpid(childPid_, &status, 0);
    childPid_ = -1; // already reaped, nothing left for the destructor to do
    munmap(stack, kStackSize);
}

} // namespace cr
