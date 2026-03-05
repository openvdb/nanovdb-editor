// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/SceneTree.cpp

    \author Petra Hapalova

    \brief  Scene tree window for displaying and managing scene hierarchy
*/

#include "SceneTree.h"
#include "ImguiInstance.h"
#include "EditorScene.h"
#include "EditorSceneManager.h"
#include "EditorToken.h"
#include "Editor.h"
#include "Console.h"
#include "Pipeline.h"

#include <vector>
#include <cstdio>
#include <cstring>

namespace pnanovdb_editor
{
using namespace imgui_instance_user;

// Helper to draw an eye icon (visibility indicator)
// Returns true if clicked
static bool drawEyeIcon(ImDrawList* drawList, ImVec2 pos, float size, bool isVisible, const char* id)
{
    float cx = pos.x + size * 0.5f;
    float cy = pos.y + size * 0.5f;
    float r = size * 0.25f;
    ImU32 col = isVisible ? ImGui::GetColorU32(ImGuiCol_Text) : ImGui::GetColorU32(ImGuiCol_TextDisabled);

    // Draw eye outline (almond shape)
    drawList->AddEllipse(ImVec2(cx, cy), ImVec2(r * 1.5f, r), col, 0.f, 12, 1.5f);
    // Draw pupil (only if visible)
    if (isVisible)
    {
        drawList->AddCircleFilled(ImVec2(cx, cy), r * 0.5f, col);
    }

    // Invisible button for click handling
    bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
    return clicked;
}

// Helper to draw a side-facing eye icon (viewport camera indicator)
static void drawSideEyeIcon(ImDrawList* drawList, ImVec2 pos, float size)
{
    // Draw at 0.75 scale, centered in the row
    const float scale = 0.75f;
    float scaledSize = size * scale;
    float offsetY = (size - scaledSize) * 0.5f; // Center vertically
    ImVec2 drawPos = ImVec2(pos.x, pos.y + offsetY);

    float cy = drawPos.y + scaledSize * 0.5f;
    ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
    float thickness = 1.5f;

    // Eye ellipse
    float eyeCenterX = drawPos.x + scaledSize * 0.30f;
    float eyeRadiusX = scaledSize * 0.18f;
    float eyeRadiusY = scaledSize * 0.34f;
    drawList->AddEllipse(ImVec2(eyeCenterX, cy), ImVec2(eyeRadiusX, eyeRadiusY), col, 0.f, 16, thickness);

    // Small filled pupil
    float pupilRadius = scaledSize * 0.07f;
    drawList->AddCircleFilled(ImVec2(eyeCenterX - scaledSize * 0.08f, cy), pupilRadius, col);

    // ">" viewing direction
    float vTipX = drawPos.x + scaledSize;
    float vStartX = eyeCenterX - scaledSize * 0.12f;
    float vHalfHeight = scaledSize * 0.48f;
    drawList->AddLine(ImVec2(vStartX, cy - vHalfHeight), ImVec2(vTipX, cy), col, thickness);
    drawList->AddLine(ImVec2(vTipX, cy), ImVec2(vStartX, cy + vHalfHeight), col, thickness);
}

// Helper to draw a "+" icon
static void drawPlusIcon(ImDrawList* drawList, ImVec2 pos, float size, float thickness = 1.5f)
{
    float cx = pos.x + size * 0.5f;
    float cy = pos.y + size * 0.5f;
    float r = size * 0.25f;
    ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);

    // Horizontal line
    drawList->AddLine(ImVec2(cx - r, cy), ImVec2(cx + r, cy), col, thickness);
    // Vertical line
    drawList->AddLine(ImVec2(cx, cy - r), ImVec2(cx, cy + r), col, thickness);
}

static pnanovdb_editor_token_t* createUniqueSceneToken(EditorScene* editor_scene)
{
    if (!editor_scene)
    {
        return nullptr;
    }

    auto scene_tokens = editor_scene->get_all_scene_tokens();
    std::string scene_name;
    int suffix = 1;
    bool is_unique = false;

    while (!is_unique)
    {
        scene_name = "Scene " + std::to_string(suffix++);
        is_unique = true;
        for (pnanovdb_editor_token_t* token : scene_tokens)
        {
            if (token && token->str && scene_name == token->str)
            {
                is_unique = false;
                break;
            }
        }
    }

    return EditorToken::getInstance().getToken(scene_name.c_str());
}

