cmake_minimum_required(VERSION 3.25)
project(MyEngine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Dependencies -----------------------------------------------------------
include(FetchContent)

# SDL3: window, input, timing. Built as a static lib so no DLL needs copying.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.4.16
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(SDL3)

find_package(OpenGL REQUIRED)

# --- Engine -----------------------------------------------------------------
add_executable(MyEngine
    src/main.cpp
)

target_link_libraries(MyEngine PRIVATE SDL3::SDL3 OpenGL::GL)

if(MSVC)
    target_compile_options(MyEngine PRIVATE /W4 /permissive-)
else()
    target_compile_options(MyEngine PRIVATE -Wall -Wextra -Wpedantic)
endif()
