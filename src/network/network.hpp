#pragma once

#include <string>
#include <sys/types.h>

namespace cr::network {

constexpr const char* kBridgeName = "cr0";
constexpr const char* kBridgeIp = "172.20.0.1";
constexpr const char* kSubnetCidr = "172.20.0.0/24";

// Idempotent: creates the shared "cr0" Linux bridge (if missing), gives it
// 172.20.0.1/24, brings it up, enables IPv4 forwarding, and adds the
// iptables NAT/FORWARD rules containers need to reach the internet. Safe
// to call before every container run — a no-op if already set up.
void ensureBridge();

// One veth pair connecting a container's network namespace to the host
// bridge. `containerPid` must already be inside the target network
// namespace (referenced via /proc/<containerPid>/ns/net) — in practice,
// blocked on a synchronization read() until this constructor finishes,
// so nothing execs inside the container before its network exists.
//
// RAII: the constructor creates the veth pair, attaches the host end to
// the bridge, moves the peer into the container's netns, and configures
// it there (renamed to eth0, given an IP, brought up, default route
// added). The destructor deletes the host-side veth interface; the
// kernel deletes its peer along with it (veth interfaces are always
// deleted in pairs), and the container's network namespace itself is
// torn down automatically once its last process exits.
class Veth {
public:
    Veth(pid_t containerPid, int ipHostOctet);
    ~Veth();

    Veth(const Veth&) = delete;
    Veth& operator=(const Veth&) = delete;

private:
    std::string hostIfName_;
};

} // namespace cr::network
