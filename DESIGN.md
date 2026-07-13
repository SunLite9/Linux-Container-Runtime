# Linux Container Runtime — Design Document

A container runtime similar to Docker, built from raw Linux kernel primitives (namespaces, cgroups v2, OverlayFS, veth networking) with a real Docker Hub registry client on top, written in C++20 with no container framework libraries.

## 1. Executive Overview

This project is a working, from scratch implementation of the mechanics that make Docker possible. It pulls a real image from Docker Hub, extracts and layers its filesystem with OverlayFS, isolates a process into its own PID, mount, UTS, IPC, and network namespaces, pivots its root filesystem, applies cgroup v2 resource limits, wires it into a NAT'd virtual network, and execs the requested command, all through direct Linux syscalls with no dependency on `runc`, `libcontainer`, or any existing container framework.

The single command `sudo ./build/container-runtime run alpine:latest /bin/sh` performs every one of those steps and drops the caller into an isolated, resource capped, networked shell running the real, unmodified `alpine` image. Every claim made about the system in this document, isolation, resource limits, layer sharing, networking, and real image compatibility, has been measured against a live Linux host, not just asserted, and the raw evidence is reproduced in full later in this document.

The project was built in stages: namespace isolation first, then filesystem isolation, then resource limits, then layered storage, then networking, then a real registry client, followed by a hardening pass that closed several gaps found by adversarial testing and self review (a fork bomb defense, a concurrency race in bridge setup, and a missing device filesystem). What remains open is disclosed candidly in Section 21, most importantly that this runtime provides real namespace isolation but not real privilege isolation, since the container process still runs as actual root with full Linux capabilities.

## 2. Problem Definition and Context

### What a container actually is

Docker, and every other container runtime, is not one mechanism. It is five or six independent Linux kernel primitives, each solving a narrow problem, wired together so their combination produces something with no single name in the kernel itself: an isolated, resource capped, networked process running an arbitrary downloaded filesystem image. The primitives involved are:

- **Namespaces** (`clone`/`unshare` with `CLONE_NEW*` flags): isolate what a process can see (its own process tree, hostname, IPC objects, mount table, network stack).
- **cgroups**: limit what a process can consume (CPU time, memory, process count).
- **A union filesystem** (OverlayFS): let many containers share a common base filesystem image while keeping their own writes private.
- **`pivot_root`**: replace a process's root filesystem entirely, closing off any path back to the host's real filesystem.
- **Virtual networking** (veth pairs, bridges, NAT): give an isolated network namespace a way to reach other containers and the outside world.
- **A registry client**: fetch a real, versioned, shareable filesystem image from a remote source (Docker Hub) rather than a manually prepared one.

None of these primitives know about each other. `clone()` doesn't know about cgroups. cgroups don't know about mount namespaces. OverlayFS doesn't know about network namespaces. The entire difficulty of building a container runtime is in the *sequencing and interaction* of independent subsystems, not in any one subsystem being individually hard. Concretely:

- Get the ordering wrong (write cgroup limits after the child has already started running) and the limits arrive too late to matter.
- Get a namespace flag wrong (forget `CLONE_NEWNS`) and mount operations leak back to the host.
- Get filesystem sequencing wrong (call `pivot_root` before making the mount tree private) and the whole thing fails outright with a cryptic `EINVAL`.
- Get resource ordering wrong (mount a fresh `/proc` before `pivot_root` instead of after) and the container sees the host's PID namespace's proc entries, not its own.

### Why this project exists

Almost every working engineer has *used* Docker. Very few have built the mechanics underneath it, because there is rarely a practical reason to when `runc` and `containerd` already exist and are correct. That gap between "uses containers" and "understands what a container actually is at the kernel level" is exactly what this project closes. It was built specifically to demonstrate systems level understanding of Linux process isolation, resource control, filesystem layering, and networking, using nothing but the raw primitives, so that every sequencing decision above had to be made and justified explicitly rather than inherited from a library that already got it right.

### Development context

The project was built entirely on a real Linux host, an AWS EC2 `t3.micro` instance running Ubuntu, since namespaces, `pivot_root`, and cgroup v2 delegation require a real Linux kernel and cannot be exercised from macOS or WSL. The instance was provisioned specifically for this project, used for the full build and verification cycle described in this document, and terminated once the codebase and all verification evidence were safely committed to GitHub, since nothing about the finished artifact depends on that instance continuing to exist.

## 3. Goals, Success Criteria, and Scope

### Goals

1. Isolate a process using real Linux namespaces (PID, mount, UTS, IPC, network), built directly on `clone()`, not a wrapper library.
2. Give that process its own root filesystem via `pivot_root`, backed by OverlayFS so that image layers can be shared across containers without duplication.
3. Enforce CPU, memory, and process count limits on that process via cgroup v2, verified under real adversarial stress (an actual memory hog and an actual fork bomb, not just a claim).
4. Give that process outbound and inter container network connectivity via a veth pair, a Linux bridge, and NAT.
5. Make the whole pipeline work against **real, unmodified Docker Hub images**, not a single hand prepared filesystem, by implementing a real registry v2 client.
6. Do all of this in C++20 using RAII for resource lifetime management, with no container framework dependency of any kind.

### Success criteria

The project is considered successful if each of the following can be demonstrated with real, reproducible evidence rather than assertion:

- A process inside the container is PID 1 in its own PID namespace and cannot see the host's process tree.
- The container's filesystem is genuinely isolated: there is no path back to the host filesystem, and the container sees the pulled image's actual files, not the host's.
- A memory limit set on a container is enforced by the kernel (an over limit allocation is OOM-killed), not just configured.
- A CPU limit set on a container measurably caps its actual CPU consumption, not just configured.
- A process count limit set on a container stops a real fork bomb from taking down the host, not just configured.
- Two containers sharing a base image do not duplicate that image's storage on disk.
- A container can reach the public internet and can reach another concurrently running container.
- A real, live pulled Docker Hub image (not a bootstrap tarball) runs successfully end to end, and this is demonstrated with at least two different base images to prove the mechanism is general, not hard-coded to one image.

All of the above have been met and are documented with raw evidence in Section 16.

### Scope

**In scope:** a single container-per-invocation command line tool. Each invocation of `container-runtime run <image> <command>` pulls (or reuses a cached copy of) one image, builds one isolated environment, runs one command in it, and exits when that command exits.

**Explicitly out of scope**, by design, not by oversight: a background daemon, multi-container orchestration, a compose style multi service model, private registry support, and a full security boundary equivalent to production container runtimes (see Section 17 and Section 21 for the specific gap). The project's purpose is to demonstrate the underlying mechanics correctly and honestly, not to compete with `runc`/`containerd`/`dockerd` as a production artifact.

## 4. Requirements and Constraints

### Functional requirements

- Isolate a process into new PID, mount, UTS, IPC, and network namespaces.
- Provide the isolated process a filesystem built from one or more OverlayFS layers, with a private writable layer per container.
- Enforce CPU, memory, and process count limits via cgroup v2 on the isolated process and everything it spawns.
- Provide the isolated process outbound internet access and the ability to reach other concurrently running containers.
- Pull real image manifests and layers from Docker Hub's public registry API, resolving multi-architecture manifest lists, and cache them locally.
- Accept a CLI invocation specifying an image reference, an optional command, and optional resource limit flags.

### Non functional requirements and environmental constraints

