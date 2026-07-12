# container-runtime

A minimal Docker-like container runtime built directly on raw Linux kernel
primitives — namespaces, cgroups v2, OverlayFS, veth networking, and a
real Docker Hub registry client — in C++20, with no container framework
libraries. `sudo ./build/container-runtime run alpine:latest /bin/sh`
pulls the real `alpine` image from Docker Hub and drops you into an
isolated, resource-capped, networked shell running it.

## Architecture

```
container-runtime run <image> <command>
        │
        ▼
 [1] Pull <image> from Docker Hub (registry v2 API) → layer tarballs,
     cached locally                              (src/registry/)
        │
        ▼
 [2] Extract layers, mount via OverlayFS (lower=image layers, upper=
     writable)                                    (src/fs/overlay.cpp)
        │
        ▼
 [3] clone() new PID, UTS, IPC, mount, network namespaces (src/namespaces/)
        │
        ▼
 [4] pivot_root into the merged overlay filesystem  (src/fs/rootfs.cpp)
        │
        ▼
 [5] Write cgroup v2 limits (cpu.max, memory.max)   (src/cgroups/)
        │
        ▼
 [6] Create veth pair, attach to host bridge, configure NAT (src/network/)
        │
        ▼
 [7] exec the container's command — isolated, resource-capped, networked
```

Steps 1-2 and 5-6 happen in the parent process, before/around a `clone()`
call; a small pipe-based readiness barrier (see Phase 5) ensures the
child never execs the container's command until all of that is finished.

## Requirements

- A real Linux host (not macOS/WSL) — this project calls Linux-specific
  syscalls (`clone`, `unshare`, `mount`, `pivot_root`) directly.
- root/sudo — namespace, mount, cgroup, and network setup all require
  elevated privileges.
- A C++20 compiler (g++ or clang++), CMake, and `libcurl4-openssl-dev`.
- Outbound internet access on the host, to pull images from Docker Hub.

## Building

```
mkdir build && cd build
cmake ..    # fetches nlohmann/json via CMake FetchContent on first run
make
```

## Usage

```
sudo ./build/container-runtime run [--cpu-limit N] [--memory-limit MB] <image> [command] [args...]
```

- `--cpu-limit N` — fraction of one CPU core, e.g. `0.5` (default `0.5`).
- `--memory-limit MB` — memory cap in megabytes (default `100`).
- `<image>` — a Docker Hub image reference, e.g. `alpine:latest`,
  `python:3.11-slim`. Pulled and cached under `image-cache/` on first use;
  later runs of the same image reuse the cache.
- `command` — defaults to `/bin/sh` if omitted.

Examples:

```
sudo ./build/container-runtime run alpine:latest
sudo ./build/container-runtime run --cpu-limit 0.2 --memory-limit 50 alpine:latest /bin/sh
sudo ./build/container-runtime run python:3.11-slim python3
```

This pulls the requested image (if not already cached), mounts its layers
via OverlayFS, and drops you into a shell (or runs the given command)
isolated via five namespaces, pivoted into the merged image filesystem,
capped by cgroup v2 CPU/memory limits, and networked via a veth pair to a
NAT'd bridge with outbound internet access.

## Project layout

