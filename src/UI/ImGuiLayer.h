#pragma once

#include <SDL3/SDL.h>
#include <imgui.h>

// Sets up Dear ImGui for an SDL3 window with an OpenGL context, and shuts it
// down in the destructor. Must be destroyed before the GL context is.
//
// Per frame:  ProcessEvent() for each SDL event
//             BeginFrame()   -> build UI with ImGui:: calls
//             EndFrame()     -> draws the UI on top of whatever was rendered
class ImGuiLayer
{
public:
    ImGuiLayer(SDL_Window* window, SDL_GLContext glContext);
    ~ImGuiLayer();

    // There is one global ImGui context, so exactly one layer may exist.
    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    void ProcessEvent(const SDL_Event& event);
    void BeginFrame();
    void EndFrame();

    // True while the mouse is over a panel / the keyboard is typing into a
    // widget; the game should ignore that input so both don't react to it.
    bool WantsMouse() const;
    bool WantsKeyboard() const;

    // The see-through middle of the dock space where the 3D scene shows,
    // in screen pixels. Updated by BeginFrame.
    const ImVec2& GetViewportMin() const { return m_ViewportMin; }
    const ImVec2& GetViewportMax() const { return m_ViewportMax; }

private:
    ImVec2 m_ViewportMin{ 0.0f, 0.0f };
    ImVec2 m_ViewportMax{ 0.0f, 0.0f };
};
