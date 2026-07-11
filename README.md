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

## Usage

Run once, from the project root, to fetch the Alpine root filesystem:

```
./scripts/fetch-rootfs.sh
```

Then:

```
sudo ./build/container-runtime run [--cpu-limit N] [--memory-limit MB] [command] [args...]
```

- `--cpu-limit N` — fraction of one CPU core, e.g. `0.5` (default `0.5`).
- `--memory-limit MB` — memory cap in megabytes (default `100`).
- `command` — defaults to `/bin/sh` if omitted.

Must be run from the project root, so the hardcoded `rootfs/alpine` path
resolves correctly (Task 6 replaces this with a registry-pulled image path).

Examples:

```
sudo ./build/container-runtime run
sudo ./build/container-runtime run --cpu-limit 0.2 --memory-limit 50 /bin/sh
```

This drops you into a shell that is isolated via four namespaces, pivoted
into its own Alpine root filesystem, and capped by cgroup v2 CPU/memory
limits.

## Project layout

```
src/main.cpp        entrypoint, CLI parsing
src/namespaces/      namespace isolation (Phase 1)
src/cgroups/         cgroup v2 CPU/memory limits (Phase 3)
src/fs/              pivot_root (Phase 2) / OverlayFS (later phase)
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
until then. **Update: fixed in Phase 2 below** — mounting a fresh `/proc`
after `pivot_root` makes `ps aux` correct too.

## Phase 2: Root filesystem via pivot_root

### Getting a rootfs

`scripts/fetch-rootfs.sh` downloads the official Alpine Linux minirootfs
tarball directly from the Alpine CDN (no `docker` install required) and
extracts it to `rootfs/alpine`:

```
./scripts/fetch-rootfs.sh
```

This will be replaced by our own registry puller in Task 6, which fetches
arbitrary images (not just Alpine) from Docker Hub.

### `pivot_root`, not `chroot`

`src/fs/rootfs.cpp` implements `cr::fs::PivotRoot`, which swaps the
process's root filesystem to the extracted rootfs using `pivot_root`
rather than `chroot`. `chroot` only changes what path `/` resolves to for
the calling process — it does not change the process's actual root mount,
so a process with enough privilege (or a mount namespace escape) can often
climb back out via file descriptors opened before the `chroot`, or via
`mount` tricks, making it a filesystem convenience rather than a real
security boundary. `pivot_root`, combined with a private mount namespace
(`CLONE_NEWNS` from Phase 1), actually replaces the process's root mount
point and lets the old root be unmounted and unlinked entirely, closing
that escape route.

The steps, in `PivotRoot`'s constructor:

1. **Make the mount tree private.** Ubuntu mounts `/` `MS_SHARED` by
   default, which propagates mount/unmount events between the host and any
   namespace derived from it — and causes `pivot_root` to fail with
   `EINVAL`. `mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr)`
   (the `mount --make-rprivate /` most container runtimes run) fixes this,
   scoped only to this process's own mount namespace — it does not affect
   the host (verified below).
2. **Bind-mount the rootfs onto itself** (`MS_BIND | MS_REC`) — required
   because `pivot_root`'s target must already be a mount point, and a
   plain directory isn't one.
3. **Create `.old_root`** inside the new root — `pivot_root` needs
   somewhere to relocate the previous root to.
4. **`pivot_root(newRoot, oldRootDir)`** via `syscall(SYS_pivot_root, ...)`
   (glibc doesn't wrap this one) — atomically swaps the root mount.
5. **`chdir("/")`**, then **unmount and remove `/.old_root`** — severs the
   container's path back to the host filesystem entirely.
6. **Mount a fresh `/proc`** — scoped to the new PID namespace, fixing the
   Phase 1 `ps aux` gap noted above.

**RAII note:** a C++ constructor that throws never has its own destructor
invoked — so cleanup-on-partial-failure has to happen inside the
constructor itself (see the `catch` block in `rootfs.cpp`), not in
`~PivotRoot()`. Only the first two steps above are treated as reversible
(a bind mount and a directory, both trivially undone); everything from
`pivot_root` onward is irreversible in practice, so a failure there is
fatal to the container rather than something to roll back — the same
tradeoff real container runtimes make. On success, `~PivotRoot()` is
deliberately a no-op: the new root and `/proc` mount are meant to persist
across the `exec()` that immediately follows construction, not be torn
down when the object's scope ends.

### Verification

Actual output, run on the project's EC2 instance:

```
$ sudo ./build/container-runtime run /bin/sh
# inside the container:
/ $ ls /
bin  dev  etc  home  lib  media  mnt  opt  proc  root  run  sbin  srv  sys  tmp  usr  var
/ $ ls /.old_root
ls: /.old_root: No such file or directory
/ $ cat /etc/os-release
NAME="Alpine Linux"
ID=alpine
VERSION_ID=3.24.1
PRETTY_NAME="Alpine Linux v3.24"
/ $ ps aux
PID   USER     TIME  COMMAND
    1 root      0:00 /bin/sh
    4 root      0:00 ps aux