```
src/main.cpp        entrypoint, CLI parsing
src/namespaces/      namespace isolation (Phase 1)
src/cgroups/         cgroup v2 CPU/memory limits (Phase 3)
src/fs/              pivot_root (Phase 2), OverlayFS (Phase 4)
src/network/         veth + bridge + NAT (Phase 5)
src/registry/        Docker Hub registry client (Phase 6)
include/             shared headers (unused so far; all headers currently
                     live alongside their .cpp under src/)
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

## Phase 4: OverlayFS layered filesystem

### Design

`cr::fs::Overlay` (`src/fs/overlay.cpp`/`.hpp`) mounts a container's root as
an OverlayFS combining:

- **`lowerdir`** — one or more read-only image layers (currently just
  `rootfs/alpine`; Task 6's registry puller will make this a real list of
  layers pulled from a registry).
- **`upperdir`** — a fresh, container-specific writable layer.
- **`workdir`** — OverlayFS's required scratch directory for internal
  bookkeeping (atomic rename operations, etc.) — never touched directly,
  but mandatory.
- **`merged`** (the mount target) — the single directory tree a process
  actually sees, combining all of the above.

All four live under `overlay-data/<containerId>/`, where `containerId` is
this **runtime process's own PID** (`getpid()`), not the container's PID —
the overlay has to be mounted *before* `clone()` (see below), so the
container's own PID doesn't exist yet at that point. This is a different
ID scheme than `cgroups::CGroup` uses (that one waits until after `clone()`
and uses the container's own PID) — both are simply the most convenient
unique value available at the point each subsystem needs one.

**Why mount before `clone()`, in the parent:** `clone(CLONE_NEWNS, ...)`
gives the child a *copy* of the current mount table at the instant of the
call — not a live view of the parent's future mounts. So `Container::run()`
constructs `Overlay` (which mounts it) before calling `clone()`; the child
then inherits the already-mounted merged directory for free, no extra
plumbing required to get it into the container's own mount namespace. The
existing `PivotRoot` (Phase 2) is otherwise unchanged — it just now
receives the overlay's `mergedPath()` instead of the flat `rootfs/alpine`
directory as its target, and mounting via `mount("overlay", merged, ...)`
already makes `merged` a proper mount point, which is exactly what
`pivot_root()` requires.

**RAII cleanup:** `~Overlay()` unmounts `merged` (`umount2(..., MNT_DETACH)`)
and removes `upper`, `work`, `merged`, and their now-empty parent directory
— all on the *host* mount namespace/filesystem, in the parent process,
after `waitpid()` returns in `Container::run()`. Lower layers are never
touched by this destructor; that's what makes layer sharing free. (Hit the
same class of bug as Phase 3's cgroup cleanup while testing this: an
early version of `~Overlay()` removed `upper`/`work`/`merged` but forgot
their shared parent directory, leaving an empty `overlay-data/<id>/`
behind — fixed by explicitly removing `containerDir_` too.)

### Verification

**Isolation + concurrent execution** — ran two containers simultaneously
(container A sleeps 6s and writes `/root/from_a.txt`; container B starts
1s later, writes `/root/from_b.txt`, then lists `/root` from inside
itself):

```
--- overlay-data while both running ---
$ ls overlay-data/
23631
23638
$ ls overlay-data/23631/merged/root/
from_a.txt
$ ls overlay-data/23638/merged/root/
from_b.txt

--- container B's own `ls /root`, run from inside container B ---
from_b.txt
```

Container B's own view of `/root` shows only `from_b.txt` — `from_a.txt`,
written concurrently by container A into A's own upper layer, never
appears. Each container's writes are fully contained in its own
`overlay-data/<pid>/upper/`.

**Cleanup:**

```
--- overlay-data after both containers exit ---
$ ls overlay-data/
(empty)
```

Both containers' `upper`/`work`/`merged` directories (and their parent
dirs) were fully removed once each process exited — no manual cleanup
needed.

**No lower-layer duplication:**

```
$ du -sh rootfs/alpine        # before running any containers
9.5M    rootfs/alpine

