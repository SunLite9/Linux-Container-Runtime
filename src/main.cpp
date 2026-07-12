#include "namespaces/container.hpp"

#include <curl/curl.h>

#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace {
void printUsage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " run [--cpu-limit N] [--memory-limit MB] [--pids-limit N] <image> [command] [args...]\n";
    std::cerr << "  --cpu-limit N       fraction of one CPU core (default 0.5)\n";
    std::cerr << "  --memory-limit MB   memory cap in megabytes (default 100)\n";
    std::cerr << "  --pids-limit N      max processes/threads in the container (default 128)\n";
    std::cerr << "  <image>             e.g. alpine:latest, python:3.11-slim\n";
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

    cr::cgroups::Limits limits;
    int i = 2;
    for (; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--cpu-limit" && i + 1 < argc) {
            limits.cpuCores = std::stod(argv[++i]);
        } else if (arg == "--memory-limit" && i + 1 < argc) {
            limits.memoryMb = std::stol(argv[++i]);
        } else if (arg == "--pids-limit" && i + 1 < argc) {
            limits.pidsMax = std::stol(argv[++i]);
        } else {
            break;
        }
    }

    if (i >= argc) {
        std::cerr << "error: missing <image> argument\n\n";
        printUsage(argv[0]);
        return 1;
    }
    const std::string image = argv[i++];

    const std::string command = i < argc ? argv[i++] : "/bin/sh";
    std::vector<std::string> args;
    for (; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    int exitCode = 0;
    try {
        cr::Container container(image, command, args, limits);
        container.run();
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        exitCode = 1;
    }

    curl_global_cleanup();
    return exitCode;
}
