# Linux Container Runtime — Engineering Design Document

A container runtime similar to Docker, built from raw Linux kernel
primitives (namespaces, cgroups v2, OverlayFS, and veth networking) with
a real Docker Hub registry client on top, in C++20 with no container
framework libraries.

---

## Purpose of this document

This document preserves the reasoning behind `container-runtime` that
doesn't survive in the code: the alternatives that were considered and
rejected, the constraints that forced specific choices, the tradeoffs
accepted deliberately rather than by accident, and what actually broke
while building this and how that was diagnosed. The README describes what
the system does and how to run it. This document explains why it looks
the way it does, so that someone who has only read this file could
explain, defend, and probe the system as fluently as whoever built it.

## The problem, and why it's hard

Docker, and every other container runtime, is not one mechanism. It's
five or six independent Linux kernel primitives, each solving a narrow
problem, wired together so their combination produces something with no
single name in the kernel itself: an isolated, resource capped, networked
process running an arbitrary downloaded filesystem image. None of the
primitives know about each other. `clone()` doesn't know about cgroups.
cgroups don't know about mount namespaces. OverlayFS doesn't know about
network namespaces. The entire difficulty of building a container runtime
is in the *sequencing and interaction* of independent subsystems, not in
any one subsystem being individually hard:

- Get the ordering wrong (write cgroup limits after the child has already
  started running) and the limits arrive too late to matter.
- Get a namespace flag wrong (forget `CLONE_NEWNS`) and mount operations
  leak back to the host.
- Get filesystem sequencing wrong (call `pivot_root` before making the
  mount tree private) and the whole thing fails outright with a cryptic
  `EINVAL`.
- Get resource ordering wrong (mount a fresh `/proc` before `pivot_root`
  instead of after) and the container sees the host's PID namespace's
  proc entries, not its own.

This project builds each primitive from the raw syscall or interface
level. No `libcontainer`, no `runc`, no container framework of any kind,
so that every one of those sequencing decisions had to be made and
justified explicitly, rather than inherited from a library that already
got it right.

## System overview

```
container-runtime run <image> <command>

1. Pull <image> from Docker Hub (registry v2 API), cache layers locally
   (src/registry/)

2. Extract layers, mount via OverlayFS: image layers as the lower, a
   fresh writable layer on top (src/fs/overlay.cpp)

3. clone() into new PID, UTS, IPC, mount, and network namespaces
   (src/namespaces/)

4. pivot_root into the merged overlay filesystem (src/fs/rootfs.cpp)

5. Write cgroup v2 limits: cpu.max, memory.max, pids.max (src/cgroups/)

6. Create a veth pair, attach it to the host bridge, configure NAT
   (src/network/)

7. exec the container's command: isolated, resource capped, networked
```

Steps 1, 2, 5, and 6 happen in the parent process (the one invoked as
`container-runtime run ...`), either before or immediately after a single
`clone()` call that produces the child, which becomes the container's
PID 1. The child does almost nothing itself: it waits for the parent to
finish setup, sets its hostname, pivots into the prepared filesystem, and
execs the requested command. Nearly all of the interesting work happens
in the parent, which is a deliberate choice explained below.

## Ownership model: why everything is RAII, and what that model doesn't cover

Every subsystem (`Overlay`, `PivotRoot`, `CGroup`, `Veth`) is a class
whose constructor sets the resource up and whose destructor tears it
down. This is not a stylistic preference. It's the direct consequence of
one fact: a container's lifecycle is exactly the lifetime of one C++
stack frame, `Container::run()`. The moment that function returns, every
resource it created should be gone. RAII makes that automatic: there is
no cleanup function to remember to call on every exit path, including
exception paths, because the destructor runs regardless of *how* the
scope ends.

This model has a real, load bearing limitation, discovered and documented
rather than hidden. **RAII cleanup only runs if the process exits through
normal C++ control flow.** If someone sends `SIGKILL` directly to the
`container-runtime` process itself, not the containerized command, the
process is terminated by the kernel with no opportunity to run any
destructor, C++ or POSIX. A killed process cannot execute its own cleanup
code. This is not a bug to fix within the current architecture; it's a
structural property of tying resource lifetime to process lifetime, and
it's the reason real container runtimes (like `dockerd`) run a separate
long lived daemon that reconciles actual kernel state against expected
state on startup, rather than relying purely on in process destructors.
That reconciliation daemon is explicitly out of scope here (see
Limitations). This project accepts the tradeoff of a simpler, daemon
less, one invocation per container model.