$ du -sh rootfs/alpine        # after both containers ran and exited
9.5M    rootfs/alpine
```

Identical disk usage before and after two containers used `rootfs/alpine`
as their shared lower layer — OverlayFS references the existing lower
directory tree directly via the mount, rather than copying it per
container. This is the actual mechanism (not just a claim) behind Docker's
"shared base layers" storage efficiency: N containers built on the same
base image cost roughly the size of N small upper layers, not N times the
base image size.

## Phase 5: Networking — veth pairs, bridge, and NAT

### Design

`src/network/network.cpp`/`.hpp` adds two pieces:

- **`ensureBridge()`** — idempotent, host-level setup: creates a Linux
  bridge `cr0` with `172.20.0.1/24` (if missing), brings it up, enables
  `net.ipv4.ip_forward`, and adds `iptables` NAT (`MASQUERADE`) and
  `FORWARD` rules for the `172.20.0.0/24` subnet. Called before every
  container run; `iptables -C` is used to check whether a rule already
  exists before `-A`ppending it, so repeated calls don't pile up
  duplicates.
- **`cr::network::Veth`** — one veth pair per container: host end
  attached to `cr0`, peer end moved into the container's network
  namespace (`ip link set <peer> netns <pid>`), then configured *inside*
  that namespace via `nsenter -t <pid> -n -- ...` (renamed `eth0`, given
  an IP, brought up, default route added via the bridge).

**Why shell out to `ip`/`iptables`/`nsenter` instead of raw rtnetlink:**
this is a portfolio project, and these are the exact commands a human
operator would run by hand to debug the same setup — the code reads
close to a transcript of what it's doing. The tradeoff is fragility to
exit-code/quoting edge cases that talking to rtnetlink directly wouldn't
have; real production runtimes (`runc`, etc.) use netlink for exactly
that reason. Documented here as a deliberate, known simplification, not
an oversight.

**IP allocation:** each container's address is derived from its own PID
(`2 + pid % 250`) — there's no real IPAM (no persistent lease tracking,
no collision detection). Good enough for a handful of concurrently-running
containers in a demo; a real implementation would track allocated
addresses explicitly.

**`CLONE_NEWNET`** was added to `Container::run()`'s `clone()` flags
alongside the four namespaces from Phase 1.

**The synchronization problem this phase introduced, and how it's
solved:** `clone()` returns in the parent with the child already running
independently — but network setup (creating the veth pair, moving it
into the child's namespace via `/proc/<pid>/ns/net`, assigning its IP)
can only happen *after* `clone()` returns, since it needs the child's
PID. Without something to stop it, the child could `execvp()` the
container's actual command (e.g. immediately try to `ping`) before its
network exists at all. Fixed with a small pipe: `childMain()`'s very
first action is a blocking `read()` on `readyPipe_[0]`; `run()` does all
of its parent-side setup (bridge, veth, cgroup) and only then writes a
byte to `readyPipe_[1]`, releasing the child. This closed the same latent
race that existed for cgroup setup since Phase 3 (the child could
theoretically start running before `cgroup.procs` was written) — one
barrier now covers both.

**RAII cleanup:** `~Veth()` deletes the host-side veth interface
(`ip link del`); the kernel deletes the peer along with it, since veth
interfaces are always destroyed in pairs, and the container's network
namespace itself is torn down once its last process exits. The shared
`cr0` bridge is deliberately *not* torn down per-container — it's host
infrastructure meant to persist across container runs, closer to how
Docker's `docker0` bridge behaves.

### Verification — outbound internet access

```
$ sudo ./build/container-runtime run /bin/sh
# inside the container:
/ # ip addr show eth0
inet 172.20.0.53/24 scope global eth0
/ # ip route
default via 172.20.0.1 dev eth0
172.20.0.0/24 dev eth0 scope link src 172.20.0.53
/ # ping -c3 8.8.8.8
PING 8.8.8.8 (8.8.8.8): 56 data bytes
64 bytes from 8.8.8.8: seq=0 ttl=117 time=1.476 ms
64 bytes from 8.8.8.8: seq=1 ttl=117 time=1.421 ms
64 bytes from 8.8.8.8: seq=2 ttl=117 time=1.474 ms

--- 8.8.8.8 ping statistics ---
3 packets transmitted, 3 packets received, 0% packet loss
```

### Verification — two containers reaching each other

Ran container A (gets `172.20.0.3`, sleeps), then container B (gets
`172.20.0.62`) 2 seconds later, concurrently:

```
--- container A ---
inet 172.20.0.3/24 scope global eth0

--- container B ---
inet 172.20.0.62/24 scope global eth0
---
$ ping -c3 172.20.0.3          # B pinging A, over the cr0 bridge
PING 172.20.0.3 (172.20.0.3): 56 data bytes
64 bytes from 172.20.0.3: seq=0 ttl=64 time=0.067 ms
64 bytes from 172.20.0.3: seq=1 ttl=64 time=0.060 ms
64 bytes from 172.20.0.3: seq=2 ttl=64 time=0.062 ms
3 packets transmitted, 3 packets received, 0% packet loss
---
$ ping -c2 8.8.8.8              # B still reaching the internet too
64 bytes from 8.8.8.8: seq=0 ttl=117 time=1.452 ms
64 bytes from 8.8.8.8: seq=1 ttl=117 time=1.468 ms
2 packets transmitted, 2 packets received, 0% packet loss
```

Sub-millisecond (~0.06ms) round-trip between the two containers confirms
direct bridge-local delivery (versus ~1.5ms to the real internet via
NAT). Both containers reached each other *and* the internet simultaneously.

### Verification — cleanup

```
$ ip link show type veth        # after both containers exited
(nothing — no leftover veth interfaces)

