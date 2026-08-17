cmake_minimum_required(VERSION 3.15)
project(miniredis LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(miniredis
    src/main.cpp
    src/store.cpp
    src/resp.cpp
    src/commands.cpp
    src/server.cpp
)

target_include_directories(miniredis PRIVATE include)

if(MSVC)
    target_compile_options(miniredis PRIVATE /W4)
else()
    target_compile_options(miniredis PRIVATE -Wall -Wextra)
endif()

# Windows sockets live in the ws2_32 library.
if(WIN32)
    target_link_libraries(miniredis PRIVATE ws2_32)
endif()