## Component: Namespaces (`src/namespaces/`)

### What it does

`Container::run()` calls `clone()` once, with
`CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNS | CLONE_NEWNET`.
The resulting child is simultaneously PID 1 in a new PID namespace, free
to set its own hostname without affecting the host, isolated from the
host's IPC objects, free to mount and unmount without touching the host's
mount table, and possessing its own, initially empty, network stack.

### Alternative considered and rejected: `fork()` plus `unshare()`

The conventional two step approach is `fork()` to create a child, then
have the child call `unshare()` with the same flags. This was rejected in
favor of a single `clone()` call with the namespace flags passed
directly, because `fork()` plus `unshare()` has a real window, however
small, where the child exists and is running *before* it has unshared
into new namespaces. `clone()` creates the child already inside the new
namespaces atomically; there is no intermediate state to reason about or
accidentally leak through.

### Constraint: stack management

`clone()`, unlike `fork()`, does not duplicate the caller's stack. It
needs an explicit stack region for the child to use, since the two
"threads" (parent continuing, child starting) must not share a stack.
This is why `Container::run()` allocates a 1 MB region via `mmap()` with
`MAP_STACK` before calling `clone()`, and why that allocation has to
outlive the child (it's freed only after `waitpid()` returns).

## Component: Filesystem, `pivot_root` and OverlayFS (`src/fs/`)

### `pivot_root`, not `chroot`

`chroot()` only changes what path `/` resolves to for the calling
process. The process's actual root mount is untouched, so a process with
enough privilege, or one with file descriptors opened before the
`chroot()`, can often find a way back out. `pivot_root()`, combined with
a private mount namespace, actually replaces the process's root mount
point and allows the previous root to be unmounted and unlinked entirely,
closing that escape route. This is the standard reason every real
container runtime uses `pivot_root` (or `MS_MOVE` based equivalents), not
`chroot`, and it's why this project does too.

### The `MS_SHARED` and `EINVAL` problem: a real bug, diagnosed

The first working version of `PivotRoot` failed immediately with `EINVAL`
on `pivot_root()` on the target Ubuntu host. The cause: Ubuntu, like most
modern distributions, mounts `/` as `MS_SHARED` by default, meaning mount
and unmount events propagate between the host's mount namespace and any
namespace derived from it. `pivot_root()` refuses to operate under shared
mount propagation. The fix, now the first thing `PivotRoot`'s constructor
does, is `mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr)`,
the same `mount --make-rprivate /` step real runtimes run, scoped to the
child's own (already separate, thanks to `CLONE_NEWNS`) mount namespace.
This was diagnosed by recognizing the specific errno and knowing Ubuntu's
default mount propagation mode, not something discoverable from the
`pivot_root(2)` man page alone.

### The missing `/dev` problem: a real bug, diagnosed

Docker image layers ship **without device nodes**: no `/dev/null`,
`/dev/zero`, `/dev/urandom`, and so on. Real Docker creates these at
container start; this project didn't, for several stages, because
nothing in the first several rounds of manual verification happened to
need them. The gap was found while building a fork bomb stress test:
`busybox ash`'s job control tries to redirect backgrounded jobs through
`/dev/null` and failed with "can't open '/dev/null': No such file or
directory," silently preventing the fork bomb from ever running rather
than the `pids.max` limit stopping it. The fix, in `PivotRoot`: after
`pivot_root()` and `chdir("/")`, but **before** unmounting the relocated
old root, bind mount the host's `/dev` (still reachable at
`/.old_root/dev`) onto the new root's `/dev`. This has to happen in that
specific window, after the pivot and before the old root disappears,
because that's the only point where both the new root (`/dev` as a mount
target) and the old root (`/dev` as a mount source) are simultaneously
reachable.

