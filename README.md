# Linux Container Runtime

A container runtime similar to Docker, built from scratch on raw Linux kernel primitives, in C++20, with no container framework libraries.

See [DESIGN.md](DESIGN.md) for the full reasoning, alternatives considered, and measured results behind everything summarized here.

## Problem and motivation

Almost every engineer uses Docker. Very few have built the mechanics underneath it, since there is rarely a practical reason to when `runc` and `containerd` already exist and are correct. This project builds those mechanics directly, using nothing but raw Linux syscalls, to demonstrate real systems level understanding of process isolation, resource control, filesystem layering, and networking, rather than just familiarity with existing tools.

## Key features

- Namespace isolation via a single `clone()` call (PID, mount, UTS, IPC, network)
- Filesystem isolation via `pivot_root`, a real security boundary, not just `chroot`
- OverlayFS layered storage, so containers share a base image without duplicating it on disk
- cgroup v2 resource limits (CPU, memory, process count), each verified against a real adversarial workload, not just configured
- Virtual networking (veth pair, Linux bridge, NAT) with outbound internet and container to container connectivity
- A real Docker Hub registry client that pulls and runs genuine, unmodified images

## Architecture or workflow

1. **Pull** the image from Docker Hub and cache its layers (`src/registry/`)
2. **Mount** those layers with OverlayFS: a shared read only base plus a private writable layer (`src/fs/overlay.cpp`)
3. **Clone** into new PID, mount, UTS, IPC, and network namespaces (`src/namespaces/`)
4. **pivot_root** into the merged filesystem (`src/fs/rootfs.cpp`)
5. **Apply cgroup v2 limits**: CPU, memory, process count (`src/cgroups/`)
6. **Attach networking**: veth pair, bridge, NAT (`src/network/`)
7. **Exec** the requested command

Steps 1, 2, 5, and 6 run in the parent process, around a single `clone()` call. The child waits on a small pipe until that setup finishes before it execs anything.

## Tech stack

- C++20
- CMake, with `nlohmann/json` fetched via CMake `FetchContent`
- `libcurl` for HTTP
- Linux kernel interfaces: `clone`, `pivot_root`, cgroup v2, `mount`, `ip`/`iptables`/`nsenter`
- Docker Hub registry v2 API
- Built and verified on an AWS EC2 Ubuntu instance

## Installation and setup

Requirements:

- A real Linux host, not macOS or WSL
- root/sudo
- A C++20 compiler, CMake, and `libcurl4-openssl-dev`
- Outbound internet access, to pull images from Docker Hub

```
git clone https://github.com/SunLite9/Linux-Container-Runtime.git
cd Linux-Container-Runtime
mkdir build && cd build
cmake ..    # fetches nlohmann/json via CMake FetchContent on first run
make
```

## Usage examples

```
sudo ./build/container-runtime run [--cpu-limit N] [--memory-limit MB] [--pids-limit N] <image> [command] [args...]
```

```
sudo ./build/container-runtime run alpine:latest
sudo ./build/container-runtime run --cpu-limit 0.2 --memory-limit 50 alpine:latest /bin/sh
sudo ./build/container-runtime run python:3.11-slim python3
```

- `--cpu-limit N`: fraction of one CPU core (default `0.5`)
- `--memory-limit MB`: memory cap in megabytes (default `100`)
- `--pids-limit N`: max processes/threads in the container (default `128`)

## Results and metrics

Everything below was measured on a real host, not asserted. Full transcripts and methodology are in [DESIGN.md](DESIGN.md).

- **PID and hostname isolation**: the container's shell is PID 1 in its own namespace; the host's hostname is untouched.
- **Filesystem isolation**: no path back to the host after `pivot_root`.
- **Memory limit**: a memory hog was OOM killed by the kernel at exactly the configured limit; the host was unaffected.
- **CPU limit**: a busy loop capped at 20% measured **19.95%** actual usage.
- **Process limit**: a real fork bomb capped at 20 processes failed safely, completing in under a second with host load at `0.00`.
- **Layer isolation**: two containers sharing one base image had fully isolated writes and zero extra disk usage from the shared base.
- **Networking**: two concurrently running containers reached each other directly in about 0.06ms, and both reached the internet at the same time.
- **Real images**: `alpine:latest` and `python:3.11-slim` (a different base OS, with multiple layers) both pulled live from Docker Hub and run start to finish.
- **Repeatability**: the automated test harness shows a 100% pass rate over 26 runs, with zero leaked mounts, cgroups, or network interfaces.

## Testing

```
sudo bash scripts/test-harness.sh
```

Runs image compatibility checks across several images, compares cold versus cached pull latency, computes p50/p95 startup time over repeated runs, and checks for leaked mounts, cgroups, and network interfaces after everything finishes.

## Project structure

```
src/main.cpp        entrypoint, CLI parsing
src/namespaces/      namespace isolation
src/cgroups/         cgroup v2 CPU, memory, and pids limits
src/fs/              pivot_root, OverlayFS
src/network/         veth, bridge, NAT
src/registry/        Docker Hub registry client
scripts/             bootstrap and test harness scripts
```

## Design decisions and tradeoffs

A few of the bigger ones, explained fully with alternatives and costs in [DESIGN.md](DESIGN.md):

- `clone()` with namespace flags instead of `fork()` plus `unshare()`, to avoid a window where the child runs before it is isolated.
- `pivot_root` instead of `chroot`, since `chroot` alone does not close off a real escape route back to the host filesystem.
- Networking is implemented by shelling out to `ip`, `iptables`, and `nsenter` instead of speaking rtnetlink directly, favoring readability over the robustness a structured API would give.
- Container addresses are derived from process IDs rather than tracked by a real IP address manager, since the goal was proving connectivity works, not building production grade allocation.

## Limitations and future improvements

The most important ones; the full list and reasoning are in [DESIGN.md](DESIGN.md):

- No user namespaces, capability dropping, or seccomp filtering. The container runs as real root with full capabilities. Namespace isolation is real; privilege isolation is not.
- No image config parsing (`CMD`, `ENTRYPOINT`, `ENV`). Every run needs an explicit command.
- No layer digest verification, and no private registry support.
- Cleanup relies on normal process exit. A `SIGKILL` sent to the runtime itself can leave a stray cgroup or mount behind.
- One container per invocation. There is no daemon and no orchestration.

## Deployment

This is a command line tool, not a hosted service, so there is nothing to deploy in the usual sense. It was built and verified on a single AWS EC2 Ubuntu instance, provisioned specifically for development and terminated once the code and all verification evidence were pushed to GitHub. Running it elsewhere just means satisfying the same requirements (a real Linux host with root access) listed above and building from source.

## Contributing and license

This is a personal portfolio project and is not currently seeking contributions. Licensed under the MIT License; see [LICENSE](LICENSE).