$ ip addr show cr0               # bridge persists (shared infra, by design)
inet 172.20.0.1/24 scope global cr0

$ iptables -t nat -L POSTROUTING -n | grep MASQUERADE
MASQUERADE  all  --  172.20.0.0/24        0.0.0.0/0   (present exactly once, not duplicated)
```

## Phase 6: Registry pull — running real Docker images end-to-end

### Design

`src/registry/registry.cpp`/`.hpp` implements a minimal Docker Hub
registry v2 client using `libcurl` for HTTP and `nlohmann/json`
(fetched automatically via CMake `FetchContent`, pinned to `v3.11.3`)
for parsing:

1. **`fetchToken(repository)`** — `GET auth.docker.io/token?service=registry.docker.io&scope=repository:<repo>:pull`.
   Docker Hub's public registry requires a short-lived bearer token even
   for anonymous, unauthenticated pulls of public images — this just
   fetches one.
2. **`fetchManifest(repository, reference, token)`** — `GET
   registry-1.docker.io/v2/<repo>/manifests/<reference>`, with an
   `Accept` header listing both the classic Docker v2 manifest types and
   the OCI equivalents, plus the "manifest list" / "image index" types.
   Official images (`alpine`, `python`, etc.) are published as a
   **manifest list** — one manifest per CPU architecture, not a single
   manifest — so if the response is a list, this picks the entry with
   `architecture: amd64, os: linux` and recurses one level to fetch that
   concrete manifest by digest.
3. **`pull(imageRef, cacheDir)`** — parses `imageRef` (e.g.
   `"alpine:latest"` → repository `library/alpine`, tag `latest`; the
   `library/` prefix is Docker Hub's namespace for official images with
   no explicit user/org), fetches the manifest, then for each entry in
   its `layers` array downloads the blob (`GET .../blobs/<digest>`, a
   gzipped tarball) and extracts it with `tar` into its own cache
   subdirectory — skipping the download entirely if that directory
   already exists and is non-empty (idempotent local caching).

**Layer ordering:** a manifest lists layers base-first (index 0 = the
image's base layer, last index = the most recently applied layer) —
but OverlayFS's `lowerdir=` option expects the *opposite* order (leftmost
entry = highest priority / topmost layer). `pull()` builds the list in
manifest order, then returns it reversed, so callers can hand the result
straight to `Overlay` as-is.

**`Container`** (`src/namespaces/`) changed from taking a single
`rootfsPath` to an `imageRef` string; `run()` now calls
`registry::pull()` first and passes its (already correctly ordered)
`layerDirs` straight to `fs::Overlay`, replacing the Phase 2-4
hardcoded single-layer `rootfs/alpine` path entirely. `scripts/fetch-rootfs.sh`
and `rootfs/alpine` (from Phase 2) are no longer used by the main flow —
kept only as a historical artifact of how the project bootstrapped
before a real registry client existed.

**What's deliberately not implemented:** the image's `config` blob (which
holds the image's default `CMD`/`ENTRYPOINT`/`ENV`/working directory) is
never fetched or parsed — this runtime always requires an explicit
command from the CLI, consistent with how it's worked since Phase 1.
Real Docker uses the config blob to support `docker run alpine` with no
command and get a sensible default. Also not implemented: private
registries, authentication beyond anonymous public-image tokens, and
content digest verification of downloaded layers (a production registry
client would verify each blob's SHA-256 against the digest named in the
manifest before trusting it).

### Verification — two different real images, end-to-end

**Alpine** (single-layer image):

```
$ time (echo 'cat /etc/os-release; echo ---; ls /; echo ---; ping -c2 8.8.8.8' \
        | sudo ./build/container-runtime run alpine:latest /bin/sh)
NAME="Alpine Linux"
ID=alpine
VERSION_ID=3.24.1
---
bin  dev  etc  home  lib  media  mnt  opt  proc  root  run  sbin  srv  sys  tmp  usr  var
---
PING 8.8.8.8 (8.8.8.8): 56 data bytes
64 bytes from 8.8.8.8: seq=0 ttl=117 time=1.505 ms
64 bytes from 8.8.8.8: seq=1 ttl=117 time=1.528 ms
2 packets transmitted, 2 packets received, 0% packet loss