This fix has an accepted tradeoff: bind mounting the host's `/dev` gives
the container the *same* device nodes the host sees, not a private,
minimal set (which is what real Docker actually does, via a per container
`devtmpfs` or explicit `mknod` calls for a small allow list). Given that
Linux has no separate device namespace concept (device nodes are
inherently global kernel objects, and isolating access to them is
properly a job for the cgroup device controller, or more precisely today
the BPF device cgroup), bind mounting the host's `/dev` doesn't strictly
leak more than would already be reachable without one. But it does mean
the container can see, and potentially open, every device node the host
can, which a production runtime would restrict. See Limitations.

### OverlayFS: mounting before `clone()`, not after

`Overlay`'s constructor mounts the union filesystem in the parent
process, before `clone()` is called, not inside the child. This is
because `clone(CLONE_NEWNS, ...)` gives the child a *copy of the current
mount table at the instant of the call*, not a live view of the parent's
future mounts. If the overlay were mounted after `clone()`, from inside
the child's already separate mount namespace, that would also work and
would arguably be cleaner, fully self contained to the child. But
mounting in the parent first means the same, already verified `PivotRoot`
logic (which expects a pre existing mount point to pivot into) needs no
special casing for whether the mount is already present. The overlay
mount itself, via `mount("overlay", merged, ...)`, is what makes `merged`
a valid mount point in the first place, satisfying `pivot_root`'s
requirement without `PivotRoot` needing to know anything about OverlayFS
at all. This is a deliberate layering choice: `PivotRoot` has zero
OverlayFS specific logic, `Overlay` has zero namespace specific logic,
and `Container::run()` is the only place that knows both exist.

### Two different container ID schemes, and why

`Overlay`'s directories are keyed by the runtime process's own PID
(`getpid()`), while `CGroup`'s directory is keyed by the container's PID
(the value `clone()` returns). This looks inconsistent until you notice
*when* each object is constructed. `Overlay` must exist, and be mounted,
**before** `clone()`, when the container's PID doesn't exist yet, so it
uses the only unique value available at that point: the parent's own
PID. `CGroup` is constructed **after** `clone()` returns, so it uses the
value that's actually meaningful to `cgroup.procs`, a PID, not an
arbitrary process's PID. Both are simply the most convenient unique value
available at the point each subsystem needs one, not the same value,
because the two subsystems are wired up at genuinely different points in
the sequence.

### A cleanup bug that recurred twice, same root cause, two subsystems

`Overlay`'s destructor originally removed `upper/`, `work/`, and
`merged/`, but not their shared parent directory
(`overlay-data/<containerId>/`), leaving an empty directory behind after
every container exited. This was caught by manually inspecting
`overlay-data/` after a test run and noticing it wasn't empty. The fix was
one more `stdfs::remove()` call for the parent directory. What's notable
is that this is the *same class of bug* that would have existed in
`CGroup` too, had it not been designed with a single flat directory per
container from the start: a lesson about RAII cleanup in general. It's
easy to clean up the things you explicitly created and forget the
directory structure that contained them.

## Component: cgroups v2 (`src/cgroups/`)

### Delegation model

cgroup v2 requires a controller (`cpu`, `memory`, `pids`) to be
explicitly enabled in a cgroup's `cgroup.subtree_control` before that
cgroup's *children* can use it. A controller isn't automatically
available just because a parent has it. On the target host, `cpu`,
`memory`, and `pids` were already enabled in the root cgroup's
`subtree_control`, inherited from systemd's own cgroup delegation setup,
which meant `CGroup`'s constructor only needed to create
`/sys/fs/cgroup/container-runtime/` once and enable those three
controllers in *its* `subtree_control`, so that per container child
directories underneath it could set `cpu.max`, `memory.max`, and
`pids.max` directly. This delegation chain, from root to
`container-runtime/` to `<containerId>/`, is why cgroup setup is two
`mkdir`s and one `subtree_control` write, not a more involved
bootstrapping process. It relies on an assumption (systemd already
delegated `cpu`, `memory`, and `pids` at the root) that happened to hold
on the target Ubuntu host, but would need to be verified, or set up
explicitly, on a host where it doesn't.

### `cpu.max` math

