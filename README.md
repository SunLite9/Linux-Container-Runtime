# container-runtime

A minimal Docker-like container runtime built directly on raw Linux kernel
primitives (namespaces, cgroups v2, OverlayFS, veth networking) in C++20 —
no container framework libraries. Built in phases; this README grows with
each phase.

## Requirements

- A real Linux host (not macOS/WSL) — this project calls Linux-specific
  syscalls (`clone`, `unshare`, `mount`, `pivot_root`) directly.
- root/sudo — namespace, mount, and (in later phases) cgroup syscalls
  require elevated privileges.
- A C++20 compiler (g++ or clang++) and CMake.

## Building

```
mkdir build && cd build
cmake ..
make
```

## Usage (Phase 1: namespace isolation)

```
sudo ./build/container-runtime run /bin/sh
```

This drops you into a shell that is isolated from the host via four
namespaces. Default command is `/bin/sh` if none is given:
`sudo ./build/container-runtime run` also works.

## Project layout

```
src/main.cpp        entrypoint, CLI parsing
src/namespaces/      namespace isolation (this phase)
src/cgroups/         resource limits (later phase)
src/fs/              pivot_root / OverlayFS (later phase)
src/network/         veth + bridge + NAT (later phase)
src/registry/        Docker Hub registry client (later phase)
include/             shared headers (later phases)
```

## Phase 1: Namespaces

`clone()` (from `<sched.h>`) is used instead of `fork()` + `unshare()`.
`clone()` creates the child already inside the new namespaces in a single
syscall, so there is no window where the child briefly shares PID/UTS/IPC/
mount state with the parent before a separate `unshare()` call takes
effect — a smaller, more auditable isolation boundary.

Namespace flags used, and what each isolates:

- **`CLONE_NEWPID`** — the child gets its own PID namespace. It becomes PID 1
  inside the namespace and can only see its own descendant processes, not
  the host's full process table. This is what makes `ps aux` inside the
  container show just the container's processes.
- **`CLONE_NEWUTS`** — the child gets its own hostname/domainname
  namespace, so `sethostname()` inside the container does not affect the
  host's hostname (and vice versa).
- **`CLONE_NEWIPC`** — the child gets its own System V IPC / POSIX message
  queue namespace, so IPC objects (shared memory segments, semaphores,
  message queues) created inside the container are invisible to the host
  and other containers.
- **`CLONE_NEWNS`** — the child gets its own mount namespace, so mount/
  unmount operations inside the container (used heavily starting in Phase
  2 for `pivot_root` and OverlayFS) do not affect the host's mount table.

`Container` (in `src/namespaces/container.hpp`/`.cpp`) wraps this: it owns
the cloned child's PID and, in its destructor, reaps the child if `run()`
didn't already do so — the RAII pattern later phases (`cgroups`,
`fs::Overlay`, `network::Veth`) follow so that resource cleanup happens on
scope exit rather than via manually-scattered cleanup calls.

### Verification

Run on a real Linux host with sudo:

```
sudo ./build/container-runtime run
```

Actual output, run on the project's EC2 instance:

```
$ echo 'echo PID inside namespace: $$; hostname' | sudo ./build/container-runtime run /bin/sh
PID inside namespace: 1
container

$ hostname   # on the host, in a separate shell
ip-172-31-9-167
```

- `echo $$` inside the container prints `1` — the shell is PID 1 in its own
  PID namespace, proof of `CLONE_NEWPID` isolation.
- `hostname` inside the container prints `container`, while the host's own
  `hostname` is unchanged (`ip-172-31-9-167`) — proof of `CLONE_NEWUTS`
  isolation.

**Known gap in this phase:** `ps aux` run inside the container still shows
the host's full process list, not just the container's own tree. This is
expected: `ps` reads `/proc`, and the container is still using the host's
existing `/proc` mount, which reflects the PID namespace active when it was
originally mounted (the host's, at boot) rather than the container's new
one. Fixing this requires mounting a fresh `procfs` *inside* the new mount
namespace — that's what Phase 2 (`pivot_root` + mounting `/proc`) does. The
`$$` result above is the reliable way to prove PID namespace isolation
until then.
