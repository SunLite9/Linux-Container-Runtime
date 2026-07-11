#pragma once

#include <string>

namespace cr::fs {

// Performs the pivot_root dance to make `newRoot` the process's root
// filesystem in the current mount namespace, then mounts a fresh /proc
// so tools like `ps` reflect the container's own PID namespace instead
// of the host's.
//
// Must run inside a process that already has CLONE_NEWNS in effect (see
// cr::Container), before exec'ing the container's command.
//
// RAII note: a throwing constructor's destructor is never invoked by the
// language, so any cleanup-on-partial-failure has to live inside the
// constructor itself (see the .cpp), not in ~PivotRoot(). On success,
// ~PivotRoot() is deliberately a no-op: the whole point of pivoting is
// for the new root and /proc mount to persist across the exec() that
// follows construction, and by the time this object would otherwise go
// out of scope, exec() has already replaced the process image.
class PivotRoot {
public:
    explicit PivotRoot(const std::string& newRoot);
    ~PivotRoot() = default;

    PivotRoot(const PivotRoot&) = delete;
    PivotRoot& operator=(const PivotRoot&) = delete;
};

} // namespace cr::fs