- **A real Linux kernel is mandatory.** `clone()` with namespace flags, `pivot_root`, and cgroup v2 are Linux specific; there is no macOS or WSL equivalent. This ruled out development on the author's local machine entirely.
- **root/sudo is required for nearly every subsystem**: namespace creation, mount operations, cgroup filesystem writes, and network configuration (`ip`, `iptables`) all require elevated privileges.
- **cgroup v2 delegation is assumed, not bootstrapped.** The implementation assumes `cpu`, `memory`, and `pids` are already enabled in the root cgroup's `subtree_control` (true on the target Ubuntu host, because systemd delegates them by default). A host where systemd hasn't delegated these would need manual setup this project doesn't perform.
- **Single vCPU target host.** Development and all CPU limit measurements were done on an EC2 `t3.micro`, a single vCPU instance. CPU percentage claims are relative to that one core.
- **Outbound internet access is required at two different times for two different reasons**: once during the CMake configure step, to fetch the `nlohmann/json` dependency via `FetchContent`, and again at runtime, to pull image manifests and layers from Docker Hub.
- **Toolchain**: a C++20 compiler (verified with g++ 15.2.0), CMake, and `libcurl4-openssl-dev`.
- **IFNAMSIZ**: the Linux kernel caps network interface names at 15 characters, which directly constrained the veth interface naming scheme (see Section 9).
- **Docker Hub's public registry still requires an auth token** even for fully anonymous, unauthenticated pulls of public images, which added an unavoidable extra HTTP round trip to every pull.

## 5. System Architecture

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

Steps 1, 2, 5, and 6 happen in the **parent** process, the one invoked as `container-runtime run ...`, either before or immediately after a single `clone()` call that produces the **child**, which becomes the container's PID 1. The child does almost nothing itself: it waits for the parent to finish setup, sets its hostname, pivots into the prepared filesystem, and execs the requested command. Nearly all of the interesting work happens in the parent, which is a deliberate choice (see Section 11).

### Component map

```
src/main.cpp        entrypoint, CLI parsing
src/namespaces/      namespace isolation (Container class, clone() orchestration)
src/cgroups/         cgroup v2 CPU, memory, and pids limits
src/fs/              pivot_root and OverlayFS
src/network/         veth pair, bridge, NAT
src/registry/        Docker Hub registry v2 client
```

Each directory corresponds to exactly one of the kernel primitives described in Section 2, and, with one exception (`Container` in `src/namespaces/`, which also orchestrates the other subsystems), each has zero knowledge of the others. `PivotRoot` has no OverlayFS specific logic. `Overlay` has no namespace specific logic. `Container::run()` is the only place in the codebase that knows all of the subsystems exist and wires them together in the right order.

## 6. End to End System Flow

This section walks through exactly what happens, in order, for `sudo ./build/container-runtime run alpine:latest /bin/sh`, naming the actual functions and files involved.

1. **`main()`** (`src/main.cpp`) parses the CLI: subcommand (`run`), optional `--cpu-limit`/`--memory-limit`/`--pids-limit` flags, the image reference, and the command plus arguments. It initializes libcurl globally (`curl_global_init`), constructs a `cr::Container`, and calls `container.run()` inside a `try`/`catch` so that any exception thrown anywhere in the pipeline (a failed pull, a failed mount, a failed cgroup write) is reported as a clean error message rather than an unhandled `terminate()`.

2. **`Container::run()`** (`src/namespaces/container.cpp`) begins in the parent process:
   - Calls `registry::pull(imageRef_, "image-cache")` (Section 7.5), which resolves the image reference, fetches an auth token, fetches and resolves the manifest (recursing once if it's a multi-architecture manifest list), downloads and extracts any layers not already cached, and returns an ordered list of layer directories.
   - Constructs an `fs::Overlay` (Section 7.2) with those layer directories, which mounts the union filesystem immediately, in the parent's own mount namespace, before any `clone()` has happened.
   - Creates a synchronization pipe (`pipe(readyPipe_)`) and allocates a 1 MB stack region via `mmap()` for the child.
   - Calls `clone()` with `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNS | CLONE_NEWNET`, producing the child. This single syscall creates the child already inside all five new namespaces.

3. **Still in the parent**, now that `clone()` has returned the child's PID:
   - `network::ensureBridge()` (Section 7.4) idempotently creates the shared `cr0` bridge if it doesn't already exist, under a file lock that serializes concurrent invocations.
   - A `network::Veth` is constructed, creating a veth pair, attaching the host end to `cr0`, moving the peer into the child's network namespace via `/proc/<childPid>/ns/net`, and configuring it there via `nsenter` (renamed to `eth0`, given an IP, brought up, default route added).
   - A `cgroups::CGroup` is constructed for the child's PID, creating its cgroup directory and writing `cpu.max`, `memory.max`, and `pids.max`, then the child's PID is written to `cgroup.procs`.
   - Only now does the parent write one byte to the pipe, releasing the child.
   - The parent calls `waitpid()` and blocks until the child (and everything it may have forked) exits.

4. **Meanwhile, in the child** (`Container::childMain()`), which has been blocked since the instant it was created:
   - Its very first action is a blocking `read()` on the pipe, which only returns once the parent has finished all of the setup in step 3.
   - It calls `sethostname("container", ...)`.
   - It constructs an `fs::PivotRoot` (Section 7.3) targeting the overlay's merged directory, which makes the mount tree private, bind mounts the merged directory onto itself, calls `pivot_root()`, binds the host's `/dev` in before the old root disappears, unmounts and removes the old root, and mounts a fresh `/proc`.
   - It builds an argv array from the requested command and arguments and calls `execvp()`, replacing its own process image. From this point on, the child *is* the requested command, still PID 1 in its own namespace, running inside the pivoted filesystem, subject to the cgroup limits, with working network access.

5. **Back in the parent**, once `waitpid()` returns (the command has exited), the `CGroup`, `Veth`, and `Overlay` objects constructed earlier go out of scope in reverse order of construction, and their destructors run: the cgroup directory is removed, the host side veth interface is deleted (which the kernel also removes the peer for), and the overlay is unmounted and its upper/work/merged directories removed. The shared `cr0` bridge is deliberately not touched, since it is host infrastructure meant to persist across invocations.

## 7. Component Level Design

### 7.1 Namespaces (`src/namespaces/`)

`Container::run()` calls `clone()` once, with `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNS | CLONE_NEWNET`. The resulting child is simultaneously PID 1 in a new PID namespace, free to set its own hostname without affecting the host, isolated from the host's IPC objects, free to mount and unmount without touching the host's mount table, and possessing its own, initially empty, network stack.

`clone()` was chosen over the conventional `fork()` followed by `unshare()` because the two step approach has a real window, however small, where the child exists and is running *before* it has unshared into new namespaces. `clone()` creates the child already inside the new namespaces atomically; there is no intermediate state to reason about or accidentally leak through (see Section 11 for the full alternatives comparison).

`clone()`, unlike `fork()`, does not duplicate the caller's stack. It needs an explicit stack region for the child to use, since the two "threads" (parent continuing, child starting) must not share a stack. This is why `Container::run()` allocates a 1 MB region via `mmap()` with `MAP_STACK` before calling `clone()`, and why that allocation has to outlive the child (it is freed only after `waitpid()` returns).

`Container` owns the cloned child's PID and, in its destructor, reaps the child if `run()` didn't already do so.

### 7.2 Filesystem: OverlayFS (`src/fs/overlay.cpp`)

`cr::fs::Overlay` mounts a container's root as an OverlayFS combining:

- **`lowerdir`**: one or more read-only image layers, populated by the registry client (Section 7.5).
- **`upperdir`**: a fresh, container specific writable layer.
- **`workdir`**: OverlayFS's required scratch directory for internal bookkeeping, never touched directly but mandatory.
- **`merged`** (the mount target): the single directory tree a process actually sees, combining all of the above.

All four live under `overlay-data/<containerId>/`, where `containerId` is the *runtime process's own PID* (`getpid()`), not the container's PID, because the overlay has to be mounted *before* `clone()`, when the container's own PID doesn't exist yet. `cgroups::CGroup`, constructed after `clone()`, uses the container's own PID instead. Both are simply the most convenient unique value available at the point each subsystem needs one.

The overlay is mounted in the **parent** process, before `clone()`, not inside the child, because `clone(CLONE_NEWNS, ...)` gives the child a *copy* of the current mount table at the instant of the call, not a live view of the parent's future mounts. Mounting in the parent first also means the already-verified `PivotRoot` logic needs no special casing for whether its target mount point already exists: the `mount("overlay", merged, ...)` call itself is what makes `merged` a valid mount point in the first place.

`~Overlay()` unmounts `merged`, and removes `upper`, `work`, `merged`, and their now empty parent directory, all in the parent process, after `waitpid()` returns. Lower layers are never touched by this destructor, which is what makes layer sharing free (see Section 16 for measured proof).

### 7.3 Filesystem: `pivot_root` (`src/fs/rootfs.cpp`)

`cr::fs::PivotRoot` swaps the process's root filesystem to the target directory (the overlay's merged path) using `pivot_root` rather than `chroot`. `chroot()` only changes what path `/` resolves to for the calling process; the process's actual root mount is untouched, so a process with enough privilege, or one with file descriptors opened before the `chroot()`, can often find a way back out. `pivot_root()`, combined with a private mount namespace, actually replaces the process's root mount point and allows the previous root to be unmounted and unlinked entirely, closing that escape route.

