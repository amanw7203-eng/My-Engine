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

# Dear ImGui (docking branch): immediate-mode UI drawn with OpenGL inside the
# SDL window. It ships no CMake file, so build its sources as a library here.
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.9b-docking
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3 OpenGL::GL)

# glad: loads modern OpenGL functions at runtime. Generated for GL 4.6 core
# (glad2: glad --api gl:core=4.6 --extensions "" --out-path external/glad c)
add_library(glad STATIC external/glad/src/gl.c)
target_include_directories(glad PUBLIC external/glad/include)

# stb_image: loads PNG/JPG/TGA/BMP/... into raw pixels.
# (https://github.com/nothings/stb at commit 2c980bb, v2.30)
add_library(stb_image STATIC external/stb/stb_image.c)
target_include_directories(stb_image PUBLIC external/stb)

# CUDA: the runtime API that OptiX runs on top of. Only host code includes
# it, so the CUDA language isn't enabled; the .cu device programs are
# compiled separately with nvcc.
find_package(CUDAToolkit REQUIRED)

# OptiX: hardware ray tracing on the RT cores. Header-only, as the
# implementation ships inside the NVIDIA driver.
FetchContent_Declare(
    optix
    GIT_REPOSITORY https://github.com/NVIDIA/optix-dev.git
    GIT_TAG        v9.1.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(optix)

add_library(optix INTERFACE)
target_include_directories(optix INTERFACE ${optix_SOURCE_DIR}/include)
target_link_libraries(optix INTERFACE CUDA::cudart_static)

# --- Engine -----------------------------------------------------------------
add_executable(MyEngine
    src/main.cpp
    src/Assets/AssetLibrary.cpp
    src/RayTracing/CudaBuffer.cpp
    src/RayTracing/Denoiser.cpp
    src/RayTracing/InteropTexture.cpp
    src/RayTracing/MeshAccel.cpp
    src/RayTracing/OptixContext.cpp
    src/RayTracing/RayTracer.cpp
    src/RayTracing/SceneAccel.cpp
    src/Renderer/Camera.cpp
    src/Renderer/Mesh.cpp
    src/Renderer/ObjectPicker.cpp
    src/Renderer/Shader.cpp
    src/Renderer/ShadowMap.cpp
    src/Renderer/Texture.cpp
    src/Scene/Entity.cpp
    src/Scene/Scene.cpp
    src/UI/AssetsPanel.cpp
    src/UI/AssetWidgets.cpp
    src/UI/ImGuiLayer.cpp
    src/UI/SceneHierarchyPanel.cpp
    src/UI/SideDrawer.cpp
)

target_include_directories(MyEngine PRIVATE src)
target_link_libraries(MyEngine PRIVATE SDL3::SDL3 OpenGL::GL glad glm::glm stb_image imgui optix)

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

# OptiX programs: the code that runs on the GPU for each ray. nvcc compiles
# them to OptiX-IR, which the engine loads at runtime from raytracing/ next
# to the executable. compute_75 (Turing, the first RTX generation) is the
# oldest target CUDA 13 supports; the driver compiles it for the actual GPU.
set(OPTIX_PROGRAMS_SOURCE ${CMAKE_SOURCE_DIR}/src/RayTracing/Programs.cu)
set(OPTIX_PROGRAMS_OUTPUT ${CMAKE_BINARY_DIR}/optix/$<CONFIG>/programs.optixir)
get_filename_component(OPTIX_HOST_COMPILER_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)
add_custom_command(
    OUTPUT  ${OPTIX_PROGRAMS_OUTPUT}
    COMMAND ${CUDAToolkit_NVCC_EXECUTABLE}
            -optix-ir -arch=compute_75 -std=c++17 -use_fast_math -lineinfo
            -ccbin ${OPTIX_HOST_COMPILER_DIR}
            -I${optix_SOURCE_DIR}/include -I${CMAKE_SOURCE_DIR}/src
            ${OPTIX_PROGRAMS_SOURCE} -o ${OPTIX_PROGRAMS_OUTPUT}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${OPTIX_PROGRAMS_OUTPUT} $<TARGET_FILE_DIR:MyEngine>/raytracing/programs.optixir
    DEPENDS ${OPTIX_PROGRAMS_SOURCE} ${CMAKE_SOURCE_DIR}/src/RayTracing/DeviceTypes.h
    COMMENT "Compiling OptiX programs"
    VERBATIM
)
add_custom_target(optix_programs ALL DEPENDS ${OPTIX_PROGRAMS_OUTPUT} SOURCES ${OPTIX_PROGRAMS_SOURCE})
add_dependencies(MyEngine optix_programs)
