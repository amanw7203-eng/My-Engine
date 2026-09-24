#include "Editor/UndoHistory.h"

#include <imgui.h>

#include <utility>

UndoHistory::UndoHistory(Scene& scene, AssetLibrary& assets)
    : m_Scene(scene)
    , m_Assets(assets)
{
}

bool UndoHistory::HandleShortcuts()
{
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput)
        return false;

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z) && CanUndo())
    {
        Undo();
        return true;
    }
    if ((ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y) ||
         ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z)) &&
        CanRedo())
    {
        Redo();
        return true;
    }
    return false;
}

void UndoHistory::Update(bool editing)
{
    // Mid-edit (a slider or handle held, a field being typed in): wait for
    // it to finish, so the whole edit becomes one step.
    if (editing || ImGui::IsAnyItemActive())
        return;

    Snapshot now = Capture();
    if (!m_HasCurrent)
    {
        m_Current = std::move(now); // the starting point
        m_HasCurrent = true;
        return;
    }
    if (now == m_Current)
        return;

    m_Undo.push_back(std::move(m_Current));
    if (m_Undo.size() > kMaxSteps)
        m_Undo.pop_front();
    m_Current = std::move(now);
    m_Redo.clear(); // a new edit makes the undone steps unreachable
}

void UndoHistory::Undo()
{
    if (m_Undo.empty())
        return;
    m_Redo.push_back(std::move(m_Current));
    m_Current = std::move(m_Undo.back());
    m_Undo.pop_back();
    Restore(m_Current);
}

void UndoHistory::Redo()
{
    if (m_Redo.empty())
        return;
    m_Undo.push_back(std::move(m_Current));
    m_Current = std::move(m_Redo.back());
    m_Redo.pop_back();
    Restore(m_Current);
}

UndoHistory::Snapshot UndoHistory::Capture() const
{
    return { m_Scene.CaptureState(), m_Assets.CaptureState() };
}

void UndoHistory::Restore(const Snapshot& snapshot)
{
    // Either order works: nothing is ever freed, so every pointer in the
    // snapshot is still a live object.
    m_Assets.RestoreState(snapshot.assets);
    m_Scene.RestoreState(snapshot.scene);
}
