#include "UI/TransformGizmo.h"

#include "Scene/Entity.h"

#include <ImGuizmo.h>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui_internal.h> // BringWindowToDisplayBack

#include <cmath>

namespace
{
    // How far a drag moves between snaps while Ctrl is held.
    constexpr float kMoveSnap = 0.25f;   // units
    constexpr float kRotateSnap = 15.0f; // degrees
    constexpr float kScaleSnap = 0.1f;

    // Euler angles in degrees (Transform's convention, as glm::quat(vec3)
    // uses) for `orientation`. Every rotation has two such sets, each
    // repeating every 360 degrees; this picks the one closest to `previous`,
    // so the numbers in the Inspector change smoothly while dragging rather
    // than jumping to a different but equivalent set.
    glm::vec3 EulerNear(const glm::quat& orientation, const glm::vec3& previous)
    {
        const glm::vec3 a = glm::degrees(glm::eulerAngles(orientation));
        const glm::vec3 b(a.x + 180.0f, 180.0f - a.y, a.z + 180.0f); // the same rotation

        auto near = [&](glm::vec3 angles) {
            for (int i = 0; i < 3; ++i)
                angles[i] -= 360.0f * std::round((angles[i] - previous[i]) / 360.0f);
            return angles;
        };
        const glm::vec3 nearA = near(a);
        const glm::vec3 nearB = near(b);
        return glm::distance(nearA, previous) <= glm::distance(nearB, previous) ? nearA : nearB;
    }
}

void TransformGizmo::Draw(Entity* selected, const glm::mat4& view, const glm::mat4& projection,
                          const ImVec2& viewMin, const ImVec2& viewMax)
{
    m_WantsMouse = false;
    m_Dragging = false;

    // Shortcuts, unless typing into a field. (Not W/E/S like most editors:
    // those fly the camera.)
    if (!ImGui::GetIO().WantTextInput)
    {
        if (ImGui::IsKeyPressed(ImGuiKey_G, false))
            m_Tool = Tool::Move;
        else if (ImGui::IsKeyPressed(ImGuiKey_R, false))
            m_Tool = Tool::Rotate;
        else if (ImGui::IsKeyPressed(ImGuiKey_T, false))
            m_Tool = Tool::Scale;
    }

    ImGuizmo::BeginFrame();
    const ImVec2 size(viewMax.x - viewMin.x, viewMax.y - viewMin.y);
    if (size.x <= 0.0f || size.y <= 0.0f)
        return;
    DrawToolbar(viewMin);
    if (!selected || !selected->IsVisibleInHierarchy())
        return;

    // A see-through window exactly over the 3D view for ImGuizmo to draw
    // into, which also clips the handles to the view. Kept behind every
    // other window, so floating panels still cover it.
    ImGui::SetNextWindowPos(viewMin);
    ImGui::SetNextWindowSize(size);
    ImGui::Begin("##TransformGizmo", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav);
    ImGui::BringWindowToDisplayBack(ImGui::GetCurrentWindow());

    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist();
    // The scene is rendered across the whole window (the docked panels just
    // cover parts of it), so that is the area the projection maps onto.
    const ImGuiViewport* window = ImGui::GetMainViewport();
    ImGuizmo::SetRect(window->Pos.x, window->Pos.y, window->Size.x, window->Size.y);

    const ImGuizmo::OPERATION operation = m_Tool == Tool::Move     ? ImGuizmo::TRANSLATE
                                        : m_Tool == Tool::Rotate   ? ImGuizmo::ROTATE
                                                                   : ImGuizmo::SCALE;
    // Scaling is always along the object's own axes.
    const ImGuizmo::MODE mode = m_Local || m_Tool == Tool::Scale ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    const float snapStep = m_Tool == Tool::Move ? kMoveSnap : m_Tool == Tool::Rotate ? kRotateSnap : kScaleSnap;
    const float snap[3] = { snapStep, snapStep, snapStep };

    glm::mat4 world = selected->GetWorldMatrix();
    const bool changed = ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(projection), operation, mode,
                                              glm::value_ptr(world), nullptr,
                                              ImGui::GetIO().KeyCtrl ? snap : nullptr);
    m_Dragging = ImGuizmo::IsUsing();
    m_WantsMouse = ImGuizmo::IsOver() || m_Dragging;
    ImGui::End();

    if (!changed)
        return;

    // The handles work in the world; the transform is relative to the parent.
    const Entity* parent = selected->GetParent();
    const glm::mat4 local = parent ? glm::inverse(parent->GetWorldMatrix()) * world : world;
    Transform& transform = selected->transform;

    // Only what the tool changes is written back, so moving never disturbs
    // the rotation or scale numbers (re-deriving them could flip their signs
    // or angles to a different but equivalent set).
    switch (m_Tool)
    {
    case Tool::Move:
        transform.position = glm::vec3(local[3]);
        break;
    case Tool::Rotate:
    case Tool::Scale:
    {
        // Each axis column is the rotated axis times its scale. A mirrored
        // matrix (negative determinant) needs one negative scale: keep it on
        // the axis that already had one.
        glm::vec3 scale(glm::length(glm::vec3(local[0])), glm::length(glm::vec3(local[1])),
                        glm::length(glm::vec3(local[2])));
        if (glm::determinant(glm::mat3(local)) < 0.0f)
        {
            int axis = 0;
            for (int i = 0; i < 3; ++i)
                if (transform.scale[i] < 0.0f)
                    axis = i;
            scale[axis] = -scale[axis];
        }

        if (m_Tool == Tool::Scale)
        {
            transform.scale = scale;
        }
        else if (scale.x != 0.0f && scale.y != 0.0f && scale.z != 0.0f)
        {
            const glm::mat3 rotation(glm::vec3(local[0]) / scale.x, glm::vec3(local[1]) / scale.y,
                                     glm::vec3(local[2]) / scale.z);
            transform.rotation = EulerNear(glm::quat_cast(rotation), transform.rotation);
        }
        break;
    }
    }
}

void TransformGizmo::DrawToolbar(const ImVec2& viewMin)
{
    const float padding = ImGui::GetFontSize() * 0.5f;
    ImGui::SetNextWindowPos(ImVec2(viewMin.x + padding, viewMin.y + padding));
    ImGui::SetNextWindowBgAlpha(0.75f);
    ImGui::Begin("##TransformTools", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoMove);

    // A button per tool, highlighted when it is the current one.
    auto toolButton = [&](const char* label, Tool tool, const char* tooltip) {
        const bool active = m_Tool == tool;
        if (active)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(label))
            m_Tool = tool;
        if (active)
            ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s", tooltip);
        ImGui::SameLine();
    };
    toolButton("Move", Tool::Move, "Move (G): drag an arrow, or a square to move along two axes.\n"
                                   "Hold Ctrl to snap.");
    toolButton("Rotate", Tool::Rotate, "Rotate (R): drag a ring. Hold Ctrl to snap to 15 degrees.");
    toolButton("Scale", Tool::Scale, "Scale (T): drag a box, or the centre to scale evenly.\n"
                                     "Hold Ctrl to snap.");

    // World or local axes (scaling always uses the object's own).
    ImGui::BeginDisabled(m_Tool == Tool::Scale);
    if (ImGui::Button(m_Local || m_Tool == Tool::Scale ? "Local" : "Global"))
        m_Local = !m_Local;
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Global: handles along the world's axes.\n"
                          "Local: along the object's own (rotated) axes.");
    ImGui::End();
}
