// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0
// Portions adapted from NVIDIA NvFlow, Copyright (c) 2014-2026 NVIDIA CORPORATION & AFFILIATES.

bool flow_intersect_box(float3 origin, float3 direction, float3 lower, float3 upper,
                        out float near_t, out float far_t)
{
    near_t = 0.f;
    far_t = 1e30f;
    for (uint axis = 0; axis < 3; axis++)
    {
        if (direction[axis] == 0.f)
        {
            if (origin[axis] < lower[axis] || origin[axis] > upper[axis]) return false;
        }
        else
        {
            float a = (lower[axis] - origin[axis]) / direction[axis];
            float b = (upper[axis] - origin[axis]) / direction[axis];
            near_t = max(near_t, min(a, b));
            far_t = min(far_t, max(a, b));
        }
    }
    return near_t < far_t;
}

float flow_sample_grid(inout pnanovdb_readaccessor_t accessor, float3 position)
{
    // Flow exports cell indices without a half-cell translation in the grid map.
    float3 centered = position - 0.5f;
    int3 base = int3(floor(centered));
    float3 fraction = centered - float3(base);
    float value = 0.f;
    for (uint index = 0; index < 8; index++)
    {
        int3 offset = int3(index & 1u, (index >> 1u) & 1u, (index >> 2u) & 1u);
        float3 weight = lerp(1.f - fraction, fraction, float3(offset));
        pnanovdb_address_t address = pnanovdb_readaccessor_get_value_address(
            PNANOVDB_GRID_TYPE_FLOAT, buf, PNANOVDB_REF(accessor), base + offset);
        value += weight.x * weight.y * weight.z * pnanovdb_read_float(buf, address);
    }
    return value;
}

float flow_ray_noise(float3 direction)
{
    float3 magnitude = abs(direction);
    float2 uv = magnitude.x > magnitude.y && magnitude.x > magnitude.z ? direction.yz :
        (magnitude.y > magnitude.x && magnitude.y > magnitude.z ? direction.xz : direction.xy);
    uv = 0.5f * uv / max(magnitude.x, max(magnitude.y, magnitude.z)) + 0.5f;
    uint input_value = uint(65535.f * uv.x) ^ (uint(65535.f * uv.y) << 16u);
    uint state = input_value * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return float(((word >> 22u) ^ word) & 0xFFFFu) / 65535.f;
}

float flow_shadow(float3 world_position, float cell_size,
                  pnanovdb_grid_handle_t smoke_grid, pnanovdb_readaccessor_t smoke_accessor,
                  pnanovdb_grid_handle_t temperature_grid, pnanovdb_readaccessor_t temperature_accessor,
                  bool has_temperature)
{
    float direction_length = length(shader_params.shadow_direction);
    if (shader_params.shadow_factor == 0.f || shader_params.shadow_num_steps == 0u || direction_length == 0.f)
        return 1.f;

    float3 direction = shader_params.shadow_direction / direction_length;
    float shadow_cell = cell_size * max(shader_params.shadow_cell_size_scale, 0.01f);
    float step_size = shadow_cell * max(shader_params.shadow_step_size_scale, 0.01f);
    float3 origin = world_position + direction * shadow_cell * shader_params.shadow_step_offset_scale;
    float3 smoke_origin = pnanovdb_grid_world_to_indexf(buf, smoke_grid, origin);
    float3 smoke_step = pnanovdb_grid_world_to_index_dirf(buf, smoke_grid, direction * step_size);
    float3 temperature_origin = pnanovdb_grid_world_to_indexf(buf, temperature_grid, origin);
    float3 temperature_step = pnanovdb_grid_world_to_index_dirf(buf, temperature_grid, direction * step_size);
    float noise = flow_ray_noise(direction);
    float sample_offset = ceil(noise) - noise;
    shader_params_t shadow_material = shader_params;
    shadow_material.colormap_min = shader_params.shadow_colormap_range.x;
    shadow_material.colormap_max = shader_params.shadow_colormap_range.y;
    float transmittance = 1.f;
    for (uint index = 0u; index < flow_shadow_step_count(shader_params.shadow_num_steps); index++)
    {
        float t = float(index) + sample_offset;
        float smoke = flow_sample_grid(smoke_accessor, smoke_origin + t * smoke_step);
        if (smoke > 0.f)
        {
            float temperature = has_temperature ?
                flow_sample_grid(temperature_accessor, temperature_origin + t * temperature_step) :
                shader_params.fallback_temperature;
            float alpha = flow_sample_colormap(shadow_material, temperature).a;
            transmittance *= 1.f - flow_opacity(alpha, smoke, shader_params.shadow_attenuation, step_size);
        }
    }
    return flow_shadow_light(shader_params, transmittance);
}

