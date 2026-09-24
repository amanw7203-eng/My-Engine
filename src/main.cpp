#include "Assets/AssetLibrary.h"
#include "Editor/UndoHistory.h"
#include "Renderer/Camera.h"
#include "Renderer/Mesh.h"
#include "Renderer/ObjectPicker.h"
#include "Renderer/Shader.h"
#include "Renderer/ShadowMap.h"
#include "Renderer/Texture.h"
#include "Scene/Scene.h"
#include "Renderer/ColorSpace.h"
#include "RayTracing/MeshAccel.h"
#include "RayTracing/OptixContext.h"
#include "RayTracing/RayTracer.h"
#include "RayTracing/SceneAccel.h"
#include "RayTracing/TextureCache.h"
#include "UI/AssetsPanel.h"
#include "UI/ContentBrowserPanel.h"
#include "UI/ImGuiLayer.h"
#include "UI/SceneHierarchyPanel.h"
#include "UI/SideDrawer.h"
#include "UI/TransformGizmo.h"

#include <glad/gl.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

// Where to draw a point light's marker in the 3D view: a small diamond (a
// cube stood on a corner) at the light, the same size whatever the scale of
// the entity it belongs to.
static glm::mat4 LightMarkerMatrix(const glm::mat4& world)
{
    glm::mat4 marker = glm::translate(glm::mat4(1.0f), glm::vec3(world[3]));
    marker = glm::rotate(marker, glm::radians(45.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    marker = glm::rotate(marker, glm::radians(35.26f), glm::vec3(1.0f, 0.0f, 0.0f));
    return glm::scale(marker, glm::vec3(0.15f));
}

int main(int /*argc*/, char* /*argv*/[])
{
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << '\n';
        return 1;
    }

    // Request an OpenGL 4.6 core context: 4.3+ adds compute shaders and
    // shader storage buffers (needed for the path tracer) and debug output.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#ifndef NDEBUG
    // Debug builds get a debug context, so the driver reports GL errors and
    // warnings to the callback installed below.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24); // depth buffer for 3D
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8); // masks the selection outline

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

    // Load all OpenGL 4.6 functions from the driver. Must happen after the
    // context exists and before any other gl* call. Returns the version the
    // driver actually provides (0 on failure).
    const int glVersion = gladLoadGL(reinterpret_cast<GLADloadfunc>(SDL_GL_GetProcAddress));
    if (!glVersion || !GLAD_GL_VERSION_4_6)
    {
        std::cerr << "OpenGL 4.6 is required, but the driver provides "
                  << GLAD_VERSION_MAJOR(glVersion) << '.' << GLAD_VERSION_MINOR(glVersion) << '\n';
        SDL_GL_DestroyContext(glContext);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

#ifndef NDEBUG
    // Print driver-reported problems as they happen, instead of having to
    // call glGetError after everything. Synchronous output makes the message
    // arrive inside the gl* call that caused it, so a breakpoint here shows
    // the culprit on the call stack.
    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    glDebugMessageCallback([](GLenum /*source*/, GLenum type, GLuint id, GLenum severity, GLsizei /*length*/,
                              const GLchar* message, const void* /*userParam*/) {
        // Notifications are chatty info (e.g. "buffer will use video memory").
        if (severity == GL_DEBUG_SEVERITY_NOTIFICATION)
            return;
        std::cerr << "[GL " << (type == GL_DEBUG_TYPE_ERROR ? "error" : "warning") << ' ' << id << "] "
                  << message << '\n';
    }, nullptr);
#endif

    SDL_GL_SetSwapInterval(1); // vsync

    // Only draw a pixel if it is closer to the camera than what is already there.
    glEnable(GL_DEPTH_TEST);

    // Backface culling: skip triangles facing away from the camera (the far
    // side of a closed mesh can't be seen anyway). "Front" is
    // counter-clockwise as seen from outside, how all our meshes are wound.
    // Turned on or off each frame from the Scene settings.
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    std::cout << "OpenGL " << glGetString(GL_VERSION) << '\n';

    // Hardware ray tracing. Optional: if it fails, rendering stays OpenGL-only.
    OptixContext optix;

    // Assets are copied next to the executable by the build, so look them up
    // relative to the exe rather than the current working directory.
    const char* basePath = SDL_GetBasePath();
    const std::filesystem::path assetDir =
        std::filesystem::path(basePath ? basePath : "") / "assets";

    // The asset files the editor works on (textures, and everything the
    // Content Browser shows). In a development build that is the project's
    // own assets folder, so files added, renamed or deleted in the editor
    // change the project rather than the build's copy.
    std::filesystem::path contentRoot = assetDir;
#ifdef MYENGINE_SOURCE_ASSETS_DIR
    if (std::error_code error; std::filesystem::is_directory(u8"" MYENGINE_SOURCE_ASSETS_DIR, error))
        contentRoot = u8"" MYENGINE_SOURCE_ASSETS_DIR;
#endif

    int exitCode = 0;

    // Everything that owns GL objects lives in this scope, so it is destroyed
    // while the GL context still exists.
    {
        ImGuiLayer ui(window, glContext);

        Shader shader(assetDir / "shaders/lit.vert", assetDir / "shaders/lit.frag");
        // Draws the scene from the sun's point of view, recording only depth.
        Shader depthShader(assetDir / "shaders/shadow_depth.vert", assetDir / "shaders/shadow_depth.frag");
        // Position-only passes: a solid color (selection outline), and entity
        // IDs for clicking objects in the 3D view.
        Shader flatShader(assetDir / "shaders/flat.vert", assetDir / "shaders/flat.frag");
        Shader pickShader(assetDir / "shaders/flat.vert", assetDir / "shaders/pick.frag");
        if (!shader.IsValid() || !depthShader.IsValid() || !flatShader.IsValid() || !pickShader.IsValid())
            exitCode = 1;

        ShadowMap shadowMap(2048);
        ObjectPicker picker;

        // --- Assets ---
        AssetLibrary assets;
        const Mesh* cubeMesh = assets.AddMesh("Cube", Mesh::CreateCube());
        const Mesh* quadMesh = assets.AddMesh("Quad", Mesh::CreateQuad());
        assets.AddMesh("Plane", Mesh::CreatePlane(1.0f, 1.0f));
        // 50x50 floor with the texture tiled 25 times each way (one tile per
        // 2 units), stretching into the distance to show off mipmapping.
        const Mesh* floorMesh = assets.AddMesh("Floor 50x50", Mesh::CreatePlane(50.0f, 25.0f));

        // Ray tracing copies of the textures materials use, made when they
        // are first needed.
        TextureCache rayTracingTextures;

        // Ray tracing copies of the meshes. Built up front so the first
        // ray traced frame doesn't stall building them.
        MeshAccelCache meshAccels(optix);
        if (optix.IsValid())
        {
            int built = 0;
            size_t accelBytes = 0;
            for (const AssetLibrary::NamedMesh& entry : assets.GetMeshes())
            {
                if (const MeshAccel* accel = meshAccels.Get(*entry.mesh))
                {
                    ++built;
                    accelBytes += accel->GetAccelSize();
                }
            }
            std::cout << "Built " << built << '/' << assets.GetMeshes().size()
                      << " mesh acceleration structures (" << accelBytes << " bytes)\n";
        }

        // Every image in assets/textures. More can be imported from the
        // Assets panel or dropped onto the window.
        const std::filesystem::path textureFolder = contentRoot / "textures";
        assets.ImportFolder(textureFolder);
        const Texture* checker = assets.ImportTexture(textureFolder / "checker.png");

        // Fills empty material texture slots: white leaves values unchanged.
        const unsigned char whitePixel[] = { 255, 255, 255, 255 };
        const Texture whiteTexture(whitePixel, 1, 1);

        // Used by entities with no material.
        Material defaultMaterial;
        defaultMaterial.name = "Default";

        // --- Starting materials ---
        Material* floorMaterial = assets.CreateMaterial("Floor");
        floorMaterial->baseColor = glm::vec3(1.0f);
        floorMaterial->baseColorMap = checker;
        floorMaterial->roughness = 0.8f; // mostly matte

        Material* checkerMaterial = assets.CreateMaterial("Checker");
        checkerMaterial->baseColor = glm::vec3(1.0f);
        checkerMaterial->baseColorMap = checker;
        checkerMaterial->doubleSided = true; // for the lone flat quad, seen from both sides

        Material* orangeMaterial = assets.CreateMaterial("Orange Checker");
        orangeMaterial->baseColor = glm::vec3(1.0f, 0.65f, 0.35f);
        orangeMaterial->baseColorMap = checker;
        orangeMaterial->roughness = 0.35f;

        Material* moonMaterial = assets.CreateMaterial("Moon");
        moonMaterial->baseColor = glm::vec3(0.6f, 0.8f, 1.0f);
        moonMaterial->roughness = 0.3f;

        // --- Starting scene ---
        Scene scene;

        Entity& floorEntity = scene.CreateEntity("Floor");
        floorEntity.mesh = floorMesh;
        floorEntity.material = floorMaterial;
        floorEntity.transform.position.y = -0.5f;

        Entity& quadEntity = scene.CreateEntity("Quad");
        quadEntity.mesh = quadMesh;
        quadEntity.material = checkerMaterial;

        Entity& cubeEntity = scene.CreateEntity("Cube");
        cubeEntity.mesh = cubeMesh;
        cubeEntity.material = orangeMaterial;
        cubeEntity.transform.position = glm::vec3(2.0f, 0.0f, 0.0f);

        // A child of the cube: rotate or move the cube and this follows.
        Entity& moonEntity = scene.CreateEntity("Moon", &cubeEntity);
        moonEntity.mesh = cubeMesh;
        moonEntity.material = moonMaterial;
        moonEntity.transform.position = glm::vec3(1.2f, 0.6f, 0.0f);
        moonEntity.transform.scale = glm::vec3(0.35f);

        // A warm lamp between the quad and the cube. More point lights can
        // be added from the Hierarchy's create menu.
        Entity& lampEntity = scene.CreateEntity("Point Light");
        lampEntity.light = PointLight{ glm::vec3(1.0f, 0.6f, 0.3f), 1.5f, 6.0f, true };
        lampEntity.transform.position = glm::vec3(1.0f, 0.0f, 0.9f);

        // Ray tracing: the scene as the ray tracer sees it (rebuilt each
        // frame while in use) and the renderer. The programs are compiled
        // by the build into raytracing/ next to the exe.
        SceneAccel sceneAccel(optix);
        RayTracer rayTracer(optix, std::filesystem::path(basePath ? basePath : "") / "raytracing/programs.optixir");

        SceneHierarchyPanel hierarchyPanel;
        TransformGizmo transformGizmo; // move/rotate/scale handles on the selection
        AssetsPanel assetsPanel(window, textureFolder);
        ContentBrowserPanel contentBrowser(window, contentRoot);
        UndoHistory undoHistory(scene, assets); // Ctrl+Z / Ctrl+Y

        // Back and a little to the right so the whole scene is in view.
        Camera camera(glm::vec3(1.0f, 0.5f, 6.0f));

        // Settings the UI can edit.
        glm::vec3 clearColor(0.10f, 0.12f, 0.18f);
        bool showImGuiDemo = false;
        bool backfaceCulling = true;
        // Draw the camera view with the RT cores instead of rasterizing it.
        bool rayTracingEnabled = false;
        // Real-time defaults: few paths per frame, relying on reuse of past
        // frames and the denoiser to hide the noise.
        int rayTracingBounces = 2;
        int rayTracingSamples = 1;
        bool rayTracingDenoise = true;
        bool rayTracingDenoiseHalf = true;
        bool rayTracingReflections = true;
        int rayTracingMovingHistory = 16;

        // A single directional "sun" light. Its direction is given as two
        // angles: azimuth spins it around the vertical axis, elevation is how
        // high above the horizon it sits (90 = straight overhead).
        float lightAzimuth = 35.0f;
        float lightElevation = 50.0f;
        glm::vec3 lightColor(1.0f, 0.96f, 0.9f);
        glm::vec3 ambientColor(0.18f, 0.2f, 0.25f);

        // Shadows are only drawn within this distance of the camera. Larger
        // covers more ground but spreads the shadow map's texels thinner,
        // making edges blockier.
        bool shadowsEnabled = true;
        float shadowDistance = 15.0f;
        bool showShadowMap = false;

        // Mouse look is active while right mouse is held OR while locked on
        // with Left Alt.
        bool rightMouseHeld = false;
        bool mouseLookLocked = false;

        // Where the 3D view was left-clicked this frame (window coordinates),
        // to select whatever is under it once the frame is being rendered.
        std::optional<glm::vec2> pendingPick;

        // Engine settings, in a Blender-style sidebar on the right edge of
        // the 3D view: one tab per group.
        SideDrawer settingsDrawer;
        const SideDrawer::Tab settingsTabs[] = {
            { "Camera", [&] {
                const glm::vec3& camPos = camera.GetPosition();
                ImGui::Text("Position: %.2f, %.2f, %.2f", camPos.x, camPos.y, camPos.z);
                ImGui::SliderFloat("Move speed", &camera.moveSpeed, 0.5f, 20.0f);
                ImGui::SliderFloat("Sensitivity", &camera.mouseSensitivity, 0.01f, 0.5f);
                ImGui::SliderFloat("Field of view", &camera.fieldOfView, 30.0f, 110.0f);
                ImGui::Checkbox("Mouse look locked", &mouseLookLocked);
            } },
            { "Lighting", [&] {
                ImGui::SeparatorText("Sun");
                ImGui::SliderFloat("Azimuth", &lightAzimuth, -180.0f, 180.0f, "%.0f deg");
                ImGui::SliderFloat("Elevation", &lightElevation, -90.0f, 90.0f, "%.0f deg");
                ImGui::ColorEdit3("Color", &lightColor.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
                ImGui::ColorEdit3("Ambient", &ambientColor.x);

                ImGui::SeparatorText("Shadows");
                ImGui::Checkbox("Enabled", &shadowsEnabled);
                ImGui::SliderFloat("Distance", &shadowDistance, 2.0f, 50.0f, "%.0f");
                ImGui::Checkbox("Show shadow map", &showShadowMap);
                if (showShadowMap)
                {
                    // Depth as greyscale: black = close to the sun. Flipped
                    // vertically because GL textures start at the bottom row.
                    const float size = ImGui::GetContentRegionAvail().x;
                    ImGui::Image(static_cast<ImTextureID>(shadowMap.GetTextureId()), ImVec2(size, size),
                                 ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
                }
            } },
            { "Scene", [&] {
                const ImGuiIO& io = ImGui::GetIO();
                ImGui::Text("%.1f FPS (%.2f ms)", io.Framerate, 1000.0f / io.Framerate);
                ImGui::ColorEdit3("Background", &clearColor.x);
                ImGui::Checkbox("Backface culling", &backfaceCulling);

                ImGui::BeginDisabled(!rayTracer.IsValid());
                ImGui::Checkbox("Ray tracing", &rayTracingEnabled);
                ImGui::EndDisabled();
                ImGui::SetItemTooltip(rayTracer.IsValid()
                    ? "Render with hardware ray tracing (OptiX): PBR shading, exact\n"
                      "shadows and ambient light bouncing between surfaces."
                    : "Unavailable: needs an NVIDIA RTX GPU (see the console for why it failed)");
                if (rayTracingEnabled && rayTracer.IsValid())
                {
                    ImGui::Checkbox("Reflections", &rayTracingReflections);
                    ImGui::SetItemTooltip("Trace reflections, sharp to blurry by each material's roughness\n"
                                          "(and tinted by metals). Off: surfaces reflect a plain sky.");
                    ImGui::SliderInt("Bounces", &rayTracingBounces, 0, 8);
                    ImGui::SetItemTooltip("How many times light bounces between surfaces (diffusely or\n"
                                          "as reflections). 0 = flat ambient, like the raster view.");
                    ImGui::SliderInt("Samples / frame", &rayTracingSamples, 1, 16);
                    ImGui::SetItemTooltip("Light paths per pixel each frame: less noise while\n"
                                          "moving, at the cost of speed.");

                    ImGui::BeginDisabled(!rayTracer.IsDenoiserAvailable());
                    ImGui::Checkbox("Denoise", &rayTracingDenoise);
                    ImGui::EndDisabled();
                    ImGui::SetItemTooltip("Clean up the remaining noise with the OptiX AI denoiser.");
                    ImGui::BeginDisabled(!rayTracingDenoise);
                    ImGui::Checkbox("Half-res denoise", &rayTracingDenoiseHalf);
                    ImGui::EndDisabled();
                    ImGui::SetItemTooltip("Denoise the bounced light at half resolution: about a quarter\n"
                                          "of the cost, slightly softer bounce light in tight corners.");
                    ImGui::SliderInt("History while moving", &rayTracingMovingHistory, 1, 64);
                    ImGui::SetItemTooltip("Past frames each pixel reuses while the camera or objects move.\n"
                                          "More is smoother but smears changes; fewer adapts faster.");

                    const RayTracer::GpuTimings& timings = rayTracer.GetTimings();
                    ImGui::Text("GPU: trace %.2f ms, denoise %.2f ms", timings.trace, timings.denoise);
                    ImGui::Text("Accumulated: %u / %u frames", rayTracer.GetAccumulatedFrames(),
                                RayTracer::kMaxAccumulatedFrames);
                    ImGui::SetItemTooltip("While nothing changes, frames keep averaging until the\n"
                                          "image is final, then the GPU rests. Moving things keeps a\n"
                                          "short history; changing the lighting starts over.");
                }
            } },
            { "Help", [&] {
                ImGui::TextDisabled("WASD move, Q/E down/up, Shift sprint");
                ImGui::TextDisabled("Hold right mouse or Left Alt to look");
                ImGui::TextDisabled("Ctrl+Z undo, Ctrl+Y (or Ctrl+Shift+Z) redo");
                ImGui::TextDisabled("G / R / T: move / rotate / scale, Ctrl snaps");
                ImGui::TextDisabled("Esc quit");
                ImGui::Checkbox("Show ImGui demo", &showImGuiDemo);
            } },
        };

        Uint64 lastTicks = SDL_GetTicksNS();
        bool running = shader.IsValid();
        bool wasRayTraced = false; // last frame's renderer

        while (running)
        {
            // --- Events ---
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                // While flying the camera the cursor is hidden, so the UI
                // shouldn't see mouse input (it would hover/click blindly).
                // Button releases always go through: the right-click that
                // starts mouse look reaches the UI before look mode turns on,
                // so its release must too, or the UI thinks the button is
                // stuck down (it then ignores hovering, and keeps the mouse
                // captured so even the window's title bar stops responding).
                const bool isMouseEvent = event.type == SDL_EVENT_MOUSE_MOTION
                                       || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                                       || event.type == SDL_EVENT_MOUSE_WHEEL;
                if (!(isMouseEvent && SDL_GetWindowRelativeMouseMode(window)))
                    ui.ProcessEvent(event);

                if (event.type == SDL_EVENT_QUIT)
                    running = false;
                // Files dragged from Explorer: dropped onto the Content
                // Browser they are copied into the project; anywhere else,
                // images (or folders of them) are imported as textures where
                // they are. SDL gives UTF-8 paths.
                else if (event.type == SDL_EVENT_DROP_FILE && event.drop.data)
                {
                    std::filesystem::path dropped(reinterpret_cast<const char8_t*>(event.drop.data));
                    if (contentBrowser.Contains(event.drop.x, event.drop.y))
                        contentBrowser.QueueCopyIn(std::move(dropped));
                    else
                        assets.QueueImport(std::move(dropped));
                }
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
                // Left-clicking the 3D view (not a panel, not while looking
                // around) selects the object under the cursor.
                else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT
                         && !ui.WantsMouse() && !transformGizmo.WantsMouse()
                         && !SDL_GetWindowRelativeMouseMode(window))
                    pendingPick = glm::vec2(event.button.x, event.button.y);
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

            // Undo/redo first, so every panel draws the restored state. The
            // selection goes if the undo took the selected entity away.
            if (undoHistory.HandleShortcuts() && !scene.Contains(hierarchyPanel.GetSelected()))
                hierarchyPanel.SetSelected(nullptr);

            // Load images picked in the file dialog or dropped on the window.
            assets.ProcessQueuedImports();

            hierarchyPanel.Draw(scene, assets);
            assetsPanel.Draw(assets, scene, hierarchyPanel.GetSelected());
            contentBrowser.Draw(assets);
            settingsDrawer.Draw(ui.GetViewportMin(), ui.GetViewportMax(), settingsTabs);

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
            // --- Render ---
            int width = 0, height = 0;
            SDL_GetWindowSizeInPixels(window, &width, &height);
            // Recomputed each frame so the image never stretches on resize.
            // (height is 0 while the window is minimised.)
            const float aspect = height > 0 ? static_cast<float>(width) / height : 1.0f;
            const glm::mat4 view = camera.GetViewMatrix();
            const glm::mat4 projection = camera.GetProjectionMatrix(aspect);

            // Handles on the selected object. Dragging them changes its
            // transform, so this comes before anything is drawn with it.
            transformGizmo.Draw(hierarchyPanel.GetSelected(), view, projection, ui.GetViewportMin(),
                                ui.GetViewportMax());

            // Ray traced frames replace the shadow map and the lit pass; the
            // pick and selection outline passes still rasterize.
            const bool rayTraced = rayTracingEnabled && rayTracer.IsValid();
            // Frames rendered with raster in between leave nothing to reuse.
            if (rayTraced && !wasRayTraced)
                rayTracer.ResetHistory();
            wasRayTraced = rayTraced;

            // Point from the sun towards the scene (hence the minus signs:
            // the angles describe where the sun is in the sky).
            const float azimuth = glm::radians(lightAzimuth);
            const float elevation = glm::radians(lightElevation);
            const glm::vec3 toSun(std::cos(elevation) * std::sin(azimuth),
                                  std::sin(elevation),
                                  std::cos(elevation) * std::cos(azimuth));
            const glm::vec3 lightDir = -toSun;

            // Applies to both passes. (ImGui turns culling off while drawing
            // the UI and restores it afterwards.)
            if (backfaceCulling)
                glEnable(GL_CULL_FACE);
            else
                glDisable(GL_CULL_FACE);

            // Pass 1: shadow map. Draw the scene from the sun, keeping only
            // how far each surface is from it. The depth shader ignores the
            // material uniforms Scene::Draw sets.
            const ShadowMap::Bounds shadowBounds = ShadowMap::FitToView(camera, aspect, shadowDistance);
            const glm::mat4 lightSpace = shadowMap.GetLightSpaceMatrix(lightDir, shadowBounds);
            if (shadowsEnabled && !rayTraced)
            {
                shadowMap.BeginRender();
                depthShader.Bind();
                depthShader.SetMat4("uLightSpace", lightSpace);
                scene.Draw(depthShader, defaultMaterial, whiteTexture);
                shadowMap.EndRender();
            }

            // Click to select: draw just the clicked pixel with each entity
            // writing its ID, and select whichever one ends up there (or
            // nothing, which deselects).
            if (pendingPick && width > 0 && height > 0)
            {
                // SDL reports the mouse in window coordinates, which differ
                // from framebuffer pixels on high-DPI displays.
                int windowWidth = 0, windowHeight = 0;
                SDL_GetWindowSize(window, &windowWidth, &windowHeight);
                const glm::vec2 viewSize(width, height);
                const glm::vec2 pixel = *pendingPick * viewSize / glm::vec2(windowWidth, windowHeight);

                std::vector<Entity*> drawnEntities;
                picker.BeginRender();
                pickShader.Bind();
                pickShader.SetMat4("uView", view);
                pickShader.SetMat4("uProjection", ObjectPicker::GetPickMatrix(pixel, viewSize) * projection);
                scene.Draw(pickShader, defaultMaterial, whiteTexture, &drawnEntities);
                // Light markers are clickable too, and win over what's behind
                // them since they are drawn on top.
                glDisable(GL_DEPTH_TEST);
                scene.ForEachVisible([&](Entity& entity, const glm::mat4& world) {
                    if (!entity.light)
                        return;
                    drawnEntities.push_back(&entity);
                    pickShader.SetUInt("uEntityId", static_cast<unsigned int>(drawnEntities.size()));
                    pickShader.SetMat4("uModel", LightMarkerMatrix(world));
                    cubeMesh->Draw();
                });
                glEnable(GL_DEPTH_TEST);
                const std::uint32_t id = picker.EndRender();

                hierarchyPanel.SetSelected(id > 0 && id <= drawnEntities.size() ? drawnEntities[id - 1] : nullptr);
                pendingPick.reset();
            }

            // Every visible point light, for whichever renderer draws the scene.
            const std::vector<ScenePointLight> pointLights = scene.GatherPointLights();

            // Pass 2: the lit scene, as seen by the camera.
            glViewport(0, 0, width, height);
            glClearColor(clearColor.r, clearColor.g, clearColor.b, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

            if (rayTraced)
            {
                sceneAccel.Build(scene, meshAccels, rayTracingTextures, defaultMaterial);

                RayTracer::FrameSettings settings{};
                settings.view = view;
                settings.projection = projection;
                settings.lightDir = lightDir;
                // The pickers show sRGB; the ray tracer lights in linear space.
                settings.lightColor = SrgbToLinear(lightColor);
                settings.ambientColor = SrgbToLinear(ambientColor);
                settings.background = SrgbToLinear(clearColor);
                settings.shadowsEnabled = shadowsEnabled;
                settings.reflections = rayTracingReflections;
                settings.cullBackFaces = backfaceCulling;
                settings.maxBounces = rayTracingBounces;
                settings.samplesPerPixel = rayTracingSamples;
                settings.denoise = rayTracingDenoise;
                settings.denoiseHalfResolution = rayTracingDenoiseHalf;
                settings.movingHistoryFrames = rayTracingMovingHistory;
                settings.pointLights = pointLights;
                rayTracer.Render(sceneAccel, settings, width, height);
            }
            else
            {
                shader.Bind();
                shader.SetMat4("uView", view);
                shader.SetMat4("uProjection", projection);
                shader.SetVec3("uViewPos", camera.GetPosition());
                shader.SetVec3("uLightDir", lightDir);
                // The pickers show sRGB; the shader lights in linear space.
                shader.SetVec3("uLightColor", SrgbToLinear(lightColor));
                shader.SetVec3("uAmbientColor", SrgbToLinear(ambientColor));

                // The shader takes a fixed number of point lights: if there
                // are more, the ones nearest the camera.
                std::vector<ScenePointLight> nearest = pointLights;
                constexpr size_t kMaxRasterPointLights = 16; // MAX_POINT_LIGHTS in lit.frag
                if (nearest.size() > kMaxRasterPointLights)
                {
                    const glm::vec3 eye = camera.GetPosition();
                    std::partial_sort(nearest.begin(), nearest.begin() + kMaxRasterPointLights, nearest.end(),
                                      [&](const ScenePointLight& a, const ScenePointLight& b) {
                                          return glm::distance(a.position, eye) < glm::distance(b.position, eye);
                                      });
                    nearest.resize(kMaxRasterPointLights);
                }
                shader.SetInt("uPointLightCount", static_cast<int>(nearest.size()));
                for (size_t i = 0; i < nearest.size(); ++i)
                {
                    const std::string prefix = "uPointLights[" + std::to_string(i) + "].";
                    shader.SetVec3((prefix + "position").c_str(), nearest[i].position);
                    shader.SetVec3((prefix + "radiance").c_str(), nearest[i].radiance);
                    shader.SetFloat((prefix + "range").c_str(), nearest[i].range);
                }

                shader.SetInt("uShadowsEnabled", shadowsEnabled);
                shader.SetMat4("uLightSpace", lightSpace);
                // About 1.5 shadow-map texels, in world units.
                shader.SetFloat("uShadowNormalOffset",
                                1.5f * 2.0f * shadowBounds.radius / shadowMap.GetResolution());
                shader.SetFloat("uShadowDistance", shadowDistance);

                // Material maps go in the units Scene binds them to per entity;
                // the shadow map stays in unit 1 for the whole pass.
                Scene::SetMaterialSamplers(shader);
                shader.SetInt("uShadowMap", 1);
                shadowMap.BindTexture(1);
                scene.Draw(shader, defaultMaterial, whiteTexture);
            }

            // Light markers: a small diamond in the light's color at each
            // point light (orange when selected), on top of everything so a
            // light is easy to find and click.
            const Entity* selected = hierarchyPanel.GetSelected();
            flatShader.Bind();
            flatShader.SetMat4("uView", view);
            flatShader.SetMat4("uProjection", projection);
            glDisable(GL_DEPTH_TEST);
            scene.ForEachVisible([&](Entity& entity, const glm::mat4& world) {
                if (!entity.light)
                    return;
                flatShader.SetMat4("uModel", LightMarkerMatrix(world));
                flatShader.SetVec3("uColor", &entity == selected ? glm::vec3(1.0f, 0.55f, 0.1f) : entity.light->color);
                cubeMesh->Draw();
            });
            glEnable(GL_DEPTH_TEST);

            // Selection outline: an orange border around the selected
            // object's silhouette, visible even through things in front.
            if (selected && selected->mesh && selected->IsVisibleInHierarchy())
            {
                flatShader.Bind();
                flatShader.SetMat4("uView", view);
                flatShader.SetMat4("uProjection", projection);
                flatShader.SetMat4("uModel", selected->GetWorldMatrix());
                flatShader.SetVec3("uColor", glm::vec3(1.0f, 0.55f, 0.1f));

                glDisable(GL_DEPTH_TEST);
                glDisable(GL_CULL_FACE); // the whole silhouette, all sides
                glEnable(GL_STENCIL_TEST);

                // 1) Mark the object's silhouette in the stencil buffer,
                //    without drawing any color.
                glStencilFunc(GL_ALWAYS, 1, 0xFF);
                glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
                glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                selected->mesh->Draw();
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

                // 2) Draw its edges as thick lines, but only outside the
                //    silhouette: what's left is an outline around it.
                glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
                glStencilMask(0x00);
                glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
                glLineWidth(4.0f);
                selected->mesh->Draw();

                glLineWidth(1.0f);
                glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
                glStencilMask(0xFF);
                glDisable(GL_STENCIL_TEST);
                glEnable(GL_DEPTH_TEST);
                if (backfaceCulling)
                    glEnable(GL_CULL_FACE);
            }

            // Record an undo step if this frame's edits changed anything
            // (waits while a gizmo handle is still being dragged).
            undoHistory.Update(transformGizmo.IsDragging());

            // UI last, so it draws on top of the scene.
            ui.EndFrame();

            SDL_GL_SwapWindow(window);
        }
    } // UI, scene, assets and shader destroyed here, before the context

    SDL_GL_DestroyContext(glContext);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return exitCode;
}
