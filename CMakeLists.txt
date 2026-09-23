cmake_minimum_required(VERSION 3.26)
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

# GLM: header-only math library (vectors, matrices, transforms).
set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(glm)

find_package(OpenGL REQUIRED)

# glad: loads modern OpenGL functions at runtime. Generated for GL 3.3 core
# (glad2: glad --api gl:core=3.3 --extensions "" --out-path external/glad c)
add_library(glad STATIC external/glad/src/gl.c)
target_include_directories(glad PUBLIC external/glad/include)

# --- Engine -----------------------------------------------------------------
add_executable(MyEngine
    src/main.cpp
    src/Renderer/Mesh.cpp
    src/Renderer/Shader.cpp
)

target_include_directories(MyEngine PRIVATE src)
target_link_libraries(MyEngine PRIVATE SDL3::SDL3 OpenGL::GL glad glm::glm)

if(MSVC)
    target_compile_options(MyEngine PRIVATE /W4 /permissive-)
else()
    target_compile_options(MyEngine PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Copy assets/ next to the executable on every build (only changed files),
# so the engine finds them no matter which folder it is started from.
add_custom_target(copy_assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            ${CMAKE_SOURCE_DIR}/assets
            $<TARGET_FILE_DIR:MyEngine>/assets
    COMMENT "Copying assets"
)
add_dependencies(MyEngine copy_assets)
