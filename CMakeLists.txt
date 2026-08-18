cmake_minimum_required(VERSION 3.15)
project(miniredis LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Warning flags shared by every target we build.
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra)
endif()

# --- Core library: pure data/protocol/command logic, no sockets. ---------
# Sharing it between the server and the tests means the tests exercise the
# exact same code the server runs, and they build on every platform because
# nothing here touches the OS networking API.
add_library(miniredis_core STATIC
    src/store.cpp
    src/resp.cpp
    src/commands.cpp
)
target_include_directories(miniredis_core PUBLIC include)

# Threads: std::thread needs -pthread on POSIX; a no-op on Windows/MSVC.
find_package(Threads REQUIRED)

# --- Server executable: adds the networking layer. -----------------------
# The socket code (net.hpp) is portable across Winsock and POSIX, so the server
# builds and runs on Windows, Linux, and macOS.
add_executable(miniredis src/main.cpp src/server.cpp)
target_link_libraries(miniredis PRIVATE miniredis_core Threads::Threads)
if(WIN32)
    target_link_libraries(miniredis PRIVATE ws2_32)  # Winsock
endif()

# --- Benchmark client ----------------------------------------------------
# A standalone load generator; talks to a running server over TCP.
add_executable(miniredis_bench bench/bench.cpp)
target_include_directories(miniredis_bench PRIVATE include)
target_link_libraries(miniredis_bench PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(miniredis_bench PRIVATE ws2_32)
endif()

# --- Tests ---------------------------------------------------------------
enable_testing()
add_executable(miniredis_tests
    tests/framework.cpp
    tests/test_store.cpp
    tests/test_resp.cpp
    tests/test_commands.cpp
    tests/test_types.cpp
    tests/test_persistence.cpp
    tests/test_main.cpp
)
target_link_libraries(miniredis_tests PRIVATE miniredis_core Threads::Threads)
add_test(NAME unit COMMAND miniredis_tests)
