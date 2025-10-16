// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/FileHeaderInfo.cpp

    \author Petra Hapalova

    \brief  NanoVDB grid/file header information window
*/

#include "FileHeaderInfo.h"

namespace pnanovdb_editor
{

static const char* getGridTypeName(uint32_t gridType)
{
    switch (gridType)
    {
    case 0:
        return "UNKNOWN";
    case 1:
        return "FLOAT";
    case 2:
        return "DOUBLE";
    case 3:
        return "INT16";
    case 4:
        return "INT32";
    case 5:
        return "INT64";
    case 6:
        return "VEC3F";
    case 7:
        return "VEC3D";
    case 8:
        return "MASK";
    case 9:
        return "HALF";
    case 10:
        return "UINT32";
    case 11:
        return "BOOLEAN";
    case 12:
        return "RGBA8";
    case 13:
        return "FP4";
    case 14:
        return "FP8";
    case 15:
        return "FP16";
    case 16:
        return "FPN";
    case 17:
        return "VEC4F";
    case 18:
        return "VEC4D";
    case 19:
        return "INDEX";
    case 20:
        return "ONINDEX";
    case 21:
        return "INDEXMASK";
    case 22:
        return "ONINDEXMASK";
    case 23:
        return "POINTINDEX";
    case 24:
        return "VEC3U8";
    case 25:
        return "VEC3U16";
    case 26:
        return "UINT8";
    case 27:
        return "NODE2";
    default:
        return "INVALID";
    }
}

static const char* getGridClassName(uint32_t gridClass)
{
    switch (gridClass)
    {
    case 0:
        return "UNKNOWN";
    case 1:
        return "LEVEL_SET";
    case 2:
        return "FOG_VOLUME";
    case 3:
        return "STAGGERED";
    case 4:
        return "POINT_INDEX";
    case 5:
        return "POINT_DATA";
    case 6:
        return "TOPOLOGY";
    case 7:
        return "VOXEL_VOLUME";
    case 8:
        return "INDEX_GRID";
    case 9:
        return "TENSOR_GRID";
    default:
        return "INVALID";
    }
}

static std::string getGridNameString(uint32_t name0, uint32_t name1)
{
    char nameStr[9] = { 0 };
    nameStr[0] = (char)(name0 & 0xFF);
    nameStr[1] = (char)((name0 >> 8) & 0xFF);
    nameStr[2] = (char)((name0 >> 16) & 0xFF);
    nameStr[3] = (char)((name0 >> 24) & 0xFF);
    nameStr[4] = (char)(name1 & 0xFF);
    nameStr[5] = (char)((name1 >> 8) & 0xFF);
    nameStr[6] = (char)((name1 >> 16) & 0xFF);
    nameStr[7] = (char)((name1 >> 24) & 0xFF);
    for (int i = 0; i < 8; i++)
    {
        if (nameStr[i] == '\0')
        {
            break;
        }
        if (nameStr[i] < 32 || nameStr[i] > 126)
        {
            nameStr[i] = '?';
        }
    }
    return std::string(nameStr);
}

static std::string getFullGridName(pnanovdb_buf_t buf, pnanovdb_grid_handle_t grid, uint32_t flags)
{
    if (flags & (1 << 0))
    {
        uint32_t blindMetadataCount = pnanovdb_grid_get_blind_metadata_count(buf, grid);
        int64_t blindMetadataOffset = pnanovdb_grid_get_blind_metadata_offset(buf, grid);
        if (blindMetadataCount > 0 && blindMetadataOffset != 0)
        {
            for (uint32_t i = 0; i < blindMetadataCount; ++i)
            {
                pnanovdb_gridblindmetadata_handle_t metadata;
                metadata.address = pnanovdb_address_offset(grid.address, blindMetadataOffset + i * 288);
                uint32_t dataClass = pnanovdb_gridblindmetadata_get_data_class(buf, metadata);
                if (dataClass == 3)
                {
                    int64_t dataOffset = pnanovdb_gridblindmetadata_get_data_offset(buf, metadata);
                    pnanovdb_address_t nameAddress = pnanovdb_address_offset(metadata.address, dataOffset);
                    uint64_t valueCount = pnanovdb_gridblindmetadata_get_value_count(buf, metadata);
                    std::string longName;
                    longName.reserve(valueCount);
                    for (uint64_t j = 0; j < valueCount && j < 1024; ++j)
                    {
                        uint8_t c = pnanovdb_read_uint8(buf, pnanovdb_address_offset(nameAddress, j));
                        if (c == '\0')
                            break;
                        if (c >= 32 && c <= 126)
                            longName += (char)c;
                        else
                            longName += '?';
                    }
                    return longName;
                }
            }
        }
        return "[Long name not found in blind data]";
    }
    else
    {
        uint32_t name0 = pnanovdb_grid_get_grid_name(buf, grid, 0);
        uint32_t name1 = pnanovdb_grid_get_grid_name(buf, grid, 1);
        return getGridNameString(name0, name1);
    }
}

static const char* getMagicTypeName(uint64_t magic)
{
    if (magic == 0x304244566f6e614eULL)
        return "NanoVDB0";
    if (magic == 0x314244566f6e614eULL)
        return "NanoVDB1 (Grid)";
    if (magic == 0x324244566f6e614eULL)
        return "NanoVDB2 (File)";
    if ((magic & 0xFFFFFFFF) == 0x56444220UL)
        return "OpenVDB";
    return "Unknown";
}

static std::string getVersionString(uint32_t version)
{
    uint32_t major = (version >> 16) & 0xFFFF;
    uint32_t minor = (version >> 8) & 0xFF;
    uint32_t patch = version & 0xFF;
    char versionStr[32];
    snprintf(versionStr, sizeof(versionStr), "%u.%u.%u", major, minor, patch);
    return std::string(versionStr);
}

bool FileHeaderInfo::render(pnanovdb_compute_array_t* array)
{
    if (!(array && array->data && array->element_count > 0))
    {
        ImGui::Text("No NanoVDB file loaded");
        ImGui::Text("Please load a NanoVDB file to view debug information");
        return true;
    }

    pnanovdb_buf_t buf = pnanovdb_make_buf((pnanovdb_uint32_t*)array->data, array->element_count);
    pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };

