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
#include "EditorToken.h"
#include "Console.h"

#include <vector>

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
                                pnanovdb_bool_t* visibilityCheckbox,
                                bool* deleteRequested)
{
    bool clicked = false;
    if (useIndent)
    {
        ImGui::Indent(indentSpacing);
    }

    ImGui::AlignTextToFramePadding();

    // Save position for bullet and reserve space without drawing
    ImVec2 bulletPosScreen = ImGui::GetCursorScreenPos();
    float bulletWidth = ImGui::GetTextLineHeight();
    ImGui::Dummy(ImVec2(bulletWidth, 0));
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x + 8.f);

    // Calculate selectable width to account for buttons on the right
    float rightPadding = ImGui::GetStyle().ItemSpacing.x;
    if (visibilityCheckbox)
    {
        rightPadding += ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
    }

    float selectableWidth = ImGui::GetContentRegionAvail().x - rightPadding;
    if (ImGui::Selectable(name, isSelected, ImGuiSelectableFlags_AllowItemOverlap, ImVec2(selectableWidth, 0)))
    {
        clicked = true;
    }

    // Right-click context menu (only if deletion is allowed)
    if (deleteRequested && ImGui::BeginPopupContextItem())
    {
        if (ImGui::MenuItem("Remove"))
        {
            *deleteRequested = true;
        }
        ImGui::EndPopup();
    }

    // Draw bullet on top of selectable background
    ImVec2 itemMin = ImGui::GetItemRectMin();
    float bulletY = itemMin.y + ImGui::GetTextLineHeight() * 0.5f + 2.0f;
    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(bulletPosScreen.x + 4.0f, bulletY), 2.0f, ImGui::GetColorU32(ImGuiCol_Text));

    // Add visibility checkbox
    if (visibilityCheckbox)
    {
        // Position checkbox at fixed distance from right edge of window (same as tree node headers)
        float checkboxXPos = ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight();
        ImGui::SameLine(checkboxXPos);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.0f);

        // Make checkbox smaller with reduced frame padding
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1.0f, 1.0f));
        IMGUI_CHECKBOX_SYNC((std::string("##Visible") + name).c_str(), *visibilityCheckbox);
        ImGui::PopStyleVar();
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
    if (ImGui::Begin(SCENE, &ptr->window.show_scene))
    {
        // Scene Selector Combo Box
        if (ptr->editor_scene)
        {
            // Get all available scenes
            auto scene_tokens = ptr->editor_scene->get_all_scene_tokens();
            pnanovdb_editor_token_t* current_scene = ptr->editor_scene->get_current_scene_token();

            // Get current scene name for display
            const char* current_scene_name = current_scene ? current_scene->str : "";

            ImGui::PushItemWidth(-1.0f); // Full width
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
        if (renderTreeNodeHeader(scene_name, nullptr, isRootSelected, true))
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

            // Show other scene items
            auto renderSceneItems = [this, ptr, indentSpacing](const auto& /*itemMap*/, const char* treeLabel,
                                                               auto& pendingField, ViewType viewType)
            {
                if (renderTreeNodeHeader(treeLabel))
                {
                    // Collect items to delete (can't delete while iterating)
                    std::vector<std::string> itemsToDelete;

                    ptr->editor_scene->for_each_view(
                        viewType,
                        [&](uint64_t name_id, const auto& /*view_data*/)
                        {
                            pnanovdb_editor_token_t* token = EditorToken::getInstance().getTokenById(name_id);
                            if (!token || !token->str)
                            {
                                return;
                            }
                            const char* name = token->str;
                            bool isSelected = isSelectedInCurrentScene(name, ptr, viewType);
                            bool deleteRequested = false;

                            if (renderSceneItem(name, isSelected, indentSpacing, false, nullptr, &deleteRequested))
                            {
                                pendingField = name;
                            }

                            if (deleteRequested)
                            {
                                itemsToDelete.push_back(name);
                            }
                        });

                    // Process deletions after iteration
                    if (!itemsToDelete.empty() && ptr->editor_scene)
                    {
                        pnanovdb_editor_t* editor = ptr->editor_scene->get_editor();
                        pnanovdb_editor_token_t* current_scene = ptr->editor_scene->get_current_scene_token();

                        // If no scene token, use default scene
                        if (!current_scene && editor)
                        {
                            current_scene = editor->get_token(DEFAULT_SCENE_NAME);
                        }

                        for (const auto& itemName : itemsToDelete)
                        {
                            pnanovdb_editor_token_t* name_token = EditorToken::getInstance().getToken(itemName.c_str());
                            if (editor && current_scene && name_token)
                            {
                                editor->remove(editor, current_scene, name_token);
                            }
                            else
                            {
                                Console::getInstance().addLog(
                                    "Error: Failed to delete '%s': editor=%p, scene=%p, token=%p", itemName.c_str(),
                                    (void*)editor, (void*)current_scene, (void*)name_token);
                            }
                        }
                    }

                    ImGui::TreePop();
                }
            };

            if (ptr->editor_scene)
            {
                const auto& nanovdb_views = ptr->editor_scene->get_nanovdb_views();
                if (!nanovdb_views.empty())
                {
                    renderSceneItems(
                        nanovdb_views, "NanoVDB Views", ptr->pending.viewport_nanovdb_array, ViewType::NanoVDBs);
                }

                const auto& gaussian_views = ptr->editor_scene->get_gaussian_views();
                if (!gaussian_views.empty())
                {
                    renderSceneItems(gaussian_views, "Gaussian Views", ptr->pending.viewport_gaussian_view,
                                     ViewType::GaussianScenes);
                }
            }

            ImGui::TreePop(); // Close scene root tree node
        }

        // ImGui::PopStyleVar(); // Pop ItemSpacing
    }
    ImGui::End();
}

} // namespace pnanovdb_editor
