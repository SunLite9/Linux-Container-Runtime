# Linux Container Runtime

A container runtime similar to Docker, built from scratch on raw Linux
kernel primitives: namespaces, cgroups v2, OverlayFS, veth networking,
and a real Docker Hub registry client, in C++20, with no container
framework libraries.

```
sudo ./build/container-runtime run alpine:latest /bin/sh
```

This pulls the real `alpine` image from Docker Hub and drops you into an
isolated, resource capped, networked shell running it.

See [DESIGN.md](DESIGN.md) for the reasoning behind how this is built:
alternatives considered, tradeoffs accepted, and what broke along the way.

## Architecture

1. **Pull** the image from Docker Hub and cache its layers (`src/registry/`)
2. **Mount** those layers with OverlayFS: a shared read only base plus a private writable layer (`src/fs/overlay.cpp`)
3. **Clone** into new PID, mount, UTS, IPC, and network namespaces (`src/namespaces/`)
4. **pivot_root** into the merged filesystem (`src/fs/rootfs.cpp`)
5. **Apply cgroup v2 limits**: CPU, memory, process count (`src/cgroups/`)
6. **Attach networking**: veth pair, bridge, NAT (`src/network/`)
7. **Exec** the requested command

Steps 1, 2, 5, and 6 run in the parent process, around a single
`clone()` call. The child waits on a small pipe until that setup
finishes before it execs anything.

## Requirements

- A real Linux host, not macOS or WSL. This calls Linux syscalls
  (`clone`, `mount`, `pivot_root`) directly.
- root/sudo. Namespaces, cgroups, and networking all need it.
- A C++20 compiler, CMake, and `libcurl4-openssl-dev`.
- Outbound internet access, to pull images from Docker Hub.

## Building

```
mkdir build && cd build
cmake ..    # fetches nlohmann/json via CMake FetchContent on first run
make
```

## Usage

```
sudo ./build/container-runtime run [--cpu-limit N] [--memory-limit MB] [--pids-limit N] <image> [command] [args...]
```

- `--cpu-limit N`: fraction of one CPU core (default `0.5`)
- `--memory-limit MB`: memory cap in megabytes (default `100`)
- `--pids-limit N`: max processes/threads in the container (default `128`)
- `<image>`: a Docker Hub reference, e.g. `alpine:latest`, `python:3.11-slim`
- `command`: defaults to `/bin/sh`

```
sudo ./build/container-runtime run alpine:latest
sudo ./build/container-runtime run --cpu-limit 0.2 --memory-limit 50 alpine:latest /bin/sh
sudo ./build/container-runtime run python:3.11-slim python3
```

## Project layout

```
src/main.cpp        entrypoint, CLI parsing
src/namespaces/      namespace isolation
src/cgroups/         cgroup v2 CPU, memory, and pids limits
src/fs/              pivot_root, OverlayFS
src/network/         veth, bridge, NAT
src/registry/        Docker Hub registry client
```

## What each piece does

- **Namespaces**: `clone()` puts the container in its own PID, hostname,
  IPC, mount, and network namespace in one syscall. It becomes PID 1 and
  sees none of the host's processes, mounts, or network interfaces.
- **Filesystem**: the image's layers are combined into one OverlayFS
  mount (a read only base plus a fresh writable layer), and `pivot_root`
  swaps that in as the container's actual root. This is a real security
  boundary, not just a `chroot`, so there's no path back to the host
  filesystem.
- **cgroups v2**: every container gets its own cgroup capping CPU
  (`cpu.max`), memory (`memory.max`), and process count (`pids.max`).
- **Networking**: each container gets a veth pair wired to a shared
  Linux bridge (`cr0`) with NAT, so containers can reach each other and
  the internet.
- **Registry client**: a small Docker Hub v2 client (`libcurl` plus
  `nlohmann/json`) that resolves multi architecture manifests, downloads
  layers, and caches them locally, so any public image can be pulled and
  run.

## Verification highlights

Everything below was measured on a real EC2 Ubuntu host, not asserted.
Full transcripts and methodology are in [DESIGN.md](DESIGN.md).

- **PID and hostname isolation**: the container's shell is PID 1 in its
  own namespace; the host's hostname is untouched.
- **Filesystem isolation**: no path back to the host after `pivot_root`,
  confirmed via a removed `/.old_root` and a mismatched `/etc/os-release`.
- **Memory limit**: a memory hog was OOM killed by the kernel at exactly
  the configured limit (`dmesg`: `mem_cgroup_out_of_memory`); the host was
  unaffected.
- **CPU limit**: a busy loop capped at 20% measured **19.95%** actual
  usage via `cpu.stat`.
- **Process limit**: a real fork bomb capped at 20 processes failed with
  kernel `EAGAIN`, completing in under a second with host load at `0.00`.
- **Layer isolation**: two containers sharing one base image wrote to
  fully isolated upper layers, with zero extra disk usage from the shared
  base.
- **Networking**: outbound internet access works via NAT; two
  concurrently running containers reach each other directly over the
  bridge in about 0.06ms.
- **Real images**: `alpine:latest` and `python:3.11-slim` (a different
  base OS, with multiple layers) both pulled live from Docker Hub and run
  start to finish.
- **Repeatability**: `scripts/test-harness.sh` shows a 100% pass rate over
  26 runs, zero leaked mounts, cgroups, or veth interfaces, and a p50
  startup of 0.736s (p95 0.920s).

## Known limitations

The most important ones; see the full list and reasoning in
[DESIGN.md](DESIGN.md):

- No user namespaces, capability dropping, or seccomp. The container
  runs as real root with full capabilities. Namespace isolation is real;
  privilege isolation is not.
- No image config parsing (`CMD`, `ENTRYPOINT`, `ENV`). Every run needs
  an explicit command.
- No layer digest verification, and no private registries.
- Cleanup relies on normal process exit. A `SIGKILL` sent to the runtime
  itself can leave a stray cgroup or mount behind.
- One container per invocation. There is no daemon and no orchestration.
