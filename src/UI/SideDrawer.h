#pragma once

#include <imgui.h>

#include <functional>
#include <span>

// A Blender-style sidebar: a column of vertical tabs along the right edge of
// an area (the 3D view). Clicking a tab opens its panel as a small overlay
// to the left of the tabs, sized to fit its contents; clicking it again, or
// the panel's close button, tucks it away. At most one panel is open at a time.
class SideDrawer
{
public:
    struct Tab
    {
        const char* name;
        std::function<void()> drawContents; // ImGui:: calls for the panel body
    };

    // Call between ImGuiLayer::BeginFrame and EndFrame. areaMin/areaMax are
    // screen pixels, e.g. ImGuiLayer::GetViewportMin/Max.
    void Draw(const ImVec2& areaMin, const ImVec2& areaMax, std::span<const Tab> tabs);

private:
    int m_OpenTab = -1; // index into tabs, or -1 when closed
};