`cpu.max` is written as `"<quota_us> <period_us>"`: quota microseconds of
CPU time allowed per period. With a fixed 100ms (`100000`us) period,
`--cpu-limit 0.5` becomes `quota = 0.5 * 100000 = 50000`, meaning 50ms of
CPU time out of every 100ms, or half a core. This was verified, not just
asserted: a busy loop capped at `--cpu-limit 0.2` measured **19.95%**
actual CPU consumption via the cgroup's own `cpu.stat` `usage_usec`
counter sampled over a 5 second wall clock window, against a full core
(100%) with no cap on the same single vCPU host.

### `pids.max` and why it was added after the fact

The original design (namespaces, filesystem, cgroups, networking) only
set `cpu.max` and `memory.max`. `pids.max` was added later, specifically
to make a fork bomb defense claim actually true rather than aspirational.
See "What broke" below for the two bugs (missing `/dev`, and the initial
test design) hit while adding and verifying it. With `pids.max=20`, an
actual fork bomb (`bomb() { bomb & bomb & }; bomb`) inside a container
produced repeated kernel level `fork: Resource temporarily unavailable`
(`EAGAIN`) errors, completed in under a second (because `ash` doesn't
retry failed forks), and left the host's load average at `0.00`: the
cgroup pids controller refusing new processes, not the host running out
of any global resource.

## Component: Networking (`src/network/`)

### Shelling out to `ip`, `iptables`, and `nsenter`, not rtnetlink

Every networking operation (bridge creation, veth pair creation,
namespace assignment, IP configuration, NAT rules) is done by
constructing a command string and calling `std::system()`, rather than by
speaking rtnetlink directly via `<linux/netlink.h>`. This was a conscious
tradeoff, not an oversight. The commands run are the *exact* commands a
human operator would type by hand to debug the same setup, which makes
the code close to a runnable transcript of its own behavior, valuable for
a project whose purpose is demonstrating understanding, not shipping a
production artifact. The real cost is fragility: exit code and string
based error handling is strictly weaker than structured netlink
responses, and every `runCmd()` call is a small parsing and quoting
surface that a netlink API wouldn't have. Real production runtimes
(`runc`, `cni` plugins) use netlink for exactly this reason. This is
named explicitly as a limitation, not discovered as one.

### The synchronization problem networking introduced

`clone()` returns in the parent with the child already running
independently. But wiring up the child's network (creating a veth pair,
moving one end into the child's namespace via `/proc/<pid>/ns/net`,
assigning an IP inside that namespace via `nsenter`) can only happen
*after* `clone()` returns, since all of it needs the child's PID. Without
something to stop it, the child could reach `execvp()` on the container's
actual command, which might immediately try to use the network, before
that network exists at all.

The fix is a small pipe created before `clone()`. `childMain()`'s very
first action, before anything else including `sethostname()`, is a
blocking `read()` on `readyPipe_[0]`. `run()` performs *all* of its
parent side setup (bridge, veth, cgroup) and only afterward writes one
byte to `readyPipe_[1]`, releasing the child. This single barrier
retroactively closed a second, previously unnoticed race: before this
fix, cgroup limits were also applied after `clone()` with no
synchronization, meaning a fast starting child could theoretically begin
running before `cgroup.procs` had been written and its limits took
effect. Adding the readiness barrier for networking fixed both at once,
since both parent side setup steps now happen before the same single
release signal.

### The bridge race: found by design review, not a live crash

`ensureBridge()` does "check if `cr0` exists, create it if not," a
classic check then act race. Two containers starting at nearly the same
instant could both observe the bridge missing and both attempt
`ip link add cr0 type bridge`. The loser fails with "File exists" and
`ensureBridge()` throws, crashing that container's startup. This was
never observed as a live failure during development. Every
multi container demo happened to include enough of a timing gap (a
deliberate `sleep`) to avoid it, but the pattern was recognized as unsafe
by inspection during review. The fix is a `flock()` based file lock
(`/run/container-runtime-bridge.lock`) wrapping the whole check, create,
and configure sequence in `ensureBridge()`, so concurrent invocations
serialize instead of racing. Verified afterward by launching two
containers with zero delay against a freshly deleted bridge: both
succeeded, and the bridge was created exactly once.

### IP allocation: simplified by design, not by accident

