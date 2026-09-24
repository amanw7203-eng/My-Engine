#pragma once

#include <glm/glm.hpp>
#include <imgui.h>

class Entity;

// Blender-style handles on the selected entity in the 3D view: drag an arrow
// (or a plane square) to move, a ring to rotate, a box to scale. Hold Ctrl
// to snap. G / R / T switch between moving, rotating and scaling, and a
// toolbar in the top-left corner of the view shows and switches the tool.
// Built on ImGuizmo.
class TransformGizmo
{
public:
    enum class Tool
    {
        Move,
        Rotate,
        Scale,
    };

    // Call once per frame between ImGuiLayer::BeginFrame and EndFrame, after
    // the camera has moved, with the same view and projection the scene is
    // drawn with. viewMin/viewMax is the 3D view's area in screen pixels.
    // Changes `selected`'s transform while its handles are dragged.
    void Draw(Entity* selected, const glm::mat4& view, const glm::mat4& projection,
              const ImVec2& viewMin, const ImVec2& viewMax);

    // True if the mouse is over a handle, or a handle is being dragged. A
    // click then belongs to the gizmo, not to selecting what is behind it.
    bool WantsMouse() const { return m_WantsMouse; }
    // True while a handle is being dragged.
    bool IsDragging() const { return m_Dragging; }

private:
    void DrawToolbar(const ImVec2& viewMin);

    Tool m_Tool = Tool::Move;
    bool m_Local = false; // handles along the object's own axes, rather than the world's
    bool m_WantsMouse = false;
    bool m_Dragging = false;
};