The steps, in order, inside `PivotRoot`'s constructor:

1. Make the mount tree private (`MS_REC | MS_PRIVATE`) to work around Ubuntu's default `MS_SHARED` root mount, which otherwise makes `pivot_root` fail with `EINVAL` (see Section 12 for how this was diagnosed).
2. Bind mount the target directory onto itself, since `pivot_root`'s target must already be a mount point.
3. Create `.old_root` inside the target, the location `pivot_root` will relocate the previous root to.
4. Call `pivot_root()` via `syscall(SYS_pivot_root, ...)`, since glibc doesn't wrap it.
5. `chdir("/")`.
6. Bind mount the host's `/dev` (still reachable at `/.old_root/dev`) onto the new root's `/dev`, before the old root is unmounted (see Section 12 for why this step exists).
7. Unmount and remove `/.old_root`, severing the container's path back to the host filesystem entirely.
8. Mount a fresh `/proc`, scoped to the new PID namespace.

Only the first two steps are treated as reversible in the constructor's error handling; everything from `pivot_root` onward is irreversible in practice, so a failure there is fatal to the container rather than something to roll back, the same tradeoff real container runtimes make.

### 7.4 Networking (`src/network/`)

`cr::network::ensureBridge()` idempotently creates a Linux bridge `cr0` with `172.20.0.1/24` if missing, brings it up, enables `net.ipv4.ip_forward`, and adds `iptables` NAT (`MASQUERADE`) and `FORWARD` rules for the `172.20.0.0/24` subnet, using `iptables -C` to check for existing rules before appending so repeated calls don't create duplicates. The whole function runs under a `flock()`-based file lock (`/run/container-runtime-bridge.lock`) so concurrently starting containers can't race on bridge creation (see Section 12).

`cr::network::Veth` creates one veth pair per container: the host end is attached to `cr0`, and the peer end is moved into the container's network namespace (`ip link set <peer> netns <pid>`) and configured there via `nsenter -t <pid> -n -- ...` (renamed to `eth0`, given an IP derived from the container's PID, brought up, default route added via the bridge).

Every networking operation is done by constructing a command string and calling `std::system()`, rather than speaking rtnetlink directly via `<linux/netlink.h>` (see Section 11 for the full tradeoff discussion). `CLONE_NEWNET` was added to `Container::run()`'s `clone()` flags alongside the other four namespaces.

Wiring up the child's network can only happen *after* `clone()` returns, since it needs the child's PID, which introduced a synchronization problem solved by a readiness pipe (see Section 6, step 3 to 4, and Section 12 for the bug this closed).

`~Veth()` deletes the host side veth interface; the kernel deletes the peer along with it, since veth interfaces are always destroyed in pairs, and the container's network namespace itself is torn down once its last process exits. The shared `cr0` bridge is deliberately not torn down per-container.

### 7.5 Registry client (`src/registry/`)

`cr::registry::pull(imageRef, cacheDir)` implements a minimal Docker Hub registry v2 client using `libcurl` for HTTP and `nlohmann/json` for parsing:

1. **`parseImageRef`** splits `imageRef` (e.g. `"alpine:latest"`) into a repository (`library/alpine`, using Docker Hub's `library/` namespace for official images with no explicit user or org) and a tag (`latest`).
2. **`fetchToken`** requests a short lived anonymous pull token from `auth.docker.io`, required even for public, unauthenticated image pulls.
3. **`fetchManifest`** requests the manifest from `registry-1.docker.io`, with an `Accept` header listing both classic Docker v2 manifest types and OCI equivalents, plus manifest list and image index types. If the response is a multi-architecture manifest list (true for official images like `alpine` and `python`), it picks the entry with `architecture: amd64, os: linux` and recurses once more by digest to fetch the concrete manifest.
4. **`pull`** then, for each entry in the manifest's `layers` array, downloads the blob (a gzipped tarball) and extracts it with `tar` into its own cache subdirectory under `image-cache/<repository>/<tag>/<shortDigest>/`, skipping the download if that directory already exists and is non empty.

Manifests list layers base first (index 0 is the foundational layer); OverlayFS's `lowerdir=` option expects the opposite order (first entry is the topmost layer). `pull()` builds its list in manifest order and returns it reversed, so callers can hand the result straight to `Overlay` with no additional bookkeeping (see Section 9 for why this matters and how it was tested).

The image's `config` blob (holding the image's default `CMD`/`ENTRYPOINT`/`ENV`) is never fetched; every `run` requires an explicit command. Digest verification of downloaded layers is not implemented; layers are trusted once TLS terminates successfully (see Section 17 and Section 21).

### 7.6 cgroups v2 (`src/cgroups/`)

`cr::cgroups::CGroup` owns one cgroup v2 group per container, at `/sys/fs/cgroup/container-runtime/<containerPid>`. On the target host, `cpu`, `memory`, and `pids` were already enabled in the root cgroup's `subtree_control`, inherited from systemd's own delegation setup, so the constructor only needs to create `/sys/fs/cgroup/container-runtime/` once and enable those three controllers in *its* `subtree_control`, so per-container child directories underneath it can set limits directly.

It writes `cpu.max` as `"<quota_us> <period_us>"` (a fixed 100ms period; `--cpu-limit 0.5` becomes `"50000 100000"`), `memory.max` as a raw byte count, and `pids.max` as a raw integer. `addProcess(pid)` writes the container's PID to `cgroup.procs`.