// TODO: revisit
// Helper function to check if an item is selected in the current scene
static bool isSelectedInCurrentScene(const std::string& name, imgui_instance_user::Instance* ptr, ViewType expected_type)
{
    if (!ptr || !ptr->editor_scene)
    {
        return false;
    }

    SceneSelection sel = ptr->editor_scene->get_properties_selection();

    // Check if name matches
    const char* selected_name = (sel.name_token && sel.name_token->str) ? sel.name_token->str : "";

    // Name and type must match
    if (name != selected_name || sel.type != expected_type)
    {
        return false;
    }

    // Get current scene
    pnanovdb_editor_token_t* current_scene = ptr->editor_scene->get_current_scene_token();

    // If selection has no scene token, check if we're in default scene
    if (!sel.scene_token)
    {
        // Selection without scene token is valid in default scene or when no scene is set
        return !current_scene || strcmp(current_scene->str, pnanovdb_editor::DEFAULT_SCENE_NAME) == 0;
    }

    // If no current scene, selection can't be valid
    if (!current_scene)
    {
        return false;
    }

    // Scene IDs must match
    return sel.scene_token->id == current_scene->id;
}

bool SceneTree::renderSceneItem(const char* name,
                                bool isSelected,
                                float indentSpacing,
                                bool useIndent,
                                bool* deleteRequested,
                                bool* moveUpRequested,
                                bool* moveDownRequested,
                                const char* leftBadge,
                                bool badgeVisible,
                                bool* visibilityToggle,
                                uint64_t* dragPayloadId,
                                uint64_t* droppedSourceId)
{
    bool clicked = false;
    if (useIndent)
    {
        ImGui::Indent(indentSpacing);
    }

    ImGui::AlignTextToFramePadding();

    // Draw left-side item badge
    if (leftBadge && leftBadge[0] != '\0')
    {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() - 4.0f);
        ImU32 badgeColor = badgeVisible ? ImGui::GetColorU32(ImGuiCol_Text) : ImGui::GetColorU32(ImGuiCol_TextDisabled);
        ImGui::PushStyleColor(ImGuiCol_Text, badgeColor);
        ImGui::TextUnformatted(leftBadge);
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x + 6.0f);
    }

    // Calculate selectable width to account for right-side visibility icon
    float rightPadding = ImGui::GetStyle().ItemSpacing.x;
    if (visibilityToggle)
    {
        rightPadding += ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
    }

    float selectableWidth = ImGui::GetContentRegionAvail().x - rightPadding;
    if (ImGui::Selectable(name, isSelected, ImGuiSelectableFlags_AllowItemOverlap, ImVec2(selectableWidth, 0)))
    {
        clicked = true;
    }
    const ImVec2 itemRectMin = ImGui::GetItemRectMin();
    const ImVec2 itemRectMax = ImGui::GetItemRectMax();

    // Bind drag/drop to the selectable row item itself
    if (dragPayloadId && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
    {
        const uint64_t payload_id = *dragPayloadId;
        ImGui::SetDragDropPayload("SCENE_TREE_OBJECT_ID", &payload_id, sizeof(payload_id));
        ImGui::TextUnformatted(name);
        ImGui::EndDragDropSource();
    }
    if (droppedSourceId && ImGui::BeginDragDropTarget())
    {
        const ImGuiDragDropFlags dropFlags =
            ImGuiDragDropFlags_AcceptNoDrawDefaultRect | ImGuiDragDropFlags_AcceptBeforeDelivery;
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCENE_TREE_OBJECT_ID", dropFlags))
        {
            if (payload->Data && payload->DataSize == sizeof(uint64_t))
            {
                const uint64_t sourceId = *(const uint64_t*)payload->Data;
                const bool isSelfDrop = (dragPayloadId && sourceId == *dragPayloadId);

                // Render an insertion line where the item will be dropped
                if (payload->IsPreview() && !isSelfDrop)
                {
                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    const float lineY = itemRectMin.y;
                    const float linePadX = 2.0f;
                    drawList->AddLine(ImVec2(itemRectMin.x + linePadX, lineY), ImVec2(itemRectMax.x - linePadX, lineY),
                                      ImGui::GetColorU32(ImGuiCol_DragDropTarget), 2.0f);
                }

                if (payload->IsDelivery() && !isSelfDrop)
                {
                    *droppedSourceId = sourceId;
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    // Right-click context menu (only if deletion is allowed)
    if (deleteRequested && ImGui::BeginPopupContextItem())
    {
        if (moveUpRequested && ImGui::MenuItem("Move Up"))
        {
            *moveUpRequested = true;
        }
        if (moveDownRequested && ImGui::MenuItem("Move Down"))
        {
            *moveDownRequested = true;
        }
        if ((moveUpRequested || moveDownRequested))
        {
            ImGui::Separator();
        }
        if (ImGui::MenuItem("Remove"))
        {
            *deleteRequested = true;
        }
        ImGui::EndPopup();
    }

    // Add right-side eye icon for visibility
    if (visibilityToggle)
    {
        float iconSize = ImGui::GetFrameHeight();
        float iconXPos = ImGui::GetWindowContentRegionMax().x - iconSize;
        ImGui::SameLine(iconXPos);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 1.0f);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        bool isVisible = *visibilityToggle;
        if (drawEyeIcon(drawList, ImGui::GetCursorScreenPos(), iconSize, isVisible, "##EyeSceneItem"))
        {
            *visibilityToggle = !isVisible;
        }
    }

    if (useIndent)
    {
        ImGui::Unindent(indentSpacing);
    }

    return clicked;
}

bool SceneTree::renderTreeNodeHeader(const char* label, bool* visibilityCheckbox, bool isSelected, bool isRootNode)
{
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen;

    if (isRootNode)
    {
        if (isSelected)
        {
            flags |= ImGuiTreeNodeFlags_Selected;
        }
        flags |= ImGuiTreeNodeFlags_FramePadding;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 3.0f));
    }
    else
    {
        // no hover/selection highlighting when not root not selected
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
    }

    bool treeNodeOpen = ImGui::TreeNodeEx(label, flags);
    if (visibilityCheckbox)
    {
        // Position checkbox at fixed distance from right edge of window
        float checkboxXPos = ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight();
        ImGui::SameLine(checkboxXPos);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 1.0f);

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1.0f, 1.0f));
        ImGui::Checkbox((std::string("##VisibleAll") + label).c_str(), visibilityCheckbox);
        ImGui::PopStyleVar();
    }

    if (isRootNode)
    {
        ImGui::PopStyleVar();
    }
    else
    {
        ImGui::PopStyleColor(3);
    }

    return treeNodeOpen;
}

