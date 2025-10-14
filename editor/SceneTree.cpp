// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/SceneTree.cpp

    \author Petra Hapalova

    \brief  Scene tree window for displaying and managing scene hierarchy
*/

#include "SceneTree.h"
#include "ImguiInstance.h"

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
        bool isRootSelected = (ptr->selected_scene_item == SCENE_ROOT_NODE);
        if (renderTreeNodeHeader(SCENE_ROOT_NODE, nullptr, isRootSelected))
        {
            if (ImGui::IsItemClicked())
            {
                ptr->selected_scene_item = SCENE_ROOT_NODE;
                ptr->selected_view_type = ViewsTypes::Root;
                ImGui::SetWindowFocus(PROPERTIES);
            }

            // Show viewport camera as child of Viewer
            if (!ptr->camera_views->empty())
            {
                auto viewportCamIt = ptr->camera_views->find(VIEWPORT_CAMERA);
                if (viewportCamIt != ptr->camera_views->end() && viewportCamIt->second)
                {
                    // Add partial indentation to distinguish from tree nodes
                    ImGui::Indent(indentSpacing);

                    const std::string& cameraName = VIEWPORT_CAMERA;
                    bool isSelected = (ptr->selected_scene_item == cameraName);

                    if (renderSceneItem(cameraName.c_str(), isSelected, indentSpacing, false))
                    {
                        ptr->selected_scene_item = cameraName;
                        ptr->selected_view_type = ViewsTypes::Cameras;
                        ImGui::SetWindowFocus(PROPERTIES);
                    }

                    ImGui::Unindent(indentSpacing);
                }
            }

            // Show other loaded camera views in tree
            if (ptr->camera_views->size() > 1)
            {
                bool allVisible = true;
                for (const auto& cameraPair : *ptr->camera_views)
                {
                    if (cameraPair.first == VIEWPORT_CAMERA || !cameraPair.second)
                        continue;
                    if (!cameraPair.second->is_visible)
                    {
                        allVisible = false;
                        break;
                    }
                }
                bool commonVisible = allVisible;

                bool treeNodeOpen = renderTreeNodeHeader("Camera Views", &commonVisible);

                // Update all camera views if visibility checkbox was toggled
                if (commonVisible != allVisible)
                {
                    for (auto& cameraPair : *ptr->camera_views)
                    {
                        if (cameraPair.first == VIEWPORT_CAMERA || !cameraPair.second)
                            continue;
                        cameraPair.second->is_visible = commonVisible ? PNANOVDB_TRUE : PNANOVDB_FALSE;
                    }
                }

                if (treeNodeOpen)
                {
                    for (auto& cameraPair : *ptr->camera_views)
                    {
                        pnanovdb_camera_view_t* camera = cameraPair.second;
                        if (!camera)
                        {
                            continue;
                        }
                        const std::string& cameraName = cameraPair.first;
                        if (cameraName == VIEWPORT_CAMERA)
                        {
                            // skip viewport camera - it's shown outside the tree
                            continue;
                        }

                        bool isSelected = (ptr->selected_scene_item == cameraName);
                        if (renderSceneItem(cameraName.c_str(), isSelected, indentSpacing, false, &camera->is_visible))
                        {
                            ptr->selected_scene_item = cameraName;
                            ptr->selected_view_type = ViewsTypes::Cameras;
                            ImGui::SetWindowFocus(PROPERTIES);
                        }
                    }
                    ImGui::TreePop();
                }
            }

            // Show other scene items
            auto renderSceneItems = [this, ptr, indentSpacing](const auto& itemMap, const char* treeLabel,
                                                               auto& pendingField, ViewsTypes viewType)
            {
                if (renderTreeNodeHeader(treeLabel))
                {
                    for (auto& pair : itemMap)
                    {
                        const std::string& name = pair.first;
                        bool isSelected = (ptr->selected_scene_item == name);
                        if (renderSceneItem(name.c_str(), isSelected, indentSpacing, false))
                        {
                            pendingField = name;
                            ptr->selected_view_type = viewType;
                            ImGui::SetWindowFocus(PROPERTIES);
                        }
                    }
                    ImGui::TreePop();
                }
            };

            if (ptr->nanovdb_arrays && !ptr->nanovdb_arrays->empty())
            {
                renderSceneItems(
                    *ptr->nanovdb_arrays, "NanoVDB Scenes", ptr->pending.viweport_nanovdb_array, ViewsTypes::NanoVDBs);
            }

            if (ptr->gaussian_views && !ptr->gaussian_views->empty())
            {
                renderSceneItems(*ptr->gaussian_views, "Gaussian Views", ptr->pending.viewport_gaussian_view,
                                 ViewsTypes::GaussianScenes);
            }

            ImGui::TreePop(); // Close Viewer tree node
        }

        // ImGui::PopStyleVar(); // Pop ItemSpacing
    }
    ImGui::End();
}

} // namespace pnanovdb_editor
