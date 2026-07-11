#pragma once

#include <string>
#include <vector>
#include <sys/types.h>

#include "../cgroups/cgroups.hpp"

namespace cr {

// Runs a command inside new PID, UTS, IPC, and mount namespaces.
//
// RAII note: the clone()'d child is synchronously waited on inside run(),
// so there is no long-lived kernel resource for the destructor to release
// yet. The destructor still reaps the child if run() was never called to
// completion (e.g. an exception unwinds through here), which is the pattern
// later phases (cgroups, overlay mounts, network namespaces) build on: each
// resource-owning class cleans itself up on destruction, not via manual
// cleanup calls scattered through main().
class Container {
public:
    Container(std::string rootfsPath, std::string command, std::vector<std::string> args,
              cgroups::Limits limits = {});
    ~Container();

    Container(const Container&) = delete;
    Container& operator=(const Container&) = delete;

    // Mounts an overlay over rootfsPath, clones the isolated child, pivots
    // it into the merged overlay directory, execs the command inside it,
    // and blocks until it exits.
    void run();

private:
    static int childEntry(void* arg);
    int childMain();

    std::string rootfsPath_;
    std::string command_;
    std::vector<std::string> args_;
    cgroups::Limits limits_;
    pid_t childPid_ = -1;
    // Set by run() before clone(), to the overlay's merged directory;
    // childMain() pivot_roots into this rather than rootfsPath_ directly.
    std::string pivotTarget_;
};

} // namespace cr