void SceneTree::render(imgui_instance_user::Instance* ptr)
{
    static pnanovdb_editor_token_t* s_scene_rename_target = nullptr;
    static char s_scene_rename_buffer[128] = "";
    static bool s_scene_rename_focus_input = false;
    static std::string s_scene_rename_error;
    auto clearSceneRenameState = [&]()
    {
        s_scene_rename_target = nullptr;
        s_scene_rename_error.clear();
        s_scene_rename_focus_input = false;
    };
    auto tryCommitSceneRename = [&]() -> bool
    {
        if (!ptr || !ptr->editor_scene || !s_scene_rename_target || !s_scene_rename_buffer[0])
        {
            s_scene_rename_error = "Name must not be empty.";
            return false;
        }

        pnanovdb_editor_token_t* renamed_scene = EditorToken::getInstance().getToken(s_scene_rename_buffer);
        bool name_conflict = false;
        for (pnanovdb_editor_token_t* token : ptr->editor_scene->get_all_scene_tokens())
        {
            if (token && token->id != s_scene_rename_target->id && token->id == renamed_scene->id)
            {
                name_conflict = true;
                break;
            }
        }

        if (name_conflict)
        {
            s_scene_rename_error = "A scene with this name already exists.";
            return false;
        }
        if (!ptr->editor_scene->rename_scene(s_scene_rename_target, renamed_scene))
        {
            s_scene_rename_error = "Failed to rename scene.";
            return false;
        }

        clearSceneRenameState();
        return true;
    };

    if (ImGui::Begin(SCENE, &ptr->window.show_scene))
    {
        // Scene Selector Combo Box
        if (ptr->editor_scene)
        {
            // Get all available scenes
            auto scene_tokens = ptr->editor_scene->get_all_scene_tokens();
            pnanovdb_editor_token_t* current_scene = ptr->editor_scene->get_current_scene_token();
            auto createAndSelectScene = [&]()
            {
                pnanovdb_editor_token_t* new_scene = createUniqueSceneToken(ptr->editor_scene);
                if (new_scene)
                {
                    ptr->editor_scene->set_current_scene(new_scene);
                    ptr->editor_scene->get_or_create_scene(new_scene);
                    current_scene = new_scene;
                    scene_tokens = ptr->editor_scene->get_all_scene_tokens();
                }
            };

            if (scene_tokens.empty())
            {
                current_scene = nullptr;
                ImGui::TextDisabled("No scenes available.");
                ImGui::Spacing();
                if (ImGui::Button("Create Scene", ImVec2(-1.0f, 0.0f)))
                {
                    createAndSelectScene();
                }
                ImGui::Spacing();
            }

            if (!current_scene && !scene_tokens.empty())
            {
                current_scene = scene_tokens.front();
                ptr->editor_scene->set_current_scene(current_scene);
            }

            if (!current_scene)
            {
                ImGui::TextDisabled("Create a scene to begin.");
                ImGui::End();
                return;
            }

            // Get current scene name for display
            const char* current_scene_name = current_scene ? current_scene->str : "";
            const bool is_renaming_current_scene =
                s_scene_rename_target && current_scene && s_scene_rename_target->id == current_scene->id;

            if (is_renaming_current_scene)
            {
                ImGui::PushItemWidth(-1.0f);
                if (s_scene_rename_focus_input)
                {
                    ImGui::SetKeyboardFocusHere();
                    s_scene_rename_focus_input = false;
                }
                bool commitRename =
                    ImGui::InputText("##RenameSceneInline", s_scene_rename_buffer, IM_ARRAYSIZE(s_scene_rename_buffer),
                                     ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                ImGui::PopItemWidth();
                if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape))
                {
                    clearSceneRenameState();
                }
                if (commitRename)
                {
                    tryCommitSceneRename();
                }
            }
            else
            {
                ImGui::PushItemWidth(-1.0f);
                if (ImGui::BeginCombo("##SceneSelector", current_scene_name))
                {
                    for (auto* scene_token : scene_tokens)
                    {
                        bool is_selected = (current_scene && current_scene->id == scene_token->id);
                        if (ImGui::Selectable(scene_token->str, is_selected))
                        {
                            // Switch to selected scene
                            ptr->editor_scene->set_current_scene(scene_token);
                        }

                        if (is_selected)
                        {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopItemWidth();
            }

            if (!s_scene_rename_error.empty())
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.2f, 0.2f, 1.0f));
                ImGui::TextUnformatted(s_scene_rename_error.c_str());
                ImGui::PopStyleColor();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
        }

        const float indentSpacing = ImGui::GetTreeNodeToLabelSpacing() * 0.35f;
        // ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 16.0f);
        // ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 4.0f));

        // Scene root node - use scene name instead of "Viewer"
        pnanovdb_editor_token_t* current_scene = ptr->editor_scene->get_current_scene_token();
        const char* scene_name =
            (current_scene && current_scene->str) ? current_scene->str : pnanovdb_editor::DEFAULT_SCENE_NAME;

        bool isRootSelected = isSelectedInCurrentScene(scene_name, ptr, ViewType::Root);
        bool rootNodeOpen = renderTreeNodeHeader(scene_name, nullptr, isRootSelected, true);

        bool removeSceneRequested = false;
        if (current_scene && ImGui::BeginPopupContextItem("##SceneRootContextMenu"))
        {
            if (ImGui::MenuItem("Rename Scene"))
            {
                s_scene_rename_target = current_scene;
                std::snprintf(s_scene_rename_buffer, sizeof(s_scene_rename_buffer), "%s",
                              current_scene->str ? current_scene->str : "");
                s_scene_rename_error.clear();
                s_scene_rename_focus_input = true;
            }

            size_t scene_count = ptr->editor_scene->get_all_scene_tokens().size();
            bool canRemoveScene = scene_count > 0;
            if (!canRemoveScene)
            {
                ImGui::BeginDisabled();
            }
            if (ImGui::MenuItem("Remove Scene"))
            {
                removeSceneRequested = true;
            }
            if (!canRemoveScene)
            {
                ImGui::EndDisabled();
            }

            ImGui::EndPopup();
        }

        // Right-side "+" icon on scene root row (add new scene)
        if (ptr->editor_scene)
        {
            float iconSize = ImGui::GetFrameHeight();
            float rightEdge = ImGui::GetWindowContentRegionMax().x;
            float plusXPos = rightEdge - iconSize;
            ImGui::SameLine(plusXPos);
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawPlusIcon(drawList, ImGui::GetCursorScreenPos(), iconSize, 2.0f);
            if (ImGui::InvisibleButton("##AddSceneRoot", ImVec2(iconSize, iconSize)))
            {
                pnanovdb_editor_token_t* new_scene = createUniqueSceneToken(ptr->editor_scene);
                if (new_scene)
                {
                    ptr->editor_scene->set_current_scene(new_scene);
                    ptr->editor_scene->get_or_create_scene(new_scene);
                }
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Add new scene");
            }
        }

        if (removeSceneRequested && current_scene)
        {
            const uint64_t removed_scene_id = current_scene->id;
            ptr->editor_scene->remove_scene(current_scene);
            current_scene = ptr->editor_scene->get_current_scene_token();
            if (s_scene_rename_target && s_scene_rename_target->id == removed_scene_id)
            {
                clearSceneRenameState();
            }
            if (!current_scene)
            {
                ImGui::TextDisabled("Create a scene to begin.");
                ImGui::End();
                return;
            }
        }

        if (rootNodeOpen)
        {
            // Ensure scene exists
            if (ptr->editor_scene)
            {
                ptr->editor_scene->get_or_create_scene(current_scene);
            }

            // Show camera views section (all cameras including viewport camera)
            if (ptr->editor_scene)
            {
                bool allVisible = true;
                bool hasNonViewportCameras = false;
                ptr->editor_scene->for_each_view(
                    ViewType::Cameras,
                    [&](uint64_t name_id, const auto& view_data)
                    {
                        using ViewT = std::decay_t<decltype(view_data)>;
                        if constexpr (std::is_same_v<ViewT, CameraViewContext>)
                        {
                            if (!view_data.camera_view)
                            {
                                return;
                            }
                            // Skip viewport camera - it should always be hidden
                            pnanovdb_editor_token_t* cam_token = EditorToken::getInstance().getTokenById(name_id);
                            if (cam_token && ptr->editor_scene->is_viewport_camera(cam_token))
                            {
                                return;
                            }
                            hasNonViewportCameras = true;
                            if (!view_data.camera_view->is_visible)
                            {
                                allVisible = false;
                            }
                        }
                    });
                // If no non-viewport cameras exist, show as "all hidden"
                bool commonVisible = hasNonViewportCameras ? allVisible : false;

                bool treeNodeOpen = renderTreeNodeHeader("Camera Views", nullptr, false, false);

                // Helper to add a new camera and select it
                auto addCameraAndSelect = [&ptr]()
                {
                    pnanovdb_editor_token_t* name_token = ptr->editor_scene->add_new_camera();
                    if (name_token)
                    {
                        ptr->editor_scene->set_properties_selection(ViewType::Cameras, name_token);
                    }
                };

                // Right-click context menu to add new camera
                if (ImGui::BeginPopupContextItem("##CameraViewsContextMenu"))
                {
                    if (ImGui::MenuItem("Add Camera"))
                    {
                        addCameraAndSelect();
                    }
                    ImGui::EndPopup();
                }

                // Icons on the right: "+" button, then eye icon
                float iconSize = ImGui::GetFrameHeight();
                float rightEdge = ImGui::GetWindowContentRegionMax().x;
                ImDrawList* drawList = ImGui::GetWindowDrawList();

                // Eye icon for toggling all cameras visibility (rightmost)
                float eyeXPos = rightEdge - iconSize;
                ImGui::SameLine(eyeXPos);
                if (drawEyeIcon(drawList, ImGui::GetCursorScreenPos(), iconSize, commonVisible, "##EyeAllCameras"))
                {
                    commonVisible = !commonVisible;
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(commonVisible ? "Hide all camera frustums" : "Show all camera frustums");
                }

                // "+" button to add new camera (left of eye)
                float plusXPos = eyeXPos - iconSize - ImGui::GetStyle().ItemSpacing.x;
                ImGui::SameLine(plusXPos);
                drawPlusIcon(drawList, ImGui::GetCursorScreenPos(), iconSize, 2.0f);
                if (ImGui::InvisibleButton("##AddCamera", ImVec2(iconSize, iconSize)))
                {
                    addCameraAndSelect();
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Add new camera");
                }

                // Update all camera views if visibility was toggled (skip viewport camera)
                if (commonVisible != allVisible && ptr->editor_scene)
                {
                    // Note: We need mutable access to update visibility
                    ptr->editor_scene->for_each_view(
                        ViewType::Cameras,
                        [&](uint64_t name_id, const auto& view_data)
                        {
                            using ViewT = std::decay_t<decltype(view_data)>;
                            if constexpr (std::is_same_v<ViewT, CameraViewContext>)
                            {
                                if (!view_data.camera_view)
                                {
                                    return;
                                }
                                // Skip viewport camera - it should always be hidden
                                pnanovdb_editor_token_t* cam_token = EditorToken::getInstance().getTokenById(name_id);
                                if (cam_token && ptr->editor_scene->is_viewport_camera(cam_token))
                                {
                                    return;
                                }
                                view_data.camera_view->is_visible = commonVisible ? PNANOVDB_TRUE : PNANOVDB_FALSE;
                            }
                        });
                }

                if (treeNodeOpen)
                {
                    // Collect cameras to delete (can't delete while iterating)
                    std::vector<std::string> camerasToDelete;

                    // Only allow deleting cameras when more than one exists
                    size_t cameraCount = ptr->editor_scene->get_camera_views().size();
                    bool canDeleteCameras = (cameraCount > 1);

                    ptr->editor_scene->for_each_view(
                        ViewType::Cameras,
                        [&](uint64_t name_id, const auto& view_data)
                        {
                            using ViewT = std::decay_t<decltype(view_data)>;
                            if constexpr (std::is_same_v<ViewT, CameraViewContext>)
                            {
                                pnanovdb_camera_view_t* camera = view_data.camera_view.get();
                                pnanovdb_editor_token_t* camera_token = EditorToken::getInstance().getTokenById(name_id);
                                if (!camera || !camera_token || !camera_token->str)
                                {
                                    return;
                                }

                                const char* cameraName = camera_token->str;
                                bool isSelected = isSelectedInCurrentScene(cameraName, ptr, ViewType::Cameras);
                                bool isViewportCamera = ptr->editor_scene->is_viewport_camera(camera_token);

                                // Render item without checkbox (we'll draw custom icons)
                                if (renderSceneItem(cameraName, isSelected, indentSpacing, false, nullptr, nullptr))
                                {
                                    ptr->editor_scene->set_properties_selection(ViewType::Cameras, camera_token);
                                }

                                // Custom context menu for cameras (only for non-viewport cameras)
                                // Viewport camera can't be removed and is already the viewport
                                if (!isViewportCamera &&
                                    ImGui::BeginPopupContextItem((std::string("##CameraMenu_") + cameraName).c_str()))
                                {
                                    if (ImGui::MenuItem("Set as Viewport Camera"))
                                    {
                                        // Update the previous viewport camera
                                        pnanovdb_editor_token_t* prevViewportToken =
                                            ptr->editor_scene->get_viewport_camera_token();
                                        if (prevViewportToken)
                                        {
                                            pnanovdb_camera_view_t* prevCamera =
                                                ptr->editor_scene->get_camera(prevViewportToken);
                                            if (prevCamera)
                                            {
                                                prevCamera->is_visible = PNANOVDB_FALSE;
                                                // Set far plane to 100 for frustum visualization
                                                int prevCameraIdx =
                                                    ptr->editor_scene->get_camera_frustum_index(prevViewportToken);
                                                if (prevCamera->configs && prevCameraIdx >= 0 &&
                                                    prevCameraIdx < prevCamera->num_cameras)
                                                {
                                                    prevCamera->configs[prevCameraIdx].far_plane = 100.0f;
                                                }
                                            }
                                        }

                                        // Hide the new viewport camera (shouldn't render its own frustum)
                                        camera->is_visible = PNANOVDB_FALSE;

                                        ptr->editor_scene->set_viewport_camera(camera_token);
                                        // Sync camera state to render settings
                                        int cameraIdx = ptr->editor_scene->get_camera_frustum_index(camera_token);
                                        // Set far plane to infinity for viewport camera
                                        if (camera->configs && cameraIdx >= 0 && cameraIdx < camera->num_cameras)
                                        {
                                            camera->configs[cameraIdx].far_plane = PNANOVDB_CAMERA_INFINITY;
                                        }
                                        ptr->render_settings->camera_state = camera->states[cameraIdx];
                                        ptr->render_settings->camera_config = camera->configs[cameraIdx];
                                        ptr->render_settings->sync_camera = PNANOVDB_TRUE;
                                    }
                                    if (canDeleteCameras && ImGui::MenuItem("Remove"))
                                    {
                                        camerasToDelete.push_back(cameraName);
                                    }
                                    ImGui::EndPopup();
                                }

                                // Draw icon on the right side
                                float iconSize = ImGui::GetFrameHeight();
                                float rightEdge = ImGui::GetWindowContentRegionMax().x;
                                float iconXPos = rightEdge - iconSize;
                                ImGui::SameLine(iconXPos);
                                ImDrawList* drawList = ImGui::GetWindowDrawList();

                                if (isViewportCamera)
                                {
                                    // Viewport camera icon
                                    drawSideEyeIcon(drawList, ImGui::GetCursorScreenPos(), iconSize);
                                    ImGui::Dummy(ImVec2(iconSize, iconSize));
                                    if (ImGui::IsItemHovered())
                                    {
                                        ImGui::SetTooltip("Viewport camera");
                                    }
                                }
                                else
                                {
                                    // Eye icon for visibility toggle (non-viewport cameras)
                                    bool isVisible = (camera->is_visible == PNANOVDB_TRUE);
                                    std::string eyeBtnId = std::string("##Eye_") + cameraName;
                                    if (drawEyeIcon(drawList, ImGui::GetCursorScreenPos(), iconSize, isVisible,
                                                    eyeBtnId.c_str()))
                                    {
                                        camera->is_visible = isVisible ? PNANOVDB_FALSE : PNANOVDB_TRUE;
                                    }
                                }
                            }
                        });

                    // Process camera deletions after iteration
                    if (!camerasToDelete.empty() && ptr->editor_scene)
                    {
                        pnanovdb_editor_t* editor = ptr->editor_scene->get_editor();
                        pnanovdb_editor_token_t* current_scene = ptr->editor_scene->get_current_scene_token();

                        // If no scene token, use default scene
                        if (!current_scene && editor)
                        {
                            current_scene = editor->get_token(DEFAULT_SCENE_NAME);
                        }

                        for (const auto& cameraName : camerasToDelete)
                        {
                            pnanovdb_editor_token_t* name_token = EditorToken::getInstance().getToken(cameraName.c_str());
                            if (editor && current_scene && name_token)
                            {
                                editor->remove(editor, current_scene, name_token);
                            }
                            else
                            {
                                Console::getInstance().addLog(
                                    "Error: Failed to delete camera '%s': editor=%p, scene=%p, token=%p",
                                    cameraName.c_str(), (void*)editor, (void*)current_scene, (void*)name_token);
                            }
                        }
                    }

                    ImGui::TreePop();
                }
            }

            // Show all renderable objects in a single list with right-side type badge
            auto renderSceneItems = [this, ptr, indentSpacing]()
            {
                // Collect items to delete (can't delete while iterating)
                std::vector<std::string> itemsToDelete;

                pnanovdb_editor_t* editor = ptr->editor_scene->get_editor();
                pnanovdb_editor_token_t* current_scene = ptr->editor_scene->get_current_scene_token();
                auto* scene_manager = ptr->editor_scene->get_scene_manager();

                // If no scene token, use default scene
                if (!current_scene && editor)
                {
                    current_scene = editor->get_token(DEFAULT_SCENE_NAME);
                }

                std::vector<pnanovdb_editor_token_t*> ordered_tokens =
                    ptr->editor_scene->get_ordered_renderable_views(current_scene);

                for (auto* token : ordered_tokens)
                {
                    if (!token || !token->str)
                    {
                        continue;
                    }
                    const char* name = token->str;
                    bool deleteRequested = false;
                    bool moveUpRequested = false;
                    bool moveDownRequested = false;
                    ViewType itemViewType = ViewType::None;
                    const char* badge = nullptr;
                    bool isVisible = true;

                    if (scene_manager)
                    {
                        scene_manager->with_object(
                            current_scene, token,
                            [&](pnanovdb_editor::SceneObject* obj)
                            {
                                if (obj)
                                {
                                    isVisible = obj->visible;
                                    auto rm = pnanovdb_editor::pipeline_get_render_method(obj->pipeline.render().type);
                                    if (rm == pnanovdb_pipeline_render_method_nanovdb)
                                    {
                                        itemViewType = ViewType::NanoVDBs;
                                        badge = "N";
                                    }
                                    else if (rm == pnanovdb_pipeline_render_method_raster2d)
                                    {
                                        itemViewType = ViewType::GaussianScenes;
                                        badge = "G";
                                    }
                                }
                            });
                    }
                    if (itemViewType == ViewType::None)
                    {
                        continue;
                    }
                    bool isSelected = isSelectedInCurrentScene(name, ptr, itemViewType);
                    uint64_t droppedSourceId = 0;

                    ImGui::PushID((int)token->id);
                    if (renderSceneItem(name, isSelected, indentSpacing, false, &deleteRequested, &moveUpRequested,
                                        &moveDownRequested, badge, isVisible, &isVisible, &token->id, &droppedSourceId))
                    {
                        if (editor && current_scene)
                        {
                            // Set pending viewport so handle_pending_view_changes processes the selection
                            if (itemViewType == ViewType::NanoVDBs)
                                ptr->pending.viewport_nanovdb_array = name;
                            else if (itemViewType == ViewType::GaussianScenes)
                                ptr->pending.viewport_gaussian_view = name;
                            pnanovdb_editor::select_render_view(editor, current_scene, token);
                        }
                    }
                    if (droppedSourceId != 0 && droppedSourceId != token->id)
                    {
                        pnanovdb_editor_token_t* source_token = EditorToken::getInstance().getTokenById(droppedSourceId);
                        if (source_token)
                        {
                            ptr->editor_scene->move_renderable_before(current_scene, source_token, token);
                        }
                    }
                    if (scene_manager)
                    {
                        scene_manager->with_object(current_scene, token,
                                                   [&](pnanovdb_editor::SceneObject* obj)
                                                   {
                                                       if (obj)
                                                       {
                                                           obj->visible = isVisible;
                                                       }
                                                   });
                    }

                    if (moveUpRequested)
                    {
                        ptr->editor_scene->move_renderable_order(current_scene, token, -1);
                    }
                    if (moveDownRequested)
                    {
                        ptr->editor_scene->move_renderable_order(current_scene, token, +1);
                    }
                    if (deleteRequested)
                    {
                        itemsToDelete.push_back(name);
                    }
                    ImGui::PopID();
                }

                // Process deletions after iteration
                if (!itemsToDelete.empty() && ptr->editor_scene)
                {
                    for (const auto& itemName : itemsToDelete)
                    {
                        pnanovdb_editor_token_t* name_token = EditorToken::getInstance().getToken(itemName.c_str());
                        if (editor && current_scene && name_token)
                        {
                            editor->remove(editor, current_scene, name_token);
                        }
                        else
                        {
                            Console::getInstance().addLog("Error: Failed to delete '%s': editor=%p, scene=%p, token=%p",
                                                          itemName.c_str(), (void*)editor, (void*)current_scene,
                                                          (void*)name_token);
                        }
                    }
                }
            };

            if (ptr->editor_scene)
            {
                std::vector<pnanovdb_editor_token_t*> ordered_renderables =
                    ptr->editor_scene->get_ordered_renderable_views(current_scene);
                if (!ordered_renderables.empty())
                {
                    renderSceneItems();
                }
            }

            ImGui::TreePop(); // Close scene root tree node
        }

        // ImGui::PopStyleVar(); // Pop ItemSpacing
    }
    ImGui::End();
}

} // namespace pnanovdb_editor