float4 flow_ray_march(float3 world_origin, float3 world_direction)
{
    float4 sum = float4(0.f, 0.f, 0.f, 1.f);
    pnanovdb_grid_handle_t smoke_grid = { pnanovdb_address_null() };
    if (pnanovdb_grid_get_grid_type(buf, smoke_grid) != PNANOVDB_GRID_TYPE_FLOAT) return sum;

    pnanovdb_tree_handle_t smoke_tree = pnanovdb_grid_get_tree(buf, smoke_grid);
    pnanovdb_root_handle_t smoke_root = pnanovdb_tree_get_root(buf, smoke_tree);
    pnanovdb_readaccessor_t smoke_accessor;
    pnanovdb_readaccessor_init(PNANOVDB_REF(smoke_accessor), smoke_root);

    uint element_count, stride;
    buf.GetDimensions(element_count, stride);
    pnanovdb_uint64_t smoke_size = pnanovdb_grid_get_grid_size(buf, smoke_grid);
    bool has_temperature = pnanovdb_uint64_high(smoke_size) == 0u &&
        pnanovdb_uint64_low(smoke_size) / 8u + PNANOVDB_GRID_SIZE / 8u <= element_count;
    pnanovdb_grid_handle_t temperature_grid = smoke_grid;
    pnanovdb_readaccessor_t temperature_accessor = smoke_accessor;
    if (has_temperature)
    {
        temperature_grid.address = pnanovdb_address_offset64(smoke_grid.address, smoke_size);
        has_temperature = pnanovdb_grid_get_grid_type(buf, temperature_grid) == PNANOVDB_GRID_TYPE_FLOAT;
        if (has_temperature)
        {
            pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, temperature_grid);
            pnanovdb_root_handle_t root = pnanovdb_tree_get_root(buf, tree);
            pnanovdb_readaccessor_init(PNANOVDB_REF(temperature_accessor), root);
        }
    }

    float3 origin = pnanovdb_grid_world_to_indexf(buf, smoke_grid, world_origin);
    float3 direction = pnanovdb_grid_world_to_index_dirf(buf, smoke_grid, world_direction);
    float near_t, far_t;
    if (!flow_intersect_box(origin, direction, float3(pnanovdb_root_get_bbox_min(buf, smoke_root)),
                            float3(pnanovdb_root_get_bbox_max(buf, smoke_root)) + 1.f, near_t, far_t)) return sum;

    float3 voxel_x = pnanovdb_grid_index_to_world_dirf(buf, smoke_grid, float3(1.f, 0.f, 0.f));
    float3 voxel_y = pnanovdb_grid_index_to_world_dirf(buf, smoke_grid, float3(0.f, 1.f, 0.f));
    float3 voxel_z = pnanovdb_grid_index_to_world_dirf(buf, smoke_grid, float3(0.f, 0.f, 1.f));
    float cell_size = min(length(voxel_x), min(length(voxel_y), length(voxel_z)));
    float world_step = max(shader_params.step_size_scale, 0.01f) * cell_size;
    if (!(world_step > 0.f)) return sum;

    float3 temperature_origin = pnanovdb_grid_world_to_indexf(buf, temperature_grid, world_origin);
    float3 temperature_direction = pnanovdb_grid_world_to_index_dirf(buf, temperature_grid, world_direction);
    flow_ray_steps_t steps = flow_ray_steps(near_t, far_t, world_step, flow_ray_noise(world_direction));
    float3 smoke_entry = origin + near_t * direction;
    float3 temperature_entry = temperature_origin + near_t * temperature_direction;
    float3 world_entry = world_origin + near_t * world_direction;
    uint shadow_stride = flow_shadow_sample_stride(steps.count, shader_params.shadow_num_steps);
    uint next_shadow_index = 0u;
    float shadow_light = 1.f;
    // Relative offsets keep small steps distinct at large camera distances.
    for (uint index = 0u; index < steps.count; index++)
    {
        float offset = steps.first_offset + float(index) * steps.step_size;
        float smoke = flow_sample_grid(smoke_accessor, smoke_entry + offset * direction);
        if (smoke > 0.f)
        {
            float temperature = has_temperature ?
                flow_sample_grid(temperature_accessor, temperature_entry + offset * temperature_direction) :
                shader_params.fallback_temperature;
            float4 color = flow_smoke_sample(shader_params, temperature, smoke, steps.step_size);
            if (color.a <= 0.f) continue;
            if (index >= next_shadow_index)
            {
                // Reuse lighting along long rays to bound the total shadow samples.
                shadow_light = flow_shadow(world_entry + offset * world_direction, cell_size,
                    smoke_grid, smoke_accessor, temperature_grid, temperature_accessor, has_temperature);
                next_shadow_index = index + shadow_stride;
            }
            color.rgb *= shadow_light;
            flow_accumulate(color, sum);
            if (sum.a < 0.00005f) break;
        }
    }
    return sum;
}
