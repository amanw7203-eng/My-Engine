#pragma once

#include "Assets/AssetLibrary.h"
#include "Scene/Scene.h"

#include <cstddef>
#include <deque>
#include <vector>

// Undo and redo for editing the scene (Ctrl+Z, Ctrl+Y / Ctrl+Shift+Z).
//
// Works like Blender's: rather than every edit recording how to reverse
// itself, the history keeps snapshots of everything editable (entities,
// materials, the texture list) and restores them. After each frame in which
// nothing is mid-edit, the current state is compared with the last recorded
// one; if anything changed, that is one step. So dragging a slider or a
// gizmo handle, or typing a name, is a single step however many frames it
// took, and any edit made anywhere in the editor is covered without that
// code having to know about undo.
//
// Not covered: files changed on disk (Content Browser renames, moves and
// deletes), and view/render settings (camera, sun, ray tracing options).
class UndoHistory
{
public:
    UndoHistory(Scene& scene, AssetLibrary& assets);

    // Handles Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z, unless a text field is being
    // typed in (it has its own undo). Call early in the frame's UI, before
    // the panels, so they draw the restored state. Returns true if the state
    // was changed.
    bool HandleShortcuts();

    // Call once per frame after all editing, with `editing` true while an
    // edit outside ImGui's widgets is in progress (a gizmo drag). Records a
    // step if the state has changed since the last one.
    void Update(bool editing);

    bool CanUndo() const { return !m_Undo.empty(); }
    bool CanRedo() const { return !m_Redo.empty(); }
    void Undo();
    void Redo();

private:
    struct Snapshot
    {
        Scene::State scene;
        AssetLibrary::State assets;

        bool operator==(const Snapshot&) const = default;
    };

    Snapshot Capture() const;
    void Restore(const Snapshot& snapshot);

    // Oldest steps are dropped past this.
    static constexpr std::size_t kMaxSteps = 200;

    Scene& m_Scene;
    AssetLibrary& m_Assets;
    Snapshot m_Current;     // the state as last recorded
    bool m_HasCurrent = false;
    std::deque<Snapshot> m_Undo;
    std::vector<Snapshot> m_Redo;
};
