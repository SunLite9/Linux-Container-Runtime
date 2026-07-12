#include "network.hpp"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace cr::network {

namespace {

// Shells out to `ip`/`iptables`/`nsenter` rather than talking to
// rtnetlink directly: this is a portfolio project, and these are the
// exact commands a human operator would run by hand to debug the same
// setup, which keeps the code close to self-documenting. The tradeoff is
// fragility to exit-code/quoting edge cases that raw netlink wouldn't
// have — real production runtimes (runc, etc.) use netlink for exactly
// that reason.
void runCmd(const std::string& cmd) {
    const int rc = std::system(cmd.c_str());
    if (rc != 0) {
        throw std::runtime_error("command failed (exit " + std::to_string(rc) + "): " + cmd);
    }
}

// For idempotent/best-effort steps where failure (e.g. "already exists",
// "already deleted") is expected and harmless.
void runCmdIgnore(const std::string& cmd) {
    (void)!std::system(cmd.c_str());
}

bool commandSucceeds(const std::string& cmd) {
    return std::system((cmd + " > /dev/null 2>&1").c_str()) == 0;
}

} // namespace

void ensureBridge() {
    if (!commandSucceeds(std::string("ip link show ") + kBridgeName)) {
        runCmd(std::string("ip link add ") + kBridgeName + " type bridge");
        runCmd(std::string("ip addr add ") + kBridgeIp + "/24 dev " + kBridgeName);
    }
    runCmd(std::string("ip link set ") + kBridgeName + " up");

    // Idempotent: writing "1" when it's already "1" is a harmless no-op.
    runCmd("sysctl -q -w net.ipv4.ip_forward=1");

    // `iptables -C` checks whether a rule already exists; only `-A`ppend
    // it if not, so re-running this before every container doesn't pile
    // up duplicate NAT/FORWARD rules.
    const std::string masqueradeRule =
        "POSTROUTING -s " + std::string(kSubnetCidr) + " ! -o " + kBridgeName + " -j MASQUERADE";
    if (!commandSucceeds("iptables -t nat -C " + masqueradeRule)) {
        runCmd("iptables -t nat -A " + masqueradeRule);
    }

    if (!commandSucceeds(std::string("iptables -C FORWARD -i ") + kBridgeName + " -j ACCEPT")) {
        runCmd(std::string("iptables -A FORWARD -i ") + kBridgeName + " -j ACCEPT");
    }
    if (!commandSucceeds(std::string("iptables -C FORWARD -o ") + kBridgeName + " -j ACCEPT")) {
        runCmd(std::string("iptables -A FORWARD -o ") + kBridgeName + " -j ACCEPT");
    }
}

Veth::Veth(pid_t containerPid, int ipHostOctet) {
    // Interface names are capped at 15 characters (IFNAMSIZ); truncating
    // the PID keeps "veth<suffix>h"/"veth<suffix>c" safely under that.
    const std::string suffix = std::to_string(containerPid % 100000);
    hostIfName_ = "veth" + suffix + "h";
    const std::string peerName = "veth" + suffix + "c";
    const std::string containerIp = "172.20.0." + std::to_string(ipHostOctet);
    const std::string pidStr = std::to_string(containerPid);

    runCmd("ip link add " + hostIfName_ + " type veth peer name " + peerName);
    runCmd("ip link set " + hostIfName_ + " master " + kBridgeName);
    runCmd("ip link set " + hostIfName_ + " up");
    // Moves the peer out of this (host) namespace and into the
    // container's, addressed by containerPid as seen from here.
    runCmd("ip link set " + peerName + " netns " + pidStr);

    // From here on, configure the interface *inside* the container's
    // network namespace by joining it with nsenter rather than moving
    // more state around — containerPid must still be alive (blocked on
    // the readiness pipe) for /proc/<pid>/ns/net to be valid.
    const std::string ns = "nsenter -t " + pidStr + " -n --";
    runCmd(ns + " ip link set " + peerName + " name eth0");
    runCmd(ns + " ip addr add " + containerIp + "/24 dev eth0");
    runCmd(ns + " ip link set eth0 up");
    runCmd(ns + " ip link set lo up");
    runCmd(ns + " ip route add default via " + kBridgeIp);
}

Veth::~Veth() {
    // Deleting either end of a veth pair deletes both; the container-side
    // peer's netns is also being torn down around the same time as the
    // container's last process exits.
    runCmdIgnore("ip link del " + hostIfName_ + " > /dev/null 2>&1");
}

} // namespace cr::network
