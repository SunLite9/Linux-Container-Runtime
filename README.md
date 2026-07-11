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