Each container's IP is `2 + (containerPid % 250)` on the `172.20.0.0/24`
subnet, derived from the container's own PID, with no persistent lease
table, no collision detection, and no reclamation logic. This is
sufficient for demonstrating that isolated containers can reach each
other and the internet, which is the property being proven. It is
explicitly not a real IPAM system, and two containers landing on the same
derived address, a real if unlikely possibility, would silently misbehave
rather than error out clearly. A real implementation would track
allocated addresses in a small persistent table and hand out the next
free one.

## Component: Registry client (`src/registry/`)

### Why a real registry client, not `docker export`

Early versions of this project used a locally downloaded Alpine
minirootfs tarball (`scripts/fetch-rootfs.sh`) as a single hardcoded
lower layer. That was always understood as a bootstrap step, not the end
state. A container runtime that can only run one specific, manually
prepared filesystem doesn't demonstrate the same thing as one that can
pull and run *arbitrary* real Docker images. The registry client replaces
that bootstrap entirely.

### The two step manifest problem

Docker Hub's official images (`alpine`, `python`, and so on) are
published as a **manifest list** (also known as an OCI image index), one
manifest per CPU architecture, not a single concrete manifest.
Requesting `GET /v2/<repo>/manifests/<tag>` returns this list, and the
actual per architecture manifest (the one with a `layers` array) has to
be fetched separately by digest. `fetchManifest()` handles this by
checking the returned document's `mediaType`. If it's a list or index
type, it scans for the entry with `architecture: amd64, os: linux` and
recurses once more with that entry's digest. This two step indirection is
not optional, or an implementation quirk to work around. It's how the
registry API is actually shaped for any multi architecture image, which
is nearly all of them.

### Layer ordering: reversed on purpose

A manifest's `layers` array is ordered base first (index 0 is the image's
foundational layer; the last index is the most recently applied one).
OverlayFS's `lowerdir=` mount option expects the *opposite* order: the
first entry listed is the topmost, highest priority layer. `pull()`
builds its layer list in manifest order, then returns it reversed,
specifically so the caller (`Container::run()`) can hand the result
straight to `Overlay` with no additional bookkeeping. Getting this
backwards wouldn't cause an obvious crash. It would silently produce a
filesystem where older layers incorrectly shadow newer ones, which is the
kind of bug that's easy to miss without deliberately testing a
multi layer image (this project's `python:3.11-slim` test, at 4 layers,
was specifically chosen to exercise this path; `alpine:latest`, at 1
layer, would not have caught an ordering bug).

### What's deliberately not implemented here (see also Limitations)

The image's `config` blob, which holds the image's default `CMD`,
`ENTRYPOINT`, `ENV`, and working directory, is never fetched. Every `run`
requires an explicit command, consistent with how this runtime has
always worked. Real Docker uses the config blob so `docker run alpine`
with no command still does something sensible. Digest verification of
downloaded layers (checking each blob's SHA256 against the digest named
in its manifest entry) is also not implemented. Layers are trusted as
downloaded once TLS terminates successfully.

## Build system: why CMake `FetchContent` for `nlohmann/json`

The registry client needs a JSON parser. Vendoring a copied single header
file into the repo was rejected in favor of `FetchContent` pinned to a
specific tag (`v3.11.3`) with `GIT_SHALLOW TRUE`, because it keeps the
dependency's provenance explicit, a real upstream tag, not a copy pasted,
unversioned file sitting in the repo, at the cost of requiring network
access during the *build's* configure step, separate from the network
access the *runtime* needs to pull images. `libcurl` was linked as a
system dependency (`find_package(CURL REQUIRED)`) rather than vendored
or hand rolled over raw sockets, since implementing HTTP and TLS
correctly from scratch is a large, security sensitive undertaking wholly
outside this project's purpose (demonstrating kernel level container
mechanics, not implementing an HTTP client).

## What broke during construction: a chronological account

1. **`pivot_root` returned `EINVAL`.** Diagnosed as Ubuntu's default
   `MS_SHARED` root mount propagating events in a way `pivot_root`
   refuses. Fixed with an `MS_REC | MS_PRIVATE` remount before anything
   else. (Filesystem section, above.)
2. **Overlay cleanup left an empty directory behind.** `~Overlay()`
   removed `upper/work/merged` but not their shared parent. Found by
   inspecting `overlay-data/` after a test container exited. Fixed with
   one more `remove()` call.
