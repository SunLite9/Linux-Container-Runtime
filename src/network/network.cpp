#include "network.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace cr::network {

namespace {

// Shells out to ip/iptables/nsenter rather than talking to rtnetlink
// directly; simpler for a portfolio project, at the cost of exit-code
// fragility that raw netlink wouldn't have.
void runCmd(const std::string& cmd) {
    const int rc = std::system(cmd.c_str());
    if (rc != 0) {
        throw std::runtime_error("command failed (exit " + std::to_string(rc) + "): " + cmd);
    }
}

void runCmdIgnore(const std::string& cmd) {
    (void)!std::system(cmd.c_str());
}

bool commandSucceeds(const std::string& cmd) {
    return std::system((cmd + " > /dev/null 2>&1").c_str()) == 0;
}

// Serializes ensureBridge() across concurrently-starting containers.
// Without this, two invocations can both see the bridge missing and
// both run `ip link add`, so the loser fails with "File exists".
class FileLock {
public:
    explicit FileLock(const std::string& path) {
        fd_ = open(path.c_str(), O_CREAT | O_RDWR, 0644);
        if (fd_ < 0) {
            throw std::runtime_error("open lock file failed: " + path);
        }
        flock(fd_, LOCK_EX);
    }
    ~FileLock() {
        flock(fd_, LOCK_UN);
        close(fd_);
    }
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

private:
    int fd_ = -1;
};

} // namespace

void ensureBridge() {
    FileLock lock("/run/container-runtime-bridge.lock");

    if (!commandSucceeds(std::string("ip link show ") + kBridgeName)) {
        runCmd(std::string("ip link add ") + kBridgeName + " type bridge");
        runCmd(std::string("ip addr add ") + kBridgeIp + "/24 dev " + kBridgeName);
    }
    runCmd(std::string("ip link set ") + kBridgeName + " up");
    runCmd("sysctl -q -w net.ipv4.ip_forward=1");

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
    // IFNAMSIZ caps interface names at 15 chars.
    const std::string suffix = std::to_string(containerPid % 100000);
    hostIfName_ = "veth" + suffix + "h";
    const std::string peerName = "veth" + suffix + "c";
    const std::string containerIp = "172.20.0." + std::to_string(ipHostOctet);
    const std::string pidStr = std::to_string(containerPid);

    runCmd("ip link add " + hostIfName_ + " type veth peer name " + peerName);
    runCmd("ip link set " + hostIfName_ + " master " + kBridgeName);
    runCmd("ip link set " + hostIfName_ + " up");
    runCmd("ip link set " + peerName + " netns " + pidStr);

    const std::string ns = "nsenter -t " + pidStr + " -n --";
    runCmd(ns + " ip link set " + peerName + " name eth0");
    runCmd(ns + " ip addr add " + containerIp + "/24 dev eth0");
    runCmd(ns + " ip link set eth0 up");
    runCmd(ns + " ip link set lo up");
    runCmd(ns + " ip route add default via " + kBridgeIp);
}

Veth::~Veth() {
    runCmdIgnore("ip link del " + hostIfName_ + " > /dev/null 2>&1");
}

} // namespace cr::network
