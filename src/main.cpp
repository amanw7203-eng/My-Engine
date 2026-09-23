#include "Renderer/Mesh.h"
#include "Renderer/Shader.h"

#include <glad/gl.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

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
            (void)deltaTime; // used by update logic later

            // --- Render ---
            int width = 0, height = 0;
            SDL_GetWindowSizeInPixels(window, &width, &height);
            glViewport(0, 0, width, height);
            glClearColor(0.10f, 0.12f, 0.18f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            shader.Bind();
            quad.Draw();

            SDL_GL_SwapWindow(window);
        }
    } // shader and quad destroyed here, before the context

    SDL_GL_DestroyContext(glContext);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return exitCode;
}