3. **A stray `main.cpp` at the repo root.** An `scp` invocation that was
   meant to copy `main.cpp` into `src/` instead placed a duplicate at the
   repository root, discovered via `git status` showing an unexpected
   untracked file before a commit. Removed in a follow up commit.
4. **The networking readiness race.** Recognized by reasoning through the
   sequence (`clone()` returns immediately; network setup needs the
   child's PID; nothing stops the child from running ahead of that setup)
   rather than by observing a live failure, and fixed with the readiness
   pipe described above, which also retroactively fixed a same shaped,
   previously unaddressed cgroup timing race.
5. **The bridge check then act race.** Also recognized by design review
   (specifically, while assessing the project against the kind of
   questions a systems focused interviewer would ask) rather than a live
   crash, and fixed with a `flock` based lock. Verified afterward with
   two genuinely simultaneous container launches against a freshly
   deleted bridge.
6. **Missing `/dev` broke the fork bomb test.** `/dev/null` doesn't exist
   in a freshly extracted image layer; `ash`'s job control needs it to
   background processes, so the fork bomb test failed silently (0 forks
   attempted) rather than being stopped by `pids.max`. Diagnosed from the
   shell's own error message (`can't open '/dev/null'`), fixed by bind
   mounting the host's `/dev` at the correct point in the `pivot_root`
   sequence (see Filesystem section).
7. **The fork bomb test itself needed two iterations.** The first attempt
   defined the bomb function in one shell invocation and tried to invoke
   it from a separate one (`sh -c bomb` in a new shell that never saw the
   function definition), producing `sh: bomb: not found`, a test harness
   mistake, not a runtime bug, fixed by defining and invoking the function
   within the same `sh -c` call.

## Appendix: verification evidence

The README states what's been verified. This appendix has the actual
transcripts, for anyone who wants to check the claims rather than take
them on faith. All of it was run on a real EC2 Ubuntu host, not
simulated.

### Namespaces: PID and hostname isolation

```
$ echo 'echo PID inside namespace: $$; hostname' | sudo ./build/container-runtime run /bin/sh
PID inside namespace: 1
container

$ hostname   # on the host, in a separate shell
ip-172-31-9-167
```

`echo $$` prints `1` inside the container (its own PID namespace) and
`hostname` differs from the host's own, which is unaffected.

### Filesystem: `pivot_root` isolation

```
$ sudo ./build/container-runtime run /bin/sh
/ $ ls /
bin  dev  etc  home  lib  media  mnt  opt  proc  root  run  sbin  srv  sys  tmp  usr  var
/ $ ls /.old_root
ls: /.old_root: No such file or directory
/ $ cat /etc/os-release
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
```

`/.old_root` doesn't exist, so there's no path back to the host. `ps aux`
shows only the container's own two processes. The host's own filesystem
and process list are completely unaffected.

### cgroups: memory limit, a real OOM kill

A small memory hog (`malloc` plus `memset` in a loop, not checked into
this repo) capped at 50 MB:

```
$ sudo ./build/container-runtime run --memory-limit 50 /usr/local/bin/memhog
Allocated 10 MB
Allocated 20 MB
Allocated 30 MB
Allocated 40 MB
Allocated 50 MB
```

`dmesg` at the same moment:

```
memhog invoked oom-killer: gfp_mask=0xcc0(GFP_KERNEL), order=0, oom_score_adj=0
...
mem_cgroup_out_of_memory+0xc8/0xe0
...
oom-kill:constraint=CONSTRAINT_MEMCG,...,oom_memcg=/container-runtime/21612,task=memhog,pid=21612,uid=0
Memory cgroup out of memory: Killed process 21612 (memhog) total-vm:62472kB, anon-rss:53480kB
```

`constraint=CONSTRAINT_MEMCG` confirms the container's own cgroup limit
killed it, not a host wide OOM. Host `uptime` immediately after showed
`load average: 0.03, 0.01, 0.00`, unaffected.

### cgroups: CPU limit, measured, not asserted

A busy loop capped at `--cpu-limit 0.2`, sampled via the cgroup's own
`cpu.stat` `usage_usec` counter over a 5 second window:

```
usage_usec delta: 1000143   (over 5.013890114 s of wall clock time)
1000143 µs / 1,000,000 = 1.000143 s of CPU time consumed
1.000143 / 5.013890114 = 0.1995  →  19.95% actual CPU usage
```

19.95% against a 20% cap. The same loop uncapped pins 100% of this single
vCPU host.

### cgroups: pids limit, a real fork bomb

```
$ echo "timeout 5 sh -c 'bomb() { bomb & bomb & }; bomb'" \
    | sudo ./build/container-runtime run --pids-limit 20 alpine:latest /bin/sh
sh: can't fork: Resource temporarily unavailable
sh: can't fork: Resource temporarily unavailable
sh: can't fork: Resource temporarily unavailable

$ uptime   # immediately after
load average: 0.00, 0.00, 0.00
```

The kernel refuses new forks (`EAGAIN`) once the 20 process cap is hit.
The whole attempt completes in under a second with host load at `0.00`.

### OverlayFS: isolation and no duplication

Two containers running simultaneously, each writing a different file:

```
$ ls overlay-data/
23631
23638
$ ls overlay-data/23631/merged/root/
from_a.txt
$ ls overlay-data/23638/merged/root/
from_b.txt

# container B's own `ls /root`, from inside container B:
from_b.txt
```

Container B never sees `from_a.txt`, written concurrently by A into A's
own upper layer. After both exit:

```
$ ls overlay-data/
(empty)
```

Disk usage of the shared base layer, before and after both containers
ran:

```
$ du -sh rootfs/alpine
9.5M    rootfs/alpine   (unchanged both times)
```

### Networking: outbound access and container to container

```
$ sudo ./build/container-runtime run /bin/sh
/ # ping -c3 8.8.8.8
64 bytes from 8.8.8.8: seq=0 ttl=117 time=1.476 ms
3 packets transmitted, 3 packets received, 0% packet loss
```

Two containers running concurrently, one pinging the other over the
bridge:

```
$ ping -c3 172.20.0.3
64 bytes from 172.20.0.3: seq=0 ttl=64 time=0.067 ms
3 packets transmitted, 3 packets received, 0% packet loss
```

About 0.06ms bridge local latency versus about 1.5ms to the real internet
via NAT, consistent with direct L2 delivery. Both containers reached each
other *and* the internet at the same time. After both exit:

```
$ ip link show type veth
(nothing, no leftover veth interfaces)
$ iptables -t nat -L POSTROUTING -n | grep MASQUERADE
MASQUERADE  all  --  172.20.0.0/24        0.0.0.0/0   (present exactly once)
```

### Registry: two different real images, start to finish

```
$ time (echo 'cat /etc/os-release; ping -c2 8.8.8.8' \
        | sudo ./build/container-runtime run alpine:latest /bin/sh)
PRETTY_NAME="Alpine Linux v3.24"
2 packets transmitted, 2 packets received, 0% packet loss
real    0m2.194s
```

```
$ time (echo 'print(1+1); import sys; print(sys.version); print(open("/etc/os-release").read())' \
        | sudo ./build/container-runtime run python:3.11-slim python3)
2
3.11.15 (main, Jul  6 2026, 21:47:46) [GCC 14.2.0]
PRETTY_NAME="Debian GNU/Linux 13 (trixie)"
real    0m2.659s
```

A real Python interpreter from a real pulled image, confirming its own
OS is Debian trixie, not Alpine, not a locally built rootfs. Layer counts
differ correctly by image:

```
$ find image-cache/library_alpine -maxdepth 2   # 1 layer
$ find image-cache/library_python -maxdepth 2   # 4 layers
$ du -sh image-cache/library_alpine image-cache/library_python
8.7M    image-cache/library_alpine
135M    image-cache/library_python
```

### `scripts/test-harness.sh`: repeated run summary

```
=== image compatibility ===
alpine:latest: 0.79s   python:3.11-slim: 0.74s
busybox:latest: 0.73s  debian:bookworm-slim: 0.77s

=== cold vs. cached pull latency (alpine:latest) ===
cold pull: 1.06s   cached: 0.71s

=== repeated-run stats (alpine:latest, cached, x20) ===
p50=0.736s  p95=0.920s  min=0.713s  max=0.920s

=== leak check after all runs ===
clean: no leaked mounts, cgroups, veth interfaces, or overlay dirs

=== summary ===
pass=26 fail=0 total=26  pass_rate=100.0%
```

## Limitations, known issues, and future work

This section is deliberately exhaustive rather than reassuring. Everything
listed here is either a conscious tradeoff already explained above, or a
gap not yet closed.

**Security boundary is real but partial.** `pivot_root` plus five
namespaces gives filesystem, PID, hostname, IPC, and network isolation.
But the container process runs as **actual root**, with the full set of
Linux capabilities (`CAP_SYS_ADMIN`, `CAP_SYS_MODULE`, and so on), in the
same user ID space as the host. There is no `CLONE_NEWUSER` (user
namespace) remapping root inside the container to an unprivileged UID
outside it, no capability dropping before `exec`, and no seccomp filter
restricting available syscalls. This is the single biggest gap between
this runtime and a real security boundary: a process inside this
container that finds a way to reach a host resource (a device node via
the bind mounted `/dev`, a `mount()` syscall, a kernel module load) is
not stopped by anything this project builds, because it has the actual
privilege to do those things. Closing this fully requires, in increasing
order of effort: dropping capabilities before `exec` (moderate, self
contained, doesn't interact with other subsystems); full user namespace
UID and GID mapping (substantial, interacts with cgroup `cgroup.procs`
write semantics, OverlayFS permission handling, and `pivot_root`, all of
which currently assume real root); and a seccomp allow list (separate
effort, with a risk of breaking functionality if too strict). None of
these are implemented.

**Bind mounted `/dev` is not a private device namespace.** The container
sees the same device nodes the host does, rather than a minimal, per
container set. Linux has no device namespace concept. Isolating device
access is properly a cgroup device controller job, which this project
doesn't configure. See the security boundary point above; they're the
same underlying gap viewed from two angles.

**No image config parsing.** No default `CMD`, `ENTRYPOINT`, `ENV`, or
working directory; every `run` requires an explicit command.

**No digest or signature verification** of pulled layers. A production
registry client verifies each blob's SHA256 against its manifest declared
digest before trusting it. This one trusts TLS alone.

**No private registry or non anonymous auth support.** Only Docker Hub's
public, anonymous pull token flow.

**Simplified IP allocation.** PID derived, not tracked, collision prone
at scale. Fine for a handful of concurrent demo containers.

**RAII cleanup requires normal process exit.** A `SIGKILL` to
`container-runtime` itself, not the containerized process, leaves no
opportunity for any destructor to run, and can leave a stray cgroup or
overlay directory behind. This is a structural property of tying
resource lifetime to a single process's lifetime, not something fixable
without a separate reconciliation or garbage collection pass, which real
runtimes solve with a persistent daemon that reconciles expected versus
actual kernel state on startup. Not implemented here.

**No multi container orchestration.** One container per invocation. No
daemon, no equivalent of `docker-compose`, no tracking of multiple
running containers across separate invocations beyond the shared bridge
and cgroup base directory.

**Networking uses shelled out `ip`, `iptables`, and `nsenter`**, not raw
rtnetlink. A deliberate readability tradeoff for a portfolio project, at
the cost of exit code and string parsing fragility a netlink based
implementation wouldn't have.

**No CI, only a local test harness** (`scripts/test-harness.sh`).
GitHub hosted Actions runners restrict exactly the privileged operations
this project needs (cgroup v2 delegation, `unshare`, `pivot_root` from
inside their own already containerized environment). Running this in
real CI would likely require a self hosted runner with genuine root
access on a real Linux host, which hasn't been set up.

**Network throughput overhead has never been measured.** Connectivity
(ping latency, packet loss) has been verified. Sustained throughput
through the veth, bridge, and NAT path (via `iperf3`, for example) has
not.

**Single architecture assumption.** The registry client's manifest list
resolution hardcodes `architecture: amd64, os: linux`. It has no path for
other architectures even though the manifest list it parses often
contains them.

**Measured evidence for abnormal exit cleanup would currently show
failure, not success.** Deliberately not added as an automated test
without first fixing the underlying gap (see "RAII cleanup requires
normal process exit" above). A metric that's cheap to produce but
currently reads as failing isn't worth adding to a test suite on its own
without the corresponding fix landing at the same time.
