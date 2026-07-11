#include "namespaces/container.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {
// Phase 2 hardcodes the rootfs location; Task 6 replaces this with a
// registry-pulled image path selected by the requested image name.
constexpr const char* kRootfsPath = "rootfs/alpine";

void printUsage(const char* prog) {
    std::cerr << "Usage: " << prog << " run [command] [args...]\n";
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

    const std::string command = argc >= 3 ? argv[2] : "/bin/sh";
    std::vector<std::string> args;
    for (int i = 3; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    cr::Container container(kRootfsPath, command, args);
    container.run();
    return 0;
}