`~CGroup()` calls `rmdir()` on the container's cgroup directory, which only succeeds once the cgroup is empty of member processes, guaranteed by the time the destructor runs since `waitpid()` has already reaped the container's process.

## 8. Data Design

This system has no database and no persistent application state beyond the filesystem itself. The "data" that matters is entirely made of on disk directory layouts and the JSON structures exchanged with Docker Hub.

### 8.1 On disk layouts

```
image-cache/<sanitized repository>/<tag>/<shortDigest>/   one directory per pulled layer,
                                                            extracted from that layer's tarball;
                                                            <sanitized repository> replaces '/'
                                                            with '_' (e.g. library_alpine)

overlay-data/<runtimePid>/upper/                           this container's writable layer
overlay-data/<runtimePid>/work/                            OverlayFS internal scratch directory
overlay-data/<runtimePid>/merged/                          the mounted, combined view

/sys/fs/cgroup/container-runtime/<containerPid>/           this container's cgroup v2 directory,
                                                            containing cpu.max, memory.max,
                                                            pids.max, and cgroup.procs
```

`image-cache/` is long lived and shared across every invocation of the tool (it is the layer cache). `overlay-data/<pid>/` and the cgroup directory are both created fresh per container and removed by RAII destructors once that container exits.

### 8.2 In memory data structures

```cpp
// src/registry/registry.hpp
struct PulledImage {
    std::vector<std::string> layerDirs;  // topmost layer first
};

// src/cgroups/cgroups.hpp
struct Limits {
    double cpuCores = 0.5;   // fraction of one core
    long memoryMb = 100;
    long pidsMax = 128;
};
```

`PulledImage` is the sole data handed from the registry client to the filesystem layer; it is deliberately just a list of paths, with no metadata about the image beyond what's needed to mount it (the config blob, digests, and history are never retained, consistent with Section 7.5's scope decision).

### 8.3 External data contracts (Docker registry v2 JSON)

The registry client parses three JSON shapes from Docker Hub, none of which this project defines; they are the actual Docker Distribution / OCI registry API schemas:

- **Auth token response**: `{ "token": "<bearer token string>" }`.
- **Manifest list / OCI image index** (when an image reference is multi-architecture): a `mediaType` field identifying it as a list, plus a `manifests` array of `{ digest, platform: { architecture, os } }` entries.
- **Concrete image manifest** (Docker v2 schema 2 or OCI equivalent): a `layers` array of `{ mediaType, size, digest }` entries, each `digest` a `sha256:<hex>` string identifying a downloadable blob at `/v2/<repo>/blobs/<digest>`.

The system's only outbound data contract is the CLI itself (Section 10).

## 9. Algorithms, Models, and Technical Methods

