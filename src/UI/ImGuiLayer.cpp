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
    ImGui_ImplOpenGL3_Init("#version 460 core");
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
    // left, Hierarchy above Assets on the right, Content Browser and Console
    // along the bottom, 3D viewport in the middle.
    ImGuiDockNode* dockspace = ImGui::DockBuilderGetNode(dockspaceId);
    if (dockspace && dockspace->IsLeafNode() && dockspace->Windows.empty())
    {
        ImGuiID centerId = dockspaceId;
        const ImGuiID leftId = ImGui::DockBuilderSplitNode(centerId, ImGuiDir_Left, 0.22f, nullptr, &centerId);
        ImGuiID rightTopId = ImGui::DockBuilderSplitNode(centerId, ImGuiDir_Right, 0.28f, nullptr, &centerId);
        const ImGuiID rightBottomId = ImGui::DockBuilderSplitNode(rightTopId, ImGuiDir_Down, 0.55f, nullptr, &rightTopId);
        const ImGuiID bottomId = ImGui::DockBuilderSplitNode(centerId, ImGuiDir_Down, 0.3f, nullptr, &centerId);

        ImGui::DockBuilderDockWindow("Inspector", leftId);
        ImGui::DockBuilderDockWindow("Hierarchy", rightTopId);
        ImGui::DockBuilderDockWindow("Assets", rightBottomId);
        ImGui::DockBuilderDockWindow("Content Browser", bottomId);
        ImGui::DockBuilderDockWindow("Console", bottomId); // a tab beside it
        ImGui::DockBuilderFinish(dockspaceId);
    }
    // A layout saved before the Content Browser existed: the first time it
    // appears, give it a strip along the bottom of the 3D view rather than
    // leaving it floating.
    else if (!ImGui::FindWindowSettingsByID(ImHashStr("Content Browser")) && !ImGui::FindWindowByName("Content Browser"))
    {
        if (ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockspaceId))
        {
            const ImGuiID bottomId = ImGui::DockBuilderSplitNode(central->ID, ImGuiDir_Down, 0.3f, nullptr, nullptr);
            ImGui::DockBuilderDockWindow("Content Browser", bottomId);
            ImGui::DockBuilderFinish(dockspaceId);
        }
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
