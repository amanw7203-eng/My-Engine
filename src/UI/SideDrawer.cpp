#include "UI/SideDrawer.h"

#include <algorithm>
#include <string>

// A tab button whose label reads top to bottom. ImGui has no rotated text,
// so the label is drawn normally and its vertices are then turned 90 degrees
// clockwise into place. Returns true when clicked.
static bool VerticalTab(const char* label, bool selected, float width, float padding)
{
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    const ImVec2 size(width, textSize.x + padding * 2.0f);
    const ImVec2 pos = ImGui::GetCursorScreenPos();

    const bool clicked = ImGui::InvisibleButton(label, size);
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImGuiCol bgColor = selected ? ImGuiCol_TabSelected : hovered ? ImGuiCol_TabHovered : ImGuiCol_Tab;
    // Round the corners facing the view (left); the right side sits flat
    // against the view's edge.
    drawList->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), ImGui::GetColorU32(bgColor),
                            ImGui::GetStyle().TabRounding, ImDrawFlags_RoundCornersLeft);

    // Before rotating, the text runs sideways far past this narrow window,
    // and ImGui would skip the glyphs outside the window's clip rectangle.
    const int firstVertex = drawList->VtxBuffer.Size;
    drawList->PushClipRectFullScreen();
    drawList->AddText(pos, ImGui::GetColorU32(ImGuiCol_Text), label);
    drawList->PopClipRect();

    // Rotating (x, y) -> (-y, x) around the text's top-left corner turns its
    // left-to-right run into top-to-bottom. `target` is where that corner
    // lands: centred across the tab, `padding` below its top.
    const ImVec2 target(pos.x + (size.x + textSize.y) * 0.5f, pos.y + padding);
    for (int i = firstVertex; i < drawList->VtxBuffer.Size; ++i)
    {
        ImDrawVert& vertex = drawList->VtxBuffer[i];
        const float dx = vertex.pos.x - pos.x;
        const float dy = vertex.pos.y - pos.y;
        vertex.pos = ImVec2(target.x - dy, target.y + dx);
    }

    return clicked;
}

void SideDrawer::Draw(const ImVec2& areaMin, const ImVec2& areaMax, std::span<const Tab> tabs)
{
    const float fontSize = ImGui::GetFontSize();
    const float stripWidth = fontSize * 1.6f;
    const float tabPadding = fontSize * 0.6f;
    const float areaWidth = areaMax.x - areaMin.x;
    const float areaHeight = areaMax.y - areaMin.y;
    if (areaWidth < stripWidth || areaHeight <= 0.0f)
        return; // the view is squeezed shut; nowhere to put the drawer

    const ImGuiWindowFlags fixedFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
                                      | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing;

    // --- Tab strip, hugging the right edge of the area ---
    ImGui::SetNextWindowPos(ImVec2(areaMax.x - stripWidth, areaMin.y + tabPadding));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, fontSize * 0.15f));
    ImGui::Begin("##SideDrawerTabs", nullptr,
                 fixedFlags | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground
                     | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav);
    for (int i = 0; i < static_cast<int>(tabs.size()); ++i)
    {
        if (VerticalTab(tabs[i].name, i == m_OpenTab, stripWidth, tabPadding))
            m_OpenTab = (i == m_OpenTab) ? -1 : i;
    }
    ImGui::End();
    ImGui::PopStyleVar(3);

    if (m_OpenTab < 0 || m_OpenTab >= static_cast<int>(tabs.size()))
        return;

    // --- Open panel: a small overlay just left of the tabs, level with
    // their top, only as tall as its contents (up to the view's height) ---
    const float gap = fontSize * 0.25f;
    const float panelWidth = std::min(fontSize * 18.0f, areaWidth - stripWidth - gap);
    const float top = areaMin.y + tabPadding;
    ImGui::SetNextWindowPos(ImVec2(areaMax.x - stripWidth - gap - panelWidth, top));
    // Width is fixed; AlwaysAutoResize picks the height within these limits.
    ImGui::SetNextWindowSizeConstraints(ImVec2(panelWidth, 0.0f),
                                        ImVec2(panelWidth, std::max(areaMax.y - top - tabPadding, 0.0f)));

    // "###" keeps one window ID while the title changes with the tab.
    const Tab& tab = tabs[m_OpenTab];
    const std::string title = std::string(tab.name) + "###SideDrawerPanel";
    bool open = true;
    if (ImGui::Begin(title.c_str(), &open,
                     fixedFlags | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse))
        tab.drawContents();
    ImGui::End();

    if (!open)
        m_OpenTab = -1;
}
