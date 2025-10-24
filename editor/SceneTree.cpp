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

namespace pnanovdb_editor
{
using namespace imgui_instance_user;

bool SceneTree::renderSceneItem(
    const char* name, bool isSelected, float indentSpacing, bool useIndent, pnanovdb_bool_t* visibilityCheckbox)
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

    // Calculate selectable width to stop before checkbox column for visual consistency
    float selectableWidth =
        ImGui::GetContentRegionAvail().x - (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::Selectable(name, isSelected, ImGuiSelectableFlags_AllowItemOverlap, ImVec2(selectableWidth, 0)))
    {
        clicked = true;
    }

    // Draw bullet on top of selectable background
    ImVec2 itemMin = ImGui::GetItemRectMin();
    float bulletY = itemMin.y + ImGui::GetTextLineHeight() * 0.5f + 2.0f;
    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(bulletPosScreen.x + 4.0f, bulletY), 2.0f, ImGui::GetColorU32(ImGuiCol_Text));

    if (visibilityCheckbox)
    {
        ImGui::SameLine();
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

bool SceneTree::renderTreeNodeHeader(const char* label, bool* visibilityCheckbox, bool isSelected)
{
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen;

    if (strcmp(label, SCENE_ROOT_NODE) == 0)
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
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() + ImGui::GetCursorPosX());
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 1.0f);

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1.0f, 1.0f));
        ImGui::Checkbox((std::string("##VisibleAll") + label).c_str(), visibilityCheckbox);
        ImGui::PopStyleVar();
    }

    if (strcmp(label, SCENE_ROOT_NODE) == 0)
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
        const float indentSpacing = ImGui::GetTreeNodeToLabelSpacing() * 0.35f;
        // ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 16.0f);
        // ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 4.0f));

        // Viewer tree node - root parent of all scene items
        bool isRootSelected = (ptr->editor_scene->get_properties_selection().name == SCENE_ROOT_NODE);
        const std::string scene_name = ptr->editor_scene->get_name();
        if (renderTreeNodeHeader(scene_name.c_str(), nullptr, isRootSelected))
        {
            // Show viewport camera as child of Viewer
            if (ptr->editor_scene)
            {
                bool hasViewportCamera = false;
                ptr->editor_scene->for_each_view(
                    ViewType::Cameras,
                    [&](const std::string& name, const auto& view_data)
                    {
                        using ViewT = std::decay_t<decltype(view_data)>;
                        if constexpr (std::is_same_v<ViewT, pnanovdb_camera_view_t*>)
                        {
                            if (name == VIEWPORT_CAMERA && view_data)
                            {
                                hasViewportCamera = true;
                                // Add partial indentation to distinguish from tree nodes
                                ImGui::Indent(indentSpacing);

                                const std::string& cameraName = VIEWPORT_CAMERA;
                                bool isSelected = (ptr->editor_scene->get_properties_selection().name == cameraName);

                                if (renderSceneItem(cameraName.c_str(), isSelected, indentSpacing, false))
                                {
                                    ptr->editor_scene->set_properties_selection(ViewType::Cameras, cameraName);
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
                ptr->editor_scene->for_each_view(ViewType::Cameras,
                                                 [&](const std::string& name, const auto& view_data)
                                                 {
                                                     using ViewT = std::decay_t<decltype(view_data)>;
                                                     if constexpr (std::is_same_v<ViewT, pnanovdb_camera_view_t*>)
                                                     {
                                                         if (name == VIEWPORT_CAMERA || !view_data)
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

                bool treeNodeOpen = renderTreeNodeHeader("Camera Views", &commonVisible);

                // Update all camera views if visibility checkbox was toggled
                if (commonVisible != allVisible && ptr->editor_scene)
                {
                    // Note: We need mutable access to update visibility
                    ptr->editor_scene->for_each_view(ViewType::Cameras,
                                                     [&](const std::string& name, const auto& view_data)
                                                     {
                                                         using ViewT = std::decay_t<decltype(view_data)>;
                                                         if constexpr (std::is_same_v<ViewT, pnanovdb_camera_view_t*>)
                                                         {
                                                             if (name == VIEWPORT_CAMERA || !view_data)
                                                             {
                                                                 return;
                                                             }
                                                             view_data->is_visible =
                                                                 commonVisible ? PNANOVDB_TRUE : PNANOVDB_FALSE;
                                                         }
                                                     });
                }

                if (treeNodeOpen)
                {
                    ptr->editor_scene->for_each_view(
                        ViewType::Cameras,
                        [&](const std::string& cameraName, const auto& view_data)
                        {
                            using ViewT = std::decay_t<decltype(view_data)>;
                            if constexpr (std::is_same_v<ViewT, pnanovdb_camera_view_t*>)
                            {
                                pnanovdb_camera_view_t* camera = view_data;
                                if (!camera || cameraName == VIEWPORT_CAMERA)
                                {
                                    return;
                                }

                                bool isSelected = (ptr->editor_scene->get_properties_selection().name == cameraName);
                                if (renderSceneItem(
                                        cameraName.c_str(), isSelected, indentSpacing, false, &camera->is_visible))
                                {
                                    ptr->editor_scene->set_properties_selection(ViewType::Cameras, cameraName);
                                }
                            }
                        });
                    ImGui::TreePop();
                }
            }

            // Show other scene items
            auto renderSceneItems = [this, ptr, indentSpacing](const auto& /*itemMap*/, const char* treeLabel,
                                                               auto& pendingField, ViewType viewType)
            {
                if (renderTreeNodeHeader(treeLabel))
                {
                    ptr->editor_scene->for_each_view(
                        viewType,
                        [&](const std::string& name, const auto& /*view_data*/)
                        {
                            bool isSelected = (ptr->editor_scene->get_properties_selection().name == name);
                            if (renderSceneItem(name.c_str(), isSelected, indentSpacing, false))
                            {
                                pendingField = name;
                            }
                        });
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

            ImGui::TreePop(); // Close Viewer tree node
        }

        // ImGui::PopStyleVar(); // Pop ItemSpacing

        if (ImGui::Button("Clear"))
        {
            ptr->editor_scene->remove_all();
        }
    }
    ImGui::End();
}

} // namespace pnanovdb_editor
