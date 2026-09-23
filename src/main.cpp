#include "Renderer/Mesh.h"
#include "Renderer/Shader.h"

#include <glad/gl.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <filesystem>
#include <iostream>

int main(int /*argc*/, char* /*argv*/[])
{
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << '\n';
        return 1;
    }

    // Request an OpenGL 3.3 core context.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24); // depth buffer for 3D

    SDL_Window* window = SDL_CreateWindow("MyEngine", 1280, 720,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!window)
    {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << '\n';
        SDL_Quit();
        return 1;
    }

    SDL_GLContext glContext = SDL_GL_CreateContext(window);
    if (!glContext)
    {
        std::cerr << "SDL_GL_CreateContext failed: " << SDL_GetError() << '\n';
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Load all OpenGL 3.3 functions from the driver. Must happen after the
    // context exists and before any other gl* call.
    if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(SDL_GL_GetProcAddress)))
    {
        std::cerr << "Failed to load OpenGL functions\n";
        SDL_GL_DestroyContext(glContext);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_GL_SetSwapInterval(1); // vsync

    // Only draw a pixel if it is closer to the camera than what is already there.
    glEnable(GL_DEPTH_TEST);

    std::cout << "OpenGL " << glGetString(GL_VERSION) << '\n';

    // Assets are copied next to the executable by the build, so look them up
    // relative to the exe rather than the current working directory.
    const char* basePath = SDL_GetBasePath();
    const std::filesystem::path assetDir =
        std::filesystem::path(basePath ? basePath : "") / "assets";

    int exitCode = 0;

    // Everything that owns GL objects lives in this scope, so it is destroyed
    // while the GL context still exists.
    {
        Shader shader(assetDir / "shaders/basic.vert", assetDir / "shaders/basic.frag");
        if (!shader.IsValid())
            exitCode = 1;

        Mesh quad = Mesh::CreateQuad();

        // Camera 3 units back from the origin, looking at it, with +Y as up.
        const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.0f, 3.0f),
                                           glm::vec3(0.0f, 0.0f, 0.0f),
                                           glm::vec3(0.0f, 1.0f, 0.0f));

        float rotation = 0.0f; // radians

        Uint64 lastTicks = SDL_GetTicksNS();
        bool running = shader.IsValid();

        while (running)
        {
            // --- Events ---
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                if (event.type == SDL_EVENT_QUIT)
                    running = false;
                else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)
                    running = false;
            }

            // --- Timing ---
            const Uint64 nowTicks = SDL_GetTicksNS();
            const float deltaTime = static_cast<float>(nowTicks - lastTicks) / 1e9f;
            lastTicks = nowTicks;

            // --- Update ---
            // 90 degrees per second, the same speed at any frame rate.
            rotation += glm::radians(90.0f) * deltaTime;

            // --- Render ---
            int width = 0, height = 0;
            SDL_GetWindowSizeInPixels(window, &width, &height);
            glViewport(0, 0, width, height);
            glClearColor(0.10f, 0.12f, 0.18f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            // Recomputed each frame so the image never stretches on resize.
            // (height is 0 while the window is minimised.)
            const float aspect = height > 0 ? static_cast<float>(width) / height : 1.0f;
            const glm::mat4 projection =
                glm::perspective(glm::radians(45.0f), aspect, 0.1f, 100.0f);

            const glm::mat4 model =
                glm::rotate(glm::mat4(1.0f), rotation, glm::vec3(0.0f, 1.0f, 0.0f));

            shader.Bind();
            shader.SetMat4("uModel", model);
            shader.SetMat4("uView", view);
            shader.SetMat4("uProjection", projection);
            quad.Draw();

            SDL_GL_SwapWindow(window);
        }
    } // shader and quad destroyed here, before the context

    SDL_GL_DestroyContext(glContext);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return exitCode;
}
