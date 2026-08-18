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

# --- Server executable: adds the networking layer. -----------------------
# The networking uses Winsock, so the runnable server is built on Windows.
# (Making the socket layer portable is a later step; the core + tests already
# build and run everywhere.)
if(WIN32)
    add_executable(miniredis src/main.cpp src/server.cpp)
    target_link_libraries(miniredis PRIVATE miniredis_core ws2_32)
endif()

# --- Tests ---------------------------------------------------------------
enable_testing()
add_executable(miniredis_tests
    tests/framework.cpp
    tests/test_store.cpp
    tests/test_resp.cpp
    tests/test_commands.cpp
    tests/test_persistence.cpp
    tests/test_main.cpp
)
target_link_libraries(miniredis_tests PRIVATE miniredis_core)
add_test(NAME unit COMMAND miniredis_tests)
