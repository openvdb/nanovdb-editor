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
        const char* scene_name = (current_scene && current_scene->str) ? current_scene->str : pnanovdb_editor::DEFAULT_SCENE_NAME;

        bool isRootSelected = isSelectedInCurrentScene(SCENE_ROOT_NODE, ptr, ViewType::Root);
        if (renderTreeNodeHeader(scene_name, nullptr, isRootSelected, true))
        {
            // Show viewport camera as child of scene root
            if (ptr->editor_scene)
            {
                bool hasViewportCamera = false;
                pnanovdb_editor_token_t* viewport_token = ptr->editor_scene->get_viewport_camera_token();
                ptr->editor_scene->for_each_view(
                    ViewType::Cameras,
                    [&](uint64_t name_id, const auto& view_data)
                    {
                        using ViewT = std::decay_t<decltype(view_data)>;
                        if constexpr (std::is_same_v<ViewT, pnanovdb_camera_view_t*>)
                        {
                            if (viewport_token && name_id == viewport_token->id && view_data)
                            {
                                hasViewportCamera = true;
                                // Add partial indentation to distinguish from tree nodes
                                ImGui::Indent(indentSpacing);

                                const std::string& cameraName = VIEWPORT_CAMERA;
                                bool isSelected = isSelectedInCurrentScene(cameraName, ptr, ViewType::Cameras);

                                if (renderSceneItem(cameraName.c_str(), isSelected, indentSpacing, false))
                                {
                                    ptr->editor_scene->set_properties_selection(ViewType::Cameras, viewport_token);
                                }

                                ImGui::Unindent(indentSpacing);
                            }
                        }
                    });
                (void)hasViewportCamera;
            }

            // Show other loaded camera views in tree
            if (ptr->editor_scene && ptr->editor_scene->get_camera_views().size() > 1)
            {
                bool allVisible = true;
                pnanovdb_editor_token_t* viewport_token = ptr->editor_scene->get_viewport_camera_token();
                ptr->editor_scene->for_each_view(
                    ViewType::Cameras,
                    [&](uint64_t name_id, const auto& view_data)
                    {
                        using ViewT = std::decay_t<decltype(view_data)>;
                        if constexpr (std::is_same_v<ViewT, pnanovdb_camera_view_t*>)
                        {
                            if ((viewport_token && name_id == viewport_token->id) || !view_data)
                            {
                                return;
                            }
                            if (!view_data->is_visible)
                            {
                                allVisible = false;
                            }
                        }
                    });
                bool commonVisible = allVisible;

                bool treeNodeOpen = renderTreeNodeHeader("Camera Views", &commonVisible, false, false);

                // Update all camera views if visibility checkbox was toggled
                if (commonVisible != allVisible && ptr->editor_scene)
                {
                    // Note: We need mutable access to update visibility
                    ptr->editor_scene->for_each_view(
                        ViewType::Cameras,
                        [&](uint64_t name_id, const auto& view_data)
                        {
                            using ViewT = std::decay_t<decltype(view_data)>;
                            if constexpr (std::is_same_v<ViewT, pnanovdb_camera_view_t*>)
                            {
                                if ((viewport_token && name_id == viewport_token->id) || !view_data)
                                {
                                    return;
                                }
                                view_data->is_visible = commonVisible ? PNANOVDB_TRUE : PNANOVDB_FALSE;
                            }
                        });
                }

                if (treeNodeOpen)
                {
                    // Collect cameras to delete (can't delete while iterating)
                    std::vector<std::string> camerasToDelete;
                    pnanovdb_editor_token_t* viewport_token = ptr->editor_scene->get_viewport_camera_token();

                    ptr->editor_scene->for_each_view(
                        ViewType::Cameras,
                        [&](uint64_t name_id, const auto& view_data)
                        {
                            using ViewT = std::decay_t<decltype(view_data)>;
                            if constexpr (std::is_same_v<ViewT, pnanovdb_camera_view_t*>)
                            {
                                pnanovdb_camera_view_t* camera = view_data;
                                pnanovdb_editor_token_t* camera_token = EditorToken::getInstance().getTokenById(name_id);
                                if (!camera || !camera_token || !camera_token->str ||
                                    (viewport_token && name_id == viewport_token->id))
                                {
                                    return;
                                }

                                const char* cameraName = camera_token->str;
                                bool isSelected = isSelectedInCurrentScene(cameraName, ptr, ViewType::Cameras);
                                bool deleteRequested = false;

                                if (renderSceneItem(cameraName, isSelected, indentSpacing, false, &camera->is_visible,
                                                    &deleteRequested))
                                {
                                    ptr->editor_scene->set_properties_selection(ViewType::Cameras, camera_token);
                                }

                                if (deleteRequested)
                                {
                                    camerasToDelete.push_back(cameraName);
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
