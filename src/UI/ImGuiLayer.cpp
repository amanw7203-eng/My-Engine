#include "UI/ImGuiLayer.h"

#include <imgui.h>
#include <imgui_internal.h> // DockBuilder API for the default panel layout
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>

ImGuiLayer::ImGuiLayer(SDL_Window* window, SDL_GLContext glContext)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    // Panels can be dragged together into tabs and snapped to window edges.
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // Keyboard navigation is left off: it uses Alt, which toggles mouse look.

    ImGui::StyleColorsDark();

    // Scale the UI on high-DPI displays (e.g. 150% Windows scaling).
    const float dpiScale = SDL_GetDisplayContentScale(SDL_GetDisplayForWindow(window));
    if (dpiScale > 0.0f)
    {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(dpiScale);
        style.FontScaleDpi = dpiScale;
    }

    ImGui_ImplSDL3_InitForOpenGL(window, glContext);
    ImGui_ImplOpenGL3_Init("#version 330 core");
}

ImGuiLayer::~ImGuiLayer()
{
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void ImGuiLayer::ProcessEvent(const SDL_Event& event)
{
    ImGui_ImplSDL3_ProcessEvent(&event);
}

void ImGuiLayer::BeginFrame()
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    // A dock space covering the window, with a see-through middle so the 3D
    // scene still shows. Panels can be docked to its edges.
    const ImGuiID dockspaceId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(),
                                                             ImGuiDockNodeFlags_PassthruCentralNode);

    // Default layout, only when imgui.ini has no saved one: Inspector on the
    // left, Hierarchy on the right, 3D viewport in the middle.
    ImGuiDockNode* dockspace = ImGui::DockBuilderGetNode(dockspaceId);
    if (dockspace && dockspace->IsLeafNode() && dockspace->Windows.empty())
    {
        ImGuiID centerId = dockspaceId;
        const ImGuiID leftId = ImGui::DockBuilderSplitNode(centerId, ImGuiDir_Left, 0.22f, nullptr, &centerId);
        const ImGuiID rightId = ImGui::DockBuilderSplitNode(centerId, ImGuiDir_Right, 0.28f, nullptr, &centerId);

        ImGui::DockBuilderDockWindow("Inspector", leftId);
        ImGui::DockBuilderDockWindow("Hierarchy", rightId);
        ImGui::DockBuilderFinish(dockspaceId);
    }

    // Remember where the 3D view is, for overlays drawn on top of it.
    if (const ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockspaceId))
    {
        m_ViewportMin = central->Pos;
        m_ViewportMax = ImVec2(central->Pos.x + central->Size.x, central->Pos.y + central->Size.y);
    }
}

void ImGuiLayer::EndFrame()
{
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

bool ImGuiLayer::WantsMouse() const
{
    return ImGui::GetIO().WantCaptureMouse;
}

bool ImGuiLayer::WantsKeyboard() const
{
    return ImGui::GetIO().WantCaptureKeyboard;
}