- **`cpu.max` quota computation.** `quota_us = cpuCores * period_us`, with a fixed 100ms period. `--cpu-limit 0.2` yields `quota_us = 20000`, written as `"20000 100000"`. Verified by measuring actual `cpu.stat` `usage_usec` deltas against wall clock time (Section 15, Section 16).
- **Layer order reversal.** A manifest's `layers` array is base first; OverlayFS's `lowerdir=` is topmost first. `pull()` collects layers in manifest order into `layerDirsBaseToTop`, then constructs the returned list via `result.layerDirs.assign(layerDirsBaseToTop.rbegin(), layerDirsBaseToTop.rend())`, a simple reverse iterator assignment. This was specifically exercised by testing a 4-layer image (`python:3.11-slim`); a 1-layer image (`alpine`) cannot detect an ordering bug, since there is nothing to order.
- **Manifest list resolution (recursive, depth 1 in practice).** `fetchManifest()` checks the returned document's `mediaType`; if it identifies a manifest list or OCI image index, the function scans its `manifests` array for the entry whose `platform.architecture == "amd64"` and `platform.os == "linux"`, then calls itself again with that entry's digest as the new reference. In practice this recurses exactly once, since a concrete manifest is never itself a list.
- **Container scoped IP allocation.** `ipHostOctet = 2 + (containerPid % 250)`, giving an address in `172.20.0.2` through `172.20.0.251` on the `172.20.0.0/24` subnet (`.0` is the network address, `.1` is the bridge/gateway). This is a derivation, not an allocator: there is no table of assigned addresses and no collision detection (see Section 17, Section 21).
- **Container ID derivation, two different schemes.** `Overlay` keys its directories by the *runtime process's own* `getpid()`, since it must be constructed before `clone()`, when the container's own PID doesn't exist yet. `CGroup` keys its directory by the *container's* PID (`clone()`'s return value), since it is constructed after `clone()` and that value is what `cgroup.procs` expects. Both are simply the most convenient unique value available at the point each subsystem needs one.
- **veth interface naming under `IFNAMSIZ`.** Linux caps interface names at 15 characters. Interface names are derived as `"veth" + std::to_string(containerPid % 100000) + "h"` (host end) or `"...c"` (container end, before it is renamed to `eth0` inside the namespace), keeping names safely under the limit at the cost of a small, accepted collision probability at very large PID values.
- **p50/p95 computation in `scripts/test-harness.sh`.** Wall clock timings for repeated runs are collected into a bash array, sorted numerically, and indexed at `n * 50 / 100` and `n * 95 / 100` to approximate the 50th and 95th percentiles without external statistics tooling.

## 10. APIs, Interfaces, and Data Contracts

### 10.1 Command line interface (the system's only user facing surface)

```
sudo ./build/container-runtime run [--cpu-limit N] [--memory-limit MB] [--pids-limit N] <image> [command] [args...]
```

| Flag/argument | Meaning | Default |
|---|---|---|
| `--cpu-limit N` | fraction of one CPU core | `0.5` |
| `--memory-limit MB` | memory cap in megabytes | `100` |
| `--pids-limit N` | max processes/threads in the container | `128` |
| `<image>` | Docker Hub reference, e.g. `alpine:latest` | required |
| `command` | command to exec inside the container | `/bin/sh` |
| `args...` | arguments to that command | none |

### 10.2 Internal C++ interfaces

```cpp
// src/namespaces/container.hpp
class Container {
public:
    Container(std::string imageRef, std::string command,
              std::vector<std::string> args, cgroups::Limits limits = {});
    void run();
};

// src/fs/overlay.hpp
class Overlay {
public:
    Overlay(std::vector<std::string> lowerDirs, std::string containerId);
    const std::string& mergedPath() const;
};

// src/fs/rootfs.hpp
class PivotRoot {
public:
    explicit PivotRoot(const std::string& newRoot);
};

// src/cgroups/cgroups.hpp
class CGroup {
public:
    CGroup(std::string containerId, const Limits& limits);
    void addProcess(pid_t pid) const;
};

// src/network/network.hpp
void ensureBridge();
class Veth {
public:
    Veth(pid_t containerPid, int ipHostOctet);
};

// src/registry/registry.hpp
PulledImage pull(const std::string& imageRef, const std::string& cacheDir);
```

Every one of these classes is constructed for its side effect (mounting, cloning, writing a limit, creating an interface) and torn down via its destructor; none exposes mutating methods beyond the narrow ones shown (`addProcess`, `run`).

### 10.3 External API contract: Docker registry v2

- `GET https://auth.docker.io/token?service=registry.docker.io&scope=repository:<repo>:pull`, unauthenticated, returns a bearer token.
- `GET https://registry-1.docker.io/v2/<repo>/manifests/<reference>`, with `Authorization: Bearer <token>` and an `Accept` header listing the four manifest media types this client understands.
- `GET https://registry-1.docker.io/v2/<repo>/blobs/<digest>`, with the same bearer token, returning a gzipped tarball.

This project is a consumer of that API only; it exposes no API of its own beyond the CLI.

## 11. Design Decisions, Alternatives, and Tradeoffs

| Decision | Alternative considered | Why rejected | Accepted cost |
|---|---|---|---|
| `clone()` with namespace flags | `fork()` then `unshare()` | Two step approach has a real window where the child runs before it has unshared into new namespaces | None significant; `clone()` is a direct, equally well documented syscall |
| `pivot_root`, not `chroot` | `chroot()` | Doesn't replace the actual root mount; escapable via pre-`chroot` file descriptors or mount tricks | Slightly more setup code (bind mount, `.old_root` dance) |
| Mount OverlayFS in the parent, before `clone()` | Mount inside the child, after `clone()` | Would work and be more self-contained, but forces `PivotRoot` to special case "is the mount already there" | `Container::run()` must sequence overlay construction before `clone()`, a coupling that has to be understood, not discovered |
| Shell out to `ip`/`iptables`/`nsenter` | Raw rtnetlink via `<linux/netlink.h>` | Portfolio priority: code reads like a transcript of exactly what a human operator would run | Exit code/string parsing fragility a structured netlink API wouldn't have; named explicitly as a limitation (Section 21) |
| CMake `FetchContent` for `nlohmann/json`, pinned to `v3.11.3` | Vendor a copied single header file | Keeps dependency provenance explicit (a real upstream tag, not an unversioned copy sitting in the repo) | Requires network access during the build's configure step |
| `libcurl` via `find_package(CURL REQUIRED)` | Hand rolled HTTP/TLS over raw sockets | Implementing HTTP and TLS correctly is a large, security sensitive project on its own, orthogonal to this project's purpose | A system dependency (`libcurl4-openssl-dev`) is required |
| PID-derived container IDs (two different schemes, Section 9) | A single ID scheme, or a UUID/counter | Avoids introducing shared allocator state; uses whatever unique value already exists at the point each subsystem needs one | Two different ID schemes in the same codebase, which looks inconsistent until the reason is understood |
| PID-derived IP allocation, `2 + (pid % 250)` | A real IPAM table with lease tracking | Sufficient to demonstrate the property being proven (isolated containers can reach each other and the internet) without building allocator state | No collision detection; two containers can theoretically land on the same address |
| Bind mount the host's `/dev` into the container | A private `devtmpfs` or explicit `mknod` allow list per container | Simpler, and Linux has no separate device namespace concept to isolate against regardless | Container can see and potentially open every device node the host can (Section 17, Section 21) |
| RAII ownership for every subsystem, tied to `Container::run()`'s stack frame | A daemon that reconciles kernel state against expected state independently of any one process's lifetime | Matches this project's one invocation-per-container scope; a daemon is a materially larger project | Cleanup only runs on normal process exit; a `SIGKILL` to the runtime itself leaves no opportunity for any destructor to run (Section 17, Section 21) |
| A single flat `flock()` around `ensureBridge()` | Per-operation locking, or a lock-free retry loop on `EEXIST` | Simplest correct fix for the specific check-then-act race found (Section 12) | Serializes all concurrent container starts on bridge setup specifically, a small, accepted throughput cost |

## 12. Implementation and Project Evolution

The project was built in six main stages, each building directly on the last, followed by a hardening pass:

1. **Namespace isolation.** `clone()` with `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNS`, verified via a shell reporting itself as PID 1 and an independent hostname.
2. **Filesystem isolation.** `pivot_root` into an extracted Alpine minirootfs, plus a fresh `/proc` mount.
3. **Resource limits.** cgroup v2 `cpu.max` and `memory.max`, verified with a real memory hog and a measured CPU cap.
4. **Layered storage.** OverlayFS combining a shared lower layer with a per-container upper layer, verified with two concurrent containers.
5. **Networking.** veth pairs, a shared bridge, and NAT, verified with outbound internet access and container to container connectivity.
6. **Registry client.** A real Docker Hub v2 client replacing the bootstrap tarball entirely, verified against two different real images.

A subsequent hardening pass, prompted by a self-review against the kind of questions a systems-focused interviewer would ask, added `pids.max` and a real fork bomb test, fixed a concurrency race in bridge setup, fixed a missing `/dev` gap discovered while building the fork bomb test, and added an automated repeatable test harness (`scripts/test-harness.sh`).

### Real bugs found and fixed, in the order they were hit

1. **`pivot_root` returned `EINVAL`.** Diagnosed as Ubuntu's default `MS_SHARED` root mount propagating events in a way `pivot_root` refuses. Fixed with an `MS_REC | MS_PRIVATE` remount before anything else (Section 7.3).
2. **Overlay cleanup left an empty directory behind.** `~Overlay()` removed `upper/work/merged` but not their shared parent. Found by manually inspecting `overlay-data/` after a test container exited. Fixed with one more `remove()` call.
3. **A stray `main.cpp` at the repository root.** An `scp` invocation meant to copy `main.cpp` into `src/` instead placed a duplicate at the repository root, discovered via `git status` showing an unexpected untracked file before a commit. Removed in a follow up commit.
4. **The networking readiness race.** Recognized by reasoning through the sequence, not by observing a live failure: `clone()` returns immediately, network setup needs the child's PID, and nothing stopped the child from running ahead of that setup. Fixed with the readiness pipe described in Section 6, which also retroactively fixed a same shaped, previously unaddressed cgroup timing race.
5. **The bridge check-then-act race.** Recognized by design review rather than a live crash, and fixed with a `flock`-based lock. Verified afterward with two genuinely simultaneous container launches against a freshly deleted bridge.
6. **Missing `/dev` broke the fork bomb test.** `/dev/null` doesn't exist in a freshly extracted image layer; `ash`'s job control needs it to background processes, so the fork bomb test failed silently (zero forks attempted) rather than being stopped by `pids.max`. Diagnosed from the shell's own error message, fixed by bind mounting the host's `/dev` at the correct point in the `pivot_root` sequence.
7. **The fork bomb test itself needed two iterations.** The first attempt defined the bomb function in one shell invocation and tried to invoke it from a separate one, producing `sh: bomb: not found`, a test-harness mistake rather than a runtime bug, fixed by defining and invoking the function within the same `sh -c` call.

## 13. Operational Guide

### Requirements

- A real Linux host, not macOS or WSL. This calls Linux syscalls (`clone`, `mount`, `pivot_root`) directly.
- root/sudo. Namespaces, cgroups, and networking all need it.
- A C++20 compiler, CMake, and `libcurl4-openssl-dev`.
- Outbound internet access, to pull images from Docker Hub.

### Building

```
mkdir build && cd build
cmake ..    # fetches nlohmann/json via CMake FetchContent on first run
make
```

### Usage

```
sudo ./build/container-runtime run [--cpu-limit N] [--memory-limit MB] [--pids-limit N] <image> [command] [args...]
```

```
sudo ./build/container-runtime run alpine:latest
sudo ./build/container-runtime run --cpu-limit 0.2 --memory-limit 50 alpine:latest /bin/sh
sudo ./build/container-runtime run python:3.11-slim python3
```

### Project layout

```
src/main.cpp        entrypoint, CLI parsing
src/namespaces/      namespace isolation
src/cgroups/         cgroup v2 CPU, memory, and pids limits
src/fs/              pivot_root, OverlayFS
src/network/         veth, bridge, NAT
src/registry/        Docker Hub registry client
scripts/             bootstrap and test-harness scripts
```

### Running the test harness

```
sudo bash scripts/test-harness.sh
```

Builds on the manual verification in this document with a repeatable script covering image compatibility, cold versus cached pull latency, repeated run timing statistics, and a leak check (see Section 14 and Section 16 for what it checks and what it found).

## 14. Testing and Validation

Validation of this system happened at two levels: targeted, per-subsystem correctness checks performed manually against real kernel behavior, and an automated repeatable harness added during the hardening pass.

### Per-subsystem validation approach

- **Namespaces**: checked that a shell running inside the container reports itself as PID 1 (`echo $$`) and that setting its hostname does not affect the host's, in a separate concurrent shell.
- **Filesystem**: checked that `/etc/os-release` inside the container reports the pulled image's identity, not the host's, and that the relocated old root (`/.old_root`) is genuinely gone, not just hidden.
- **cgroups**: validated each limit against a real adversarial workload rather than trusting the configuration write to have taken effect: a real memory hog binary for `memory.max`, a real CPU bound busy loop measured against `cpu.stat` for `cpu.max`, and a real fork bomb for `pids.max`.
- **OverlayFS**: ran two containers concurrently, sharing one base image, and checked that a file written in one container's upper layer never appears in the other's view, and that disk usage of the shared base layer does not grow.
- **Networking**: checked outbound connectivity (`ping 8.8.8.8`) and container to container connectivity (two concurrent containers pinging each other's derived IPs), and specifically re tested the bridge race fix by launching two containers with zero delay against a freshly deleted bridge.
- **Registry client**: pulled and ran two different real images with different base operating systems and different layer counts (`alpine:latest`, 1 layer; `python:3.11-slim`, 4 layers, Debian based) specifically to exercise the layer ordering logic, which a single layer image cannot validate.

### Automated harness (`scripts/test-harness.sh`)

Added specifically to convert the manual, one-off checks above into something repeatable. It performs, in order: an image compatibility sweep across four different images; a cold pull versus cached pull latency comparison; twenty repeated runs of the same cached image to compute p50/p95 startup latency; and a post run leak check inspecting `/sys/fs/cgroup/container-runtime/`, `ip link show type veth`, `overlay-data/`, and active overlay mounts for anything left behind. Every run's pass or fail status is tallied into a final summary. See Section 16 for the actual numbers this produced.

## 15. Experimental Methodology

This section documents *how* the performance oriented measurements in Section 16 were produced, so the numbers can be interpreted correctly and reproduced.

- **CPU limit accuracy** was measured by sampling the container's own cgroup `cpu.stat` file's `usage_usec` counter immediately before and after a fixed 5-second wall clock window, while a CPU bound busy loop ran inside the container capped at `--cpu-limit 0.2`. The delta in `usage_usec` (in microseconds of actual CPU time consumed) was divided by the wall clock window to get a percentage, rather than trusting a single instantaneous sample from `top`, which can be misleading for a bursty workload.
- **Memory limit enforcement** was measured by running a small, purpose-built static binary (`memhog`, not checked into the repository) that allocates and touches (`malloc` plus `memset`) 10 MB chunks in a loop, and observing both the program's own output (where it stopped) and the host's kernel log (`dmesg`) for the specific OOM-killer invocation and its stated constraint.
- **Process count (pids) limit enforcement** was measured by running an actual fork bomb (`bomb() { bomb & bomb & }; bomb`, wrapped in `timeout 5` so the test terminates even without a working limit) inside a container capped at `--pids-limit 20`, and observing the shell's own fork failure error messages plus the host's `uptime` load average immediately afterward.
- **Startup latency (cold pull, cached pull, p50/p95)** was measured with the shell builtin `time`, wrapping the full `container-runtime run` invocation including image resolution, mounting, namespace setup, and command execution, not just the exec step. Cold pull timing was produced by first deleting the relevant image's cache directory; cached pull and repeated run timing reused an already-populated cache. Twenty repeated runs of the same cached image were collected into an array, sorted, and indexed for p50/p95 as described in Section 9.
- **Networking latency** was measured with standard ICMP `ping`, comparing round trip time for a container to container ping (over the local bridge) against a container to internet ping (through NAT), on the same host at the same time, to make the comparison meaningful.
- **Layer sharing disk usage** was measured with `du -sh` against the shared lower layer's directory, taken once before any container had run and again after two containers had run and exited, on the same filesystem, to rule out any difference being explained by something other than the overlay mechanism itself.
- **Environment for all measurements**: a single AWS EC2 `t3.micro` instance (single vCPU) running Ubuntu, kernel `7.0.0-1006-aws`, described further in Section 2. No measurement was taken on a different or more powerful host, so absolute latency and CPU cap versus uncapped comparisons are relative to that specific single vCPU environment.

## 16. Results and Observed Behavior

All of the following is raw evidence from a real EC2 Ubuntu host, reproduced in full so the claims in this document can be checked rather than taken on faith.

### 16.1 Namespaces: PID and hostname isolation

```
$ echo 'echo PID inside namespace: $$; hostname' | sudo ./build/container-runtime run /bin/sh
PID inside namespace: 1
container

$ hostname   # on the host, in a separate shell
ip-172-31-9-167
```

`echo $$` prints `1` inside the container (its own PID namespace) and `hostname` differs from the host's own, which is unaffected.

### 16.2 Filesystem: `pivot_root` isolation

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

`/.old_root` doesn't exist, so there is no path back to the host. `ps aux` shows only the container's own two processes. The host's own filesystem and process list are completely unaffected.

### 16.3 cgroups: memory limit, a real OOM kill

A small memory hog (`malloc` plus `memset` in a loop) capped at 50 MB:

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

`constraint=CONSTRAINT_MEMCG` confirms the container's own cgroup limit killed it, not a host wide OOM. Host `uptime` immediately after showed `load average: 0.03, 0.01, 0.00`, unaffected.

### 16.4 cgroups: CPU limit, measured, not asserted

```
usage_usec delta: 1000143   (over 5.013890114 s of wall clock time)
1000143 µs / 1,000,000 = 1.000143 s of CPU time consumed
1.000143 / 5.013890114 = 0.1995  →  19.95% actual CPU usage
```

19.95% against a 20% cap. The same loop uncapped pins 100% of this single vCPU host.

### 16.5 cgroups: pids limit, a real fork bomb

```
$ echo "timeout 5 sh -c 'bomb() { bomb & bomb & }; bomb'" \
    | sudo ./build/container-runtime run --pids-limit 20 alpine:latest /bin/sh
sh: can't fork: Resource temporarily unavailable
sh: can't fork: Resource temporarily unavailable
sh: can't fork: Resource temporarily unavailable

$ uptime   # immediately after
load average: 0.00, 0.00, 0.00
```

The kernel refuses new forks (`EAGAIN`) once the 20-process cap is hit. The whole attempt completes in under a second with host load at `0.00`.

### 16.6 OverlayFS: isolation and no duplication

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

Container B never sees `from_a.txt`, written concurrently by A into A's own upper layer. After both exit:

```
$ ls overlay-data/
(empty)
```

Disk usage of the shared base layer, before and after both containers ran:

```
$ du -sh rootfs/alpine
9.5M    rootfs/alpine   (unchanged both times)
```

### 16.7 Networking: outbound access and container to container

```
$ sudo ./build/container-runtime run /bin/sh
/ # ping -c3 8.8.8.8
64 bytes from 8.8.8.8: seq=0 ttl=117 time=1.476 ms
3 packets transmitted, 3 packets received, 0% packet loss
```

Two containers running concurrently, one pinging the other over the bridge:

```
$ ping -c3 172.20.0.3
64 bytes from 172.20.0.3: seq=0 ttl=64 time=0.067 ms
3 packets transmitted, 3 packets received, 0% packet loss
```

About 0.06ms bridge local latency versus about 1.5ms to the real internet via NAT, consistent with direct L2 delivery. Both containers reached each other *and* the internet at the same time. After both exit:

```
$ ip link show type veth
(nothing, no leftover veth interfaces)
$ iptables -t nat -L POSTROUTING -n | grep MASQUERADE
MASQUERADE  all  --  172.20.0.0/24        0.0.0.0/0   (present exactly once)
```

### 16.8 Registry: two different real images, start to finish

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

A real Python interpreter from a real pulled image, confirming its own OS is Debian trixie, not Alpine, not a locally built rootfs. Layer counts differ correctly by image:

```
$ find image-cache/library_alpine -maxdepth 2   # 1 layer
$ find image-cache/library_python -maxdepth 2   # 4 layers
$ du -sh image-cache/library_alpine image-cache/library_python
8.7M    image-cache/library_alpine
135M    image-cache/library_python
```

### 16.9 Automated harness: repeated run summary

```
=== image compatibility ===
alpine:latest: 0.79s   python:3.11-slim: 0.74s
busybox:latest: 0.73s  debian:bookworm-slim: 0.77s

=== cold vs. cached pull latency (alpine:latest) ===
cold pull: 1.06s   cached: 0.71s

=== repeated run stats (alpine:latest, cached, x20) ===
p50=0.736s  p95=0.920s  min=0.713s  max=0.920s

=== leak check after all runs ===
clean: no leaked mounts, cgroups, veth interfaces, or overlay dirs

=== summary ===
pass=26 fail=0 total=26  pass_rate=100.0%
```

## 17. Security, Reliability, and Failure Handling

### Security boundary: real but partial

`pivot_root` plus five namespaces gives filesystem, PID, hostname, IPC, and network isolation. But the container process runs as **actual root**, with the full set of Linux capabilities (`CAP_SYS_ADMIN`, `CAP_SYS_MODULE`, and so on), in the same user ID space as the host. There is no `CLONE_NEWUSER` (user namespace) remapping root inside the container to an unprivileged UID outside it, no capability dropping before `exec`, and no seccomp filter restricting available syscalls. This is the single biggest gap between this runtime and a real security boundary: a process inside this container that finds a way to reach a host resource (a device node via the bind mounted `/dev`, a `mount()` syscall, a kernel module load) is not stopped by anything this project builds, because it has the actual privilege to do those things.

Closing this fully requires, in increasing order of effort: dropping capabilities before `exec` (moderate, self-contained, doesn't interact with other subsystems); full user namespace UID and GID mapping (substantial, interacts with cgroup `cgroup.procs` write semantics, OverlayFS permission handling, and `pivot_root`, all of which currently assume real root); and a seccomp allow list (separate effort, with a risk of breaking functionality if too strict). None of these are implemented (see Section 21).

The bind mounted `/dev` compounds this: the container sees the same device nodes the host does, rather than a minimal, per-container set, since Linux has no device namespace concept and isolating device access is properly a cgroup device controller job this project doesn't configure.

### Error handling patterns

Every subsystem constructor throws a `std::system_error` or `std::runtime_error` on failure (`throwErrno()` in `PivotRoot`, exceptions from `libcurl`/JSON parsing failures in the registry client, `runCmd()` throwing on a non-zero exit code from a shelled out network command). `Container::run()` wraps parent side setup in a `try`/`catch`; on failure, it still writes the readiness byte to unblock the child (so the child can exit with its own error rather than hang forever) and still calls `waitpid()`. `main()` wraps the whole pipeline in a top level `try`/`catch` so any exception becomes a clean, readable error message and a non-zero exit code rather than an unhandled `std::terminate()`.

`PivotRoot`'s constructor distinguishes reversible setup steps (the bind mount onto itself and the `.old_root` directory creation, both trivially undone) from irreversible ones (everything from `pivot_root()` itself onward); a failure in the irreversible phase is treated as fatal to that container rather than something to roll back, matching the tradeoff real container runtimes make.

### Reliability: RAII cleanup and its limit

Every subsystem (`Overlay`, `PivotRoot`, `CGroup`, `Veth`) is a class whose constructor sets a resource up and whose destructor tears it down, because a container's lifecycle is exactly the lifetime of one C++ stack frame, `Container::run()`. This makes cleanup automatic on every exit path, including exception paths.

The real, load-bearing limitation: **RAII cleanup only runs if the process exits through normal C++ control flow.** If `container-runtime` itself (not the containerized command) receives `SIGKILL`, the process is terminated by the kernel with no opportunity to run any destructor, C++ or otherwise, and a stray cgroup or overlay directory can be left behind. This is a structural property of tying resource lifetime to process lifetime, not a bug in this implementation; it is the reason real container runtimes run a separate long lived daemon that reconciles actual kernel state against expected state on startup, which this project deliberately does not implement (see Section 21).

## 18. Performance, Scalability, and Cost

### Performance

Measured startup latency (Section 16.9) is dominated by the fixed cost of image resolution, namespace setup, mounting, and cgroup/network configuration rather than by the trivial commands used in testing: cold pull of `alpine:latest` took 1.06s, a cached pull took 0.71s, and repeated cached runs clustered tightly (p50 0.736s, p95 0.920s, min 0.713s, max 0.920s over 20 runs), indicating low run to run variance on this host. CPU and memory limits were shown to hold within measurement noise of their configured values (19.95% actual against a 20% cap).

### Scalability

This project was not built to scale past a handful of concurrently running containers on one host, and several design decisions reflect that explicitly: IP allocation is a PID-derived formula with no collision detection (Section 9, Section 21), there is no daemon tracking multiple containers across invocations (Section 3), and every container starting invocation contends on a single `flock()` around bridge setup (Section 7.4, accepted as a small serialization cost). Docker Hub's registry API itself imposes anonymous pull rate limits that this client does not track or back off from, which would become a real constraint under any kind of repeated automated pulling at scale.

### Cost

Development and all measurements in this document were performed on a single AWS EC2 `t3.micro` instance, priced at a fraction of a cent per hour and, on a new AWS account, likely covered by the 12-month Free Tier. The instance was terminated once the codebase and all verification evidence were safely committed to GitHub, so the project carries no ongoing infrastructure cost. There is no other cost dimension (no managed service, no storage service, no paid API) in this project's design.

## 19. Deployment, Monitoring, and Maintenance

### Deployment

This project is a command line tool, not a service, and has no "deployment" in the sense of a long running process exposed to traffic. Its only environment requirement (a real Linux host with root access and cgroup v2 delegated) was satisfied by a single EC2 instance used for development and verification; running it elsewhere means satisfying that same environment requirement (Section 4) on any other real Linux host, and rebuilding from source (Section 13).

### Monitoring

There is no monitoring, metrics endpoint, or structured logging in the current implementation. Observability during development was entirely manual: `dmesg` for kernel-level events (the OOM killer), `cpu.stat`/`pids.current` for live cgroup state, `ip`/`mount` for live network and filesystem state, and the tool's own stderr for its explicit error messages. A production version of this tool would need at minimum structured logging of each container's lifecycle events and resource usage over time; this is explicitly not built (Section 21).

### Maintenance

The two directories that grow over time and require periodic attention are `image-cache/` (every distinct image and tag pulled is cached indefinitely, with no eviction policy) and, in the failure case described in Section 17, stray `overlay-data/<pid>/` or `/sys/fs/cgroup/container-runtime/<pid>/` directories left behind by a `SIGKILL` to the runtime process itself, which currently require manual `rm`/`rmdir` to clean up. Neither an eviction policy nor an automatic reconciliation pass is implemented (Section 21).

## 20. Interpretation and Lessons Learned

The central lesson of this project is that container runtimes are hard because of *integration*, not because of any one kernel primitive. Every individual syscall used here (`clone`, `pivot_root`, cgroup filesystem writes, `mount`) is well documented and, in isolation, straightforward. Every real bug found during construction (Section 12) was a sequencing or timing bug between two otherwise correct subsystems: a mount propagation setting `pivot_root` didn't know about, a cleanup routine that forgot a directory `pivot_root`'s own PID scoping wouldn't have caught, a synchronization gap between `clone()` returning and the parent finishing setup that neither namespace nor cgroup code alone would surface. Building each primitive from scratch, rather than trusting a framework, was what made those interaction bugs visible at all.

A second lesson was about **honest scoping under time pressure**: several real gaps (no user namespaces, no capability dropping, no seccomp, a concurrency race in bridge setup, a missing device filesystem) were found not by accident but by deliberately adopting an adversarial reviewer's perspective on the finished system and asking what a careful, skeptical reader would probe next. Two of those (the bridge race, the missing `/dev`) turned into real fixed bugs with real regression tests. One (the security boundary) remains open, and the lesson there was that disclosing a limitation precisely and explaining what it would cost to close is a materially stronger position than either hiding the gap or leaving it undiscovered.

A third lesson was about RAII specifically: it is an excellent default for resource cleanup, but its guarantee is scoped to the lifetime of the process running the destructors, not to the lifetime of the kernel resources it manages. That distinction, obvious in hindsight, only became concrete once it was stated as an explicit design question ("what happens if this process is killed, not exited?") rather than assumed away.

## 21. Limitations, Known Issues, Technical Debt, and Future Work

This section is deliberately exhaustive rather than reassuring. Everything listed here is either a conscious tradeoff already explained above, or a gap not yet closed.

**Security boundary is real but partial.** No `CLONE_NEWUSER` user namespace remapping, no capability dropping before `exec`, no seccomp filter. A process inside the container that finds a way to reach a host resource is not stopped by anything this project builds, because it runs with real root's actual privileges. Closing this fully requires, in increasing order of effort: dropping capabilities before `exec` (moderate); full user namespace UID/GID mapping (substantial, touches cgroup, OverlayFS, and `pivot_root` code that currently assumes real root); and a seccomp allow list (separate effort, risk of breaking functionality if too strict). None of these are implemented.

**Bind mounted `/dev` is not a private device namespace.** The container sees the same device nodes the host does, rather than a minimal, per-container set. Linux has no device namespace concept; isolating device access is properly a cgroup device controller job, not configured here. Same underlying gap as the point above, viewed from a different angle.

**No image config parsing.** No default `CMD`, `ENTRYPOINT`, `ENV`, or working directory; every `run` requires an explicit command.

**No digest or signature verification** of pulled layers. A production registry client verifies each blob's SHA256 against its manifest declared digest before trusting it; this one trusts TLS alone.

**No private registry or non-anonymous auth support.** Only Docker Hub's public, anonymous pull token flow.

**No handling of Docker Hub's anonymous pull rate limits.** No tracking, backoff, or clear error surfacing if the limit is hit.

**Simplified IP allocation.** PID-derived, not tracked, collision prone at scale. Fine for a handful of concurrent demo containers, not for production scale.

**RAII cleanup requires normal process exit.** A `SIGKILL` to `container-runtime` itself leaves no opportunity for any destructor to run, and can leave a stray cgroup or overlay directory behind. Not fixable without a separate reconciliation or garbage collection pass, which real runtimes solve with a persistent daemon. Not implemented here.

**No multi-container orchestration.** One container per invocation. No daemon, no equivalent of `docker-compose`, no tracking of multiple running containers across separate invocations beyond the shared bridge and cgroup base directory.

**Networking uses shelled out `ip`, `iptables`, and `nsenter`**, not raw rtnetlink. A deliberate readability tradeoff for a portfolio project, at the cost of exit code and string parsing fragility a netlink based implementation wouldn't have.

**No CI, only a local test harness** (`scripts/test-harness.sh`). GitHub hosted Actions runners restrict exactly the privileged operations this project needs (cgroup v2 delegation, `unshare`, `pivot_root` from inside their own already-containerized environment). Real CI would likely require a self-hosted runner with genuine root access on a real Linux host, which hasn't been set up.

**Network throughput overhead has never been measured.** Connectivity (ping latency, packet loss) has been verified; sustained throughput through the veth/bridge/NAT path (via `iperf3`, for example) has not.

**Single-architecture assumption.** The registry client's manifest list resolution hardcodes `architecture: amd64, os: linux`; it has no path for other architectures even though the manifest lists it parses often contain them.

**No monitoring, metrics, or structured logging** beyond ad hoc stderr messages and manual kernel log inspection (Section 19).

**No image-cache eviction policy.** `image-cache/` grows indefinitely with every distinct image and tag ever pulled.

**Measured evidence for abnormal exit cleanup would currently show failure, not success.** Deliberately not added as an automated test without first fixing the underlying gap (RAII cleanup requires normal process exit, above). A metric that's cheap to produce but currently reads as failing isn't worth adding to a test suite on its own without the corresponding fix landing at the same time.

## 22. Conclusion

This project set out to prove that the mechanics underneath Docker can be built directly from Linux kernel primitives, with no container framework, and to prove every claim about the result with real, reproducible measurement rather than assertion. That goal was met: namespace isolation, filesystem isolation via `pivot_root` and OverlayFS, cgroup v2 resource limits verified against real adversarial workloads (a memory hog and a fork bomb), a working virtual network with NAT, and a real Docker Hub registry client that runs genuinely different, live pulled images end to end, are all demonstrated with raw evidence in Section 16.

What remains open is stated plainly rather than buried: this runtime provides real namespace isolation but not real privilege isolation, since the container process still runs as actual root with the full set of Linux capabilities and no user namespace, capability dropping, or seccomp filtering stands between it and the host. Everything else left unfinished, a daemon, orchestration, private registries, layer verification, CI, is a deliberate scope boundary rather than an oversight, and each is named specifically in Section 21 along with what closing it would require. Read together with the codebase and the README, this document is intended to be a complete, standalone account of what was built, why it was built that way, what it measurably does, and exactly where its edges are.