real    0m2.194s
```

**Python 3.11-slim** (multi-layer image, different base OS entirely —
Debian, not Alpine — proving this isn't Alpine-specific code):

```
$ time (echo 'print(1+1); import sys; print(sys.version); print(open("/etc/os-release").read())' \
        | sudo ./build/container-runtime run python:3.11-slim python3)
2
3.11.15 (main, Jul  6 2026, 21:47:46) [GCC 14.2.0]
PRETTY_NAME="Debian GNU/Linux 13 (trixie)"
NAME="Debian GNU/Linux"
...

real    0m2.659s
```

A real Python interpreter, from a real pulled image, computing `1+1` and
confirming its own OS is Debian trixie (the actual `python:3.11-slim`
base) — not Alpine, not a locally-built rootfs.

### Verification — multi-layer extraction and caching

```
$ find image-cache/library_alpine -maxdepth 2
image-cache/library_alpine
image-cache/library_alpine/latest
image-cache/library_alpine/latest/55afa1ecc21d2bb5        # 1 layer

$ find image-cache/library_python -maxdepth 2
image-cache/library_python
image-cache/library_python/3.11-slim
image-cache/library_python/3.11-slim/298bacc1f3a58135     # 4 layers,
image-cache/library_python/3.11-slim/3ec520ed9418633e     # extracted
image-cache/library_python/3.11-slim/e95a6c7ea7d49b37     # and overlaid
image-cache/library_python/3.11-slim/f66cea3b82f91b15     # correctly

$ du -sh image-cache/library_alpine image-cache/library_python
8.7M    image-cache/library_alpine
135M    image-cache/library_python
```

(`du` printed several "Permission denied" warnings while measuring the
Python image, e.g. on `.../root` and `.../var/cache/apt/archives/partial`
— that's actually confirmation the extraction is *correct*: `tar`
preserved the original restrictive permissions, like `/root` at mode
`0700`, from the real image layers.)

## Known limitations vs. real Docker

Documented honestly, not glossed over:

- **No image config parsing** — no default `CMD`/`ENTRYPOINT`/`ENV`;
  every `run` requires an explicit command (Phase 6).
- **No digest/signature verification** of pulled layers — a real registry
  client verifies each blob's SHA-256 against its manifest-declared
  digest before trusting it (Phase 6).
- **No private registry or non-anonymous auth support** — only Docker
  Hub's public, anonymous pull token flow (Phase 6).
- **Simplified IP allocation** — container addresses are derived from the
  container's own PID (`2 + pid % 250`), not tracked via real IPAM; fine
  for a handful of concurrent containers in a demo, not for production
  scale (Phase 5).
- **RAII cleanup requires normal process exit** — if `container-runtime`
  itself is `SIGKILL`ed (not the containerized process), no C++
  destructor runs, and a stray cgroup or overlay directory can be left
  behind requiring manual cleanup. Inherent to process-lifetime-scoped
  RAII, not fixable without a separate reconciliation/GC pass (Phase 3,
  Phase 4).
- **No multi-container orchestration** — this runs one container per
  invocation; there's no equivalent of `docker-compose` or a daemon
  tracking multiple running containers across invocations.
- **Networking uses shelled-out `ip`/`iptables`/`nsenter`**, not raw
  rtnetlink — chosen deliberately for readability in a portfolio project;
  documented in Phase 5.

## What's proven, phase by phase

1. **Namespaces** — PID 1 inside its own namespace, independent hostname,
   verified via `$$` and `hostname` (host unaffected).
2. **Filesystem isolation** — `pivot_root` into a real rootfs, no path
   back to the host filesystem, verified via `/etc/os-release` and a
   removed `/.old_root`.
3. **Resource limits** — cgroup v2 `memory.max` triggers a real kernel
   `mem_cgroup_out_of_memory` kill at exactly the configured limit (host
   unaffected); `cpu.max` measured at 19.95% actual usage against a 20%
   configured cap.
4. **Layered storage** — two containers sharing one lower layer, writing
   to independent upper layers with zero cross-contamination, and zero
   disk-usage growth from the shared layer.
5. **Networking** — outbound internet access via NAT (`ping 8.8.8.8`
   succeeds), and two concurrently-running containers reaching each other
   directly over the bridge (sub-millisecond latency).
6. **Real image execution** — `alpine:latest` and `python:3.11-slim`,
   two different base OSes, pulled live from Docker Hub and run
   end-to-end through the full isolation/limits/network pipeline.
