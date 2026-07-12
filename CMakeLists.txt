cmake_minimum_required(VERSION 3.16)
project(container-runtime CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(container-runtime
    src/main.cpp
    src/namespaces/container.cpp
    src/fs/rootfs.cpp
    src/fs/overlay.cpp
    src/cgroups/cgroups.cpp
    src/network/network.cpp
)

target_include_directories(container-runtime PRIVATE include src)
