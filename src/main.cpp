#include "namespaces/container.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {
// Phase 2 hardcodes the rootfs location; Task 6 replaces this with a
// registry-pulled image path selected by the requested image name.
constexpr const char* kRootfsPath = "rootfs/alpine";

void printUsage(const char* prog) {
    std::cerr << "Usage: " << prog << " run [--cpu-limit N] [--memory-limit MB] [command] [args...]\n";
    std::cerr << "  --cpu-limit N       fraction of one CPU core (default 0.5)\n";
    std::cerr << "  --memory-limit MB   memory cap in megabytes (default 100)\n";
    std::cerr << "  (default command is /bin/sh; must be run with sudo from\n";
    std::cerr << "   the project root, so " << kRootfsPath << " resolves correctly)\n";
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    const std::string subcommand = argv[1];
    if (subcommand != "run") {
        printUsage(argv[0]);
        return 1;
    }

    cr::cgroups::Limits limits;
    int i = 2;
    for (; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--cpu-limit" && i + 1 < argc) {
            limits.cpuCores = std::stod(argv[++i]);
        } else if (arg == "--memory-limit" && i + 1 < argc) {
            limits.memoryMb = std::stol(argv[++i]);
        } else {
            break;
        }
    }

    const std::string command = i < argc ? argv[i++] : "/bin/sh";
    std::vector<std::string> args;
    for (; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    cr::Container container(kRootfsPath, command, args, limits);
    container.run();
    return 0;
}
