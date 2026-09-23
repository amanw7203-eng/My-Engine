cmake_minimum_required(VERSION 3.25)
project(MyEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(MyEngine
    src/main.cpp
)

if(MSVC)
    target_compile_options(MyEngine PRIVATE /W4 /permissive-)
else()
    target_compile_options(MyEngine PRIVATE -Wall -Wextra -Wpedantic)
endif()
