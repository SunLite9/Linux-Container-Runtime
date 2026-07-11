#include "namespaces/container.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {
void printUsage(const char* prog) {
    std::cerr << "Usage: " << prog << " run [command] [args...]\n";
    std::cerr << "  (default command is /bin/sh; must be run with sudo)\n";
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

    cr::Container container(command, args);
    container.run();
    return 0;
}