```

```
# on the host, in a separate shell, immediately after:
$ ls /
bin  boot  dev  etc  home  lib  lib64  lost+found  media  mnt  opt  proc  root  run  sbin  snap  srv  sys  tmp  usr  var
$ cat /etc/os-release
PRETTY_NAME="Ubuntu 26.04 LTS"
...
```

- `ls /` inside the container shows the Alpine filesystem, not the host's
  (confirmed further by `/etc/os-release` reporting Alpine, not Ubuntu).
- `/.old_root` does not exist — there is no path back to the host
  filesystem from inside the container.
- `ps aux` now correctly shows only the container's own process tree
  (`/bin/sh` as PID 1 and `ps aux` itself) — the fresh `/proc` mount fixed
  the Phase 1 gap.
- The host's own `/`, `/etc/os-release`, and process list are completely
  unaffected — the `MS_REC | MS_PRIVATE` remount and the `pivot_root` are
  fully contained within the container's own mount and PID namespaces.

## Phase 3: cgroups v2 resource limits

### Design

`cr::cgroups::CGroup` (`src/cgroups/cgroups.cpp`/`.hpp`) owns one cgroup
v2 group per container, at `/sys/fs/cgroup/container-runtime/<pid>`
(`<pid>` — the container's PID as seen in the host/parent's PID namespace,
since cgroup membership is written from the writer's own namespace view —
doubles as a unique, self-cleaning container ID).

On this host, `cpu` and `memory` were already enabled in the root cgroup's
`cgroup.subtree_control` (visible via `cat /sys/fs/cgroup/cgroup.subtree_control`
→ `cpu memory pids`), so `container-runtime/` inherits them automatically.
The constructor still has to enable them in `container-runtime/`'s *own*
`subtree_control` so each per-container child directory underneath it can
use them — a cgroup only gets access to a controller if its parent
explicitly delegates it, all the way up the tree.

Construction, in order:

1. `mkdir -p /sys/fs/cgroup/container-runtime` (idempotent).
2. Write `+cpu +memory` to its `cgroup.subtree_control` (also idempotent —
   re-enabling an already-enabled controller is a no-op to the kernel).
3. `mkdir /sys/fs/cgroup/container-runtime/<pid>` for this container.
4. Write `cpu.max` as `"<quota_us> <period_us>"` — e.g. `--cpu-limit 0.5`
   with the standard 100ms period becomes `"50000 100000"`: 50ms of CPU
   time allowed per 100ms period, i.e. half a core.
5. Write `memory.max` as a raw byte count — `--memory-limit 100` becomes
   `104857600`.

`addProcess(pid)` writes the container's PID to `cgroup.procs`, called
from `Container::run()` right after `clone()` returns (before `waitpid`),
applying both limits to the container process (and anything it forks)
from the start.

**RAII cleanup:** `~CGroup()` calls `rmdir()` on the container's cgroup
directory. This is only called after `Container::run()`'s `waitpid()`
returns — cgroup v2 requires a group to be empty of member processes
before it can be removed, and by the time the C++ destructor runs, the
container's process has already exited and been reaped, so the directory
is guaranteed empty. One real limitation, observed while testing: this
only works if the *container-runtime process itself* exits through normal
control flow. If something sends `SIGKILL` directly to `container-runtime`
(not to the containerized process), no C++ destructor runs at all — a
stray cgroup directory can be left behind, requiring manual `rmdir`. This
is an inherent limit of process-lifetime-scoped RAII, not something
specific to this implementation: a killed process cannot run any of its
own cleanup code, C++ or otherwise. Real container runtimes handle this
with a separate reconciliation/garbage-collection pass; out of scope here.

### CLI

`--cpu-limit N` (fraction of a core, default `0.5`) and `--memory-limit MB`
(default `100`) are parsed in `main.cpp` with simple hand-rolled argument
parsing — no dependency was worth adding for two optional flags.

### Verification — memory limit

Compiled a small static memory-hog (`memhog.c`, not checked into this
repo — a throwaway test tool) that `malloc`s and `memset`s 10 MB chunks in
a loop, dropped it into `rootfs/alpine/usr/local/bin/memhog`, and ran it
capped at 50 MB:

```
$ sudo ./build/container-runtime run --cpu-limit 0.5 --memory-limit 50 /usr/local/bin/memhog
Allocated 10 MB
Allocated 20 MB
Allocated 30 MB
Allocated 40 MB
Allocated 50 MB
```

(Output stops abruptly at 50 MB — no "malloc failed" message, because the
process was killed outright, not returned a failing `malloc`.) Host kernel
log (`dmesg`) at the same moment:

```
memhog invoked oom-killer: gfp_mask=0xcc0(GFP_KERNEL), order=0, oom_score_adj=0
...
oom_kill_process.cold+0x8/0xb5
out_of_memory+0xff/0x2b0
mem_cgroup_out_of_memory+0xc8/0xe0
try_charge_memcg+0x3c3/0x620
...
oom-kill:constraint=CONSTRAINT_MEMCG,...,oom_memcg=/container-runtime/21612,task_memcg=/container-runtime/21612,task=memhog,pid=21612,uid=0
Memory cgroup out of memory: Killed process 21612 (memhog) total-vm:62472kB, anon-rss:53480kB, file-rss:672kB, shmem-rss:0kB, UID:0 pgtables:148kB oom_score_adj:0
```

`constraint=CONSTRAINT_MEMCG` and `oom_memcg=/container-runtime/21612`
confirm this was the container's own cgroup memory limit killing it — not
a host-wide OOM condition. The host (`uptime` immediately after: `load
average: 0.03, 0.01, 0.00`) never noticed.

### Verification — CPU limit

Ran a CPU-bound busy loop (`i=0; while true; do i=$((i+1)); done`) capped
at `--cpu-limit 0.2` (20% of one core), and sampled the cgroup's own
`cpu.stat` `usage_usec` counter before and after a 5-second window on the
host, rather than trusting `top`'s instantaneous sampling:

```
usage_usec delta: 1000143   (over 5.013890114 s wall-clock)
1000143 µs / 1,000,000 = 1.000143 s of CPU time consumed
1.000143 / 5.013890114 = 0.1995  →  19.95% actual CPU usage
```

19.95% actual usage against a 20% configured cap — the limit holds to
within measurement noise. For comparison, the same loop with no cgroup at
all pins a full core (100%) on this single-vCPU `t3.micro`.
