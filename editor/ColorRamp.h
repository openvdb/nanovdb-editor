// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstddef>
#include <vector>

namespace pnanovdb_editor
{

struct ColorRampPoint
{
    float position;
    std::array<float, 4> color;
};

std::array<float, 4> sampleColorRamp(const std::vector<ColorRampPoint>& points, float position);
bool addColorRampPoint(std::vector<ColorRampPoint>& points, size_t capacity, float position);
bool removeColorRampPoint(std::vector<ColorRampPoint>& points, size_t index);
bool renderColorRamp(const char* label, std::vector<ColorRampPoint>& points, size_t capacity);

} // namespace pnanovdb_editor
