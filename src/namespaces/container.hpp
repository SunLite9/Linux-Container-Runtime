#pragma once

#include <string>
#include <vector>
#include <sys/types.h>

#include "../cgroups/cgroups.hpp"

namespace cr {

class Container {
public:
    Container(std::string imageRef, std::string command, std::vector<std::string> args,
              cgroups::Limits limits = {});
    ~Container();

    Container(const Container&) = delete;
    Container& operator=(const Container&) = delete;

    void run();

private:
    static int childEntry(void* arg);
    int childMain();

    std::string imageRef_;
    std::string command_;
    std::vector<std::string> args_;
    cgroups::Limits limits_;
    pid_t childPid_ = -1;
    std::string pivotTarget_;
    int readyPipe_[2] = {-1, -1};
};

} // namespace cr