    ImGui::Text("NanoVDB Grid Information");
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Grid Header", ImGuiTreeNodeFlags_DefaultOpen))
    {
        uint64_t magic = pnanovdb_grid_get_magic(buf, grid);
        ImGui::Text("Magic: %s", getMagicTypeName(magic));

        uint32_t version = pnanovdb_grid_get_version(buf, grid);
        std::string versionStr = getVersionString(version);
        ImGui::Text("Version: %s", versionStr.c_str());

        uint32_t flags = pnanovdb_grid_get_flags(buf, grid);
        if (ImGui::CollapsingHeader("Grid Flags"))
        {
            ImGui::Indent();
            struct FlagInfo
            {
                uint32_t bit;
                const char* name;
                const char* description;
            };
            FlagInfo flagInfos[] = { { 1u << 0, "Long Grid Name", "Grid name > 256 characters" },
                                     { 1u << 1, "Bounding Box", "Nodes contain bounding boxes" },
                                     { 1u << 2, "Min/Max Values", "Nodes contain min/max statistics" },
                                     { 1u << 3, "Average Values", "Nodes contain average statistics" },
                                     { 1u << 4, "Std Deviation", "Nodes contain standard deviation" },
                                     { 1u << 5, "Breadth First", "Memory layout is breadth-first" } };
            bool hasAnyFlags = false;
            for (const auto& flagInfo : flagInfos)
            {
                bool isSet = (flags & flagInfo.bit) != 0;
                if (isSet)
                    hasAnyFlags = true;
                ImU32 textColor = isSet ? IM_COL32(144, 238, 144, 255) : IM_COL32(128, 128, 128, 255);
                ImGui::PushStyleColor(ImGuiCol_Text, textColor);
                ImGui::Text("[%s] %s", isSet ? "X" : " ", flagInfo.name);
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("%s", flagInfo.description);
                }
                ImGui::PopStyleColor();
            }
            if (!hasAnyFlags)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(128, 128, 128, 255));
                ImGui::Text("[ ] No flags set");
                ImGui::PopStyleColor();
            }
            ImGui::Unindent();
        }
        ImGui::Text("Grid Index: %u", pnanovdb_grid_get_grid_index(buf, grid));
        ImGui::Text("Grid Count: %u", pnanovdb_grid_get_grid_count(buf, grid));
        ImGui::Text("Grid Size: %llu bytes", (unsigned long long)pnanovdb_grid_get_grid_size(buf, grid));
        uint32_t gridType = pnanovdb_grid_get_grid_type(buf, grid);
        ImGui::Text("Grid Type: %s", getGridTypeName(gridType));
        uint32_t gridClass = pnanovdb_grid_get_grid_class(buf, grid);
        ImGui::Text("Grid Class: %s", getGridClassName(gridClass));

        std::string gridNameStr = getFullGridName(buf, grid, flags);
        ImGui::Text("Grid Name: \"%s\"", gridNameStr.c_str());
        if (flags & (1u << 0))
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.5f, 0.7f, 1.0f, 1.0f), "(Long Name)");
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Grid name longer than 256 characters, stored in blind metadata");
            }
        }

        ImGui::Text("World BBox Min: (%.3f, %.3f, %.3f)", pnanovdb_grid_get_world_bbox(buf, grid, 0),
                    pnanovdb_grid_get_world_bbox(buf, grid, 1), pnanovdb_grid_get_world_bbox(buf, grid, 2));
        ImGui::Text("World BBox Max: (%.3f, %.3f, %.3f)", pnanovdb_grid_get_world_bbox(buf, grid, 3),
                    pnanovdb_grid_get_world_bbox(buf, grid, 4), pnanovdb_grid_get_world_bbox(buf, grid, 5));

        ImGui::Text("Voxel Size: (%.6f, %.6f, %.6f)", pnanovdb_grid_get_voxel_size(buf, grid, 0),
                    pnanovdb_grid_get_voxel_size(buf, grid, 1), pnanovdb_grid_get_voxel_size(buf, grid, 2));
    }

    if (ImGui::CollapsingHeader("Tree Information", ImGuiTreeNodeFlags_DefaultOpen))
    {
        pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);
        ImGui::Text("Active Voxel Count: %llu", (unsigned long long)pnanovdb_tree_get_voxel_count(buf, tree));
        ImGui::Text("Leaf Node Count: %u", pnanovdb_tree_get_node_count_leaf(buf, tree));
        ImGui::Text("Lower Internal Node Count: %u", pnanovdb_tree_get_node_count_lower(buf, tree));
        ImGui::Text("Upper Internal Node Count: %u", pnanovdb_tree_get_node_count_upper(buf, tree));
        ImGui::Text("Lower Tile Count: %u", pnanovdb_tree_get_tile_count_lower(buf, tree));
        ImGui::Text("Upper Tile Count: %u", pnanovdb_tree_get_tile_count_upper(buf, tree));
        ImGui::Text("Root Tile Count: %u", pnanovdb_tree_get_tile_count_root(buf, tree));
    }

    if (ImGui::CollapsingHeader("Root Information", ImGuiTreeNodeFlags_DefaultOpen))
    {
        pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);
        pnanovdb_root_handle_t root = pnanovdb_tree_get_root(buf, tree);

        pnanovdb_coord_t root_bbox_min = pnanovdb_root_get_bbox_min(buf, root);
        pnanovdb_coord_t root_bbox_max = pnanovdb_root_get_bbox_max(buf, root);
        ImGui::Text("Index BBox Min: (%d, %d, %d)", root_bbox_min.x, root_bbox_min.y, root_bbox_min.z);
        ImGui::Text("Index BBox Max: (%d, %d, %d)", root_bbox_max.x, root_bbox_max.y, root_bbox_max.z);

        ImGui::Text("Root Tile Count: %u", pnanovdb_root_get_tile_count(buf, root));
    }

    if (ImGui::CollapsingHeader("Blind Metadata"))
    {
        pnanovdb_grid_handle_t grid_handle = grid;
        uint32_t blindMetadataCount = pnanovdb_grid_get_blind_metadata_count(buf, grid_handle);
        int64_t blindMetadataOffset = pnanovdb_grid_get_blind_metadata_offset(buf, grid_handle);

        ImGui::Text("Blind Metadata Count: %u", blindMetadataCount);
        ImGui::Text("Blind Metadata Offset: %lld", (long long)blindMetadataOffset);

        if (blindMetadataCount > 0 && blindMetadataOffset != 0)
        {
            ImGui::Indent();
            for (uint32_t i = 0; i < blindMetadataCount && i < 10; ++i)
            {
                pnanovdb_gridblindmetadata_handle_t metadata;
                metadata.address = pnanovdb_address_offset(grid_handle.address, blindMetadataOffset + i * 288);

                uint32_t dataClass = pnanovdb_gridblindmetadata_get_data_class(buf, metadata);
                uint64_t valueCount = pnanovdb_gridblindmetadata_get_value_count(buf, metadata);
                uint32_t valueSize = pnanovdb_gridblindmetadata_get_value_size(buf, metadata);

                const char* className = "Unknown";
                switch (dataClass)
                {
                case 0:
                    className = "Unknown";
                    break;
                case 1:
                    className = "Index Array";
                    break;
                case 2:
                    className = "Attribute Array";
                    break;
                case 3:
                    className = "Grid Name";
                    break;
                case 4:
                    className = "Channel Array";
                    break;
                default:
                    className = "Invalid";
                    break;
                }

                ImGui::Text("Entry %u: %s (%u values, %u bytes each)", i, className, (unsigned)valueCount, valueSize);
            }
            if (blindMetadataCount > 10)
            {
                ImGui::Text("... and %u more entries", blindMetadataCount - 10);
            }
            ImGui::Unindent();
        }
        else
        {
            ImGui::Text("No blind metadata present");
        }
    }

    if (ImGui::CollapsingHeader("Memory Usage"))
    {
        ImGui::Text("Total Buffer Size: %llu bytes", (unsigned long long)(array->element_count * array->element_size));
        ImGui::Text("Element Size: %llu bytes", (unsigned long long)array->element_size);
        ImGui::Text("Element Count: %llu", (unsigned long long)array->element_count);
    }

    return true;
}

} // namespace pnanovdb_editor
