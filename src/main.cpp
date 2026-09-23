#include "Renderer/Camera.h"
#include "Renderer/Mesh.h"
#include "Renderer/Shader.h"
#include "Renderer/Texture.h"
#include "UI/ImGuiLayer.h"

#include <glad/gl.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

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
        ImGuiLayer ui(window, glContext);

        Shader shader(assetDir / "shaders/basic.vert", assetDir / "shaders/basic.frag");
        if (!shader.IsValid())
            exitCode = 1;

        Mesh quad = Mesh::CreateQuad();
        // 50x50 floor with the texture tiled 25 times each way (one tile per
        // 2 units), stretching into the distance to show off mipmapping.
        Mesh floor = Mesh::CreatePlane(50.0f, 25.0f);

        Texture checker(assetDir / "textures/checker.png");

        Camera camera(glm::vec3(0.0f, 0.0f, 3.0f)); // 3 units back, facing the quad

        // Settings the UI can edit.
        glm::vec3 clearColor(0.10f, 0.12f, 0.18f);
        bool spinQuad = true;
        float spinSpeed = 90.0f; // degrees per second
        bool showImGuiDemo = false;

        float rotation = 0.0f; // radians

        // Mouse look is active while right mouse is held OR while locked on
        // with Left Alt.
        bool rightMouseHeld = false;
        bool mouseLookLocked = false;

        Uint64 lastTicks = SDL_GetTicksNS();
        bool running = shader.IsValid();

        while (running)
        {
            // --- Events ---
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                // While flying the camera the cursor is hidden, so the UI
                // shouldn't see mouse input (it would hover/click blindly).
                const bool isMouseEvent = event.type == SDL_EVENT_MOUSE_MOTION
                                       || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                                       || event.type == SDL_EVENT_MOUSE_BUTTON_UP
                                       || event.type == SDL_EVENT_MOUSE_WHEEL;
                if (!(isMouseEvent && SDL_GetWindowRelativeMouseMode(window)))
                    ui.ProcessEvent(event);

                if (event.type == SDL_EVENT_QUIT)
                    running = false;
                else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE && !ui.WantsKeyboard())
                    running = false;
                // Left Alt toggles mouse look on/off (ignoring key repeat, so
                // holding it down doesn't flicker).
                else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_LALT && !event.key.repeat
                         && !ui.WantsKeyboard())
                    mouseLookLocked = !mouseLookLocked;
                // Right-clicking a UI panel is for the UI, not for mouse look.
                else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_RIGHT
                         && !ui.WantsMouse())
                    rightMouseHeld = true;
                else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_RIGHT)
                    rightMouseHeld = false;
                else if (event.type == SDL_EVENT_MOUSE_MOTION && SDL_GetWindowRelativeMouseMode(window))
                    camera.Rotate(event.motion.xrel, event.motion.yrel);
            }

            // While looking around, hide the cursor and report raw mouse
            // movement instead of a screen position.
            const bool mouseLook = rightMouseHeld || mouseLookLocked;
            if (mouseLook != SDL_GetWindowRelativeMouseMode(window))
                SDL_SetWindowRelativeMouseMode(window, mouseLook);

            // --- Timing ---
            const Uint64 nowTicks = SDL_GetTicksNS();
            const float deltaTime = static_cast<float>(nowTicks - lastTicks) / 1e9f;
            lastTicks = nowTicks;

            // --- UI ---
            ui.BeginFrame();

            ImGui::Begin("Engine");
            const ImGuiIO& io = ImGui::GetIO();
            ImGui::Text("%.1f FPS (%.2f ms)", io.Framerate, 1000.0f / io.Framerate);

            ImGui::SeparatorText("Camera");
            const glm::vec3& camPos = camera.GetPosition();
            ImGui::Text("Position: %.2f, %.2f, %.2f", camPos.x, camPos.y, camPos.z);
            ImGui::SliderFloat("Move speed", &camera.moveSpeed, 0.5f, 20.0f);
            ImGui::SliderFloat("Sensitivity", &camera.mouseSensitivity, 0.01f, 0.5f);
            ImGui::SliderFloat("Field of view", &camera.fieldOfView, 30.0f, 110.0f);
            ImGui::Checkbox("Mouse look locked", &mouseLookLocked);

            ImGui::SeparatorText("Scene");
            ImGui::ColorEdit3("Background", &clearColor.x);
            ImGui::Checkbox("Spin quad", &spinQuad);
            ImGui::SliderFloat("Spin speed", &spinSpeed, -360.0f, 360.0f, "%.0f deg/s");

            ImGui::SeparatorText("Controls");
            ImGui::TextDisabled("WASD move, Q/E down/up, Shift sprint");
            ImGui::TextDisabled("Hold right mouse or Left Alt to look");
            ImGui::TextDisabled("Esc quit");
            ImGui::Checkbox("Show ImGui demo", &showImGuiDemo);
            ImGui::End();

            if (showImGuiDemo)
                ImGui::ShowDemoWindow(&showImGuiDemo);

            // --- Update ---
            // Read held keys (not key events) so movement is smooth while a
            // key stays down, rather than waiting for keyboard repeat.
            // Skipped while typing into a UI text field.
            if (!ui.WantsKeyboard())
            {
                const bool* keys = SDL_GetKeyboardState(nullptr);
                glm::vec3 moveDirection(0.0f);
                if (keys[SDL_SCANCODE_W]) moveDirection.z += 1.0f;
                if (keys[SDL_SCANCODE_S]) moveDirection.z -= 1.0f;
                if (keys[SDL_SCANCODE_D]) moveDirection.x += 1.0f;
                if (keys[SDL_SCANCODE_A]) moveDirection.x -= 1.0f;
                if (keys[SDL_SCANCODE_E]) moveDirection.y += 1.0f;
                if (keys[SDL_SCANCODE_Q]) moveDirection.y -= 1.0f;
                camera.sprinting = keys[SDL_SCANCODE_LSHIFT];
                camera.Move(moveDirection, deltaTime);
            }

            // Same speed at any frame rate.
            if (spinQuad)
                rotation += glm::radians(spinSpeed) * deltaTime;

            // --- Render ---
            int width = 0, height = 0;
            SDL_GetWindowSizeInPixels(window, &width, &height);
            glViewport(0, 0, width, height);
            glClearColor(clearColor.r, clearColor.g, clearColor.b, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            // Recomputed each frame so the image never stretches on resize.
            // (height is 0 while the window is minimised.)
            const float aspect = height > 0 ? static_cast<float>(width) / height : 1.0f;

            shader.Bind();
            shader.SetMat4("uView", camera.GetViewMatrix());
            shader.SetMat4("uProjection", camera.GetProjectionMatrix(aspect));

            // The sampler reads texture unit 0, which the checker is bound to.
            shader.SetInt("uTexture", 0);
            checker.Bind(0);

            // Floor just below the quad's bottom edge.
            shader.SetMat4("uModel", glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -0.5f, 0.0f)));
            floor.Draw();

            shader.SetMat4("uModel",
                glm::rotate(glm::mat4(1.0f), rotation, glm::vec3(0.0f, 1.0f, 0.0f)));
            quad.Draw();

            // UI last, so it draws on top of the scene.
            ui.EndFrame();

            SDL_GL_SwapWindow(window);
        }
    } // UI, shader, meshes and texture destroyed here, before the context

    SDL_GL_DestroyContext(glContext);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return exitCode;
}
