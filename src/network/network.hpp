#pragma once

#include <string>
#include <sys/types.h>

namespace cr::network {

constexpr const char* kBridgeName = "cr0";
constexpr const char* kBridgeIp = "172.20.0.1";
constexpr const char* kSubnetCidr = "172.20.0.0/24";

// Idempotent: creates the shared "cr0" bridge, enables IPv4 forwarding,
// and adds the iptables NAT/FORWARD rules containers need for internet
// access. Safe to call before every container run.
void ensureBridge();

// One veth pair connecting a container's network namespace to the host
// bridge. containerPid must already be inside the target network
// namespace (/proc/<containerPid>/ns/net).
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
