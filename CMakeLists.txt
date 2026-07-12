cmake_minimum_required(VERSION 3.16)
project(container-runtime CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)

include(FetchContent)
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(json)

add_executable(container-runtime
    src/main.cpp
    src/namespaces/container.cpp
    src/fs/rootfs.cpp
    src/fs/overlay.cpp
    src/cgroups/cgroups.cpp
    src/network/network.cpp
    src/registry/registry.cpp
)

target_include_directories(container-runtime PRIVATE include src)
target_link_libraries(container-runtime PRIVATE CURL::libcurl nlohmann_json::nlohmann_json)
