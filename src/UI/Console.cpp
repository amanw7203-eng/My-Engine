#include "UI/Console.h"

#include <imgui.h>
#include <imgui_internal.h> // FindWindowByName, for docking beside the Content Browser

#include <iostream>

namespace
{
    // Oldest lines are dropped past this, so a chatty script can't grow the
    // log without bound.
    constexpr size_t kMaxLines = 5000;
}

void Console::Log(Level level, std::string message)
{
    (level == Level::Info ? std::cout : std::cerr) << message << '\n';

    std::lock_guard lock(m_Mutex);
    m_Lines.push_back({ level, std::move(message) });
    if (m_Lines.size() > kMaxLines)
        m_Lines.erase(m_Lines.begin(), m_Lines.begin() + (m_Lines.size() - kMaxLines));
}

void Console::Clear()
{
    std::lock_guard lock(m_Mutex);
    m_Lines.clear();
}

void Console::Draw()
{
    // The first time it appears in a saved layout that predates it, open it
    // as a tab beside the Content Browser rather than floating.
    if (const ImGuiWindow* browser = ImGui::FindWindowByName("Content Browser"); browser && browser->DockId)
        ImGui::SetNextWindowDockID(browser->DockId, ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Console"))
    {
        ImGui::End();
        return;
    }

    if (ImGui::SmallButton("Clear"))
        Clear();
    ImGui::Separator();

    if (ImGui::BeginChild("##lines", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar))
    {
        std::lock_guard lock(m_Mutex);
        // Only the visible lines are laid out, however long the log.
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_Lines.size()));
        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            {
                const Line& line = m_Lines[i];
                if (line.level == Level::Info)
                    ImGui::TextUnformatted(line.text.c_str());
                else
                    ImGui::TextColored(line.level == Level::Error ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f)
                                                                  : ImVec4(1.0f, 0.8f, 0.35f, 1.0f),
                                       "%s", line.text.c_str());
            }
        }
        // Follow new lines, unless the user has scrolled up to read. (The
        // scroll limits are last frame's, so this reads "was at the bottom".)
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();
}
