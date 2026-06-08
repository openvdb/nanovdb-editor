// icosahedron.slang
#include "editor_params.slang"

struct shader_params_t
{
    uint mode;
};

StructuredBuffer<uint2> buf;
RWStructuredBuffer<uint> image_out_deprecated;
RWTexture2D<float4> texture_out;
ConstantBuffer<EditorParams> editor_params;
ConstantBuffer<shader_params_t> shader_params;

static const float g = (1.f + sqrt(5.f)) / 2.f;
static const float3 verts[12u] = {
    {0.f, -1.f, -g},
    {0.f, 1.f, -g},
    {0.f, -1.f, g},
    {0.f, 1.f, g},
    {-1.f, -g, 0.f},
    {1.f, -g, 0.f},
    {-1.f, g, 0.f},
    {1.f, g, 0.f},
    {-g, 0.f, -1.f},
    {-g, 0.f, 1.f},
    {g, 0.f, -1.f},
    {g, 0.f, 1.f}
};
static const float sphere_radius = sqrt(g*g + 1.f);

float3 cross_product(float3 a, float3 b)
{
    return float3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

// based on: https://iquilezles.org/articles/intersectors/
float2 intersect_sphere(float3 ro, float3 rd, float3 ce, float ra)
{
    float3 oc = ro - ce;
    float b = dot(oc, rd);
    float3 qc = oc - b*rd;
    float h = ra*ra - dot(qc, qc);
    if (h < 0.f) return float2(-1.f); // no intersection
    h = sqrt(h);
    return float2(-b - h, -b + h);
}

// based on: https://iquilezles.org/articles/intersectors/
float3 intersect_triangle(float3 ro, float3 rd, float3 v0, float3 v1, float3 v2)
{
    float3 v1v0 = v1 - v0;
    float3 v2v0 = v2 - v0;
    float3 rov0 = ro - v0;
    float3  n = cross_product( v1v0, v2v0 );
    float3  q = cross_product( rov0, rd );
    float d = 1.0f/dot( rd, n );
    float u = d*dot( -q, v2v0 );
    float v = d*dot(  q, v1v0 );
    float t = d*dot( -n, rov0 );
    if( u<0.0f || v<0.0f || (u+v)>1.0f ) t = -1.0f;
    return float3( t, u, v );
}

void ray_intersect(float3 worldRayOrigin,
                   float rayMinT,
                   float3 worldRayDir,
                   float rayMaxT,
                   float3 worldRayDirInv,
                   inout float4 sum,
                   inout float nominalT)
{
    for (uint vert_idx = 0u; vert_idx < 12u; vert_idx++)
    {
        float2 pt_isect = intersect_sphere(worldRayOrigin, worldRayDir, verts[vert_idx], 0.1f);
        if (pt_isect.x > 0.f)
        {
            float alpha = 1.f;
            float3 color = float3(0.33f, 0.33f, 0.33f);
            if (pt_isect.x < nominalT || nominalT == 0.f)
            {
                nominalT = pt_isect.x;
                sum.rgb = alpha * color;
                sum.a = 0.f;
            }
        }
    }

    float2 sphere_isect = intersect_sphere(worldRayOrigin, worldRayDir, float3(0.f), sphere_radius);
    if (sphere_isect.x < 0.f)
    {
        return;
    }

    float3 rayOrigin2 = worldRayDir * sphere_isect.x + worldRayOrigin;
    float3 rayDir2 = -normalize(rayOrigin2);
    // offset ray origin off of sphere face
    rayOrigin2 = rayOrigin2 - rayDir2;

    float3 min_dot = float3(1.f, 1.f, 1.f);
    int3 min_idx = int3(-1, -1, -1);
    for (uint vert_idx = 0u; vert_idx < 12u; vert_idx++)
    {
        float cur_dot = dot(rayDir2, verts[vert_idx]);
        int cur_idx = vert_idx;
        if (cur_dot < min_dot.x)
        {
            float old_dot = min_dot.x;
            int old_idx = min_idx.x;
            min_dot.x = cur_dot;
            min_idx.x = cur_idx;
            cur_dot = old_dot;
            cur_idx = old_idx;
        }
        if (cur_dot < min_dot.y)
        {
            float old_dot = min_dot.y;
            int old_idx = min_idx.y;
            min_dot.y = cur_dot;
            min_idx.y = cur_idx;
            cur_dot = old_dot;
            cur_idx = old_idx;
        }
        if (cur_dot < min_dot.z)
        {
            float old_dot = min_dot.z;
            int old_idx = min_idx.z;
            min_dot.z = cur_dot;
            min_idx.z = cur_idx;
            cur_dot = old_dot;
            cur_idx = old_idx;
        }
    }

    min_idx.xy = int2(min(min_idx.x, min_idx.y), max(min_idx.x, min_idx.y));
    min_idx.yz = int2(min(min_idx.y, min_idx.z), max(min_idx.y, min_idx.z));
    min_idx.xy = int2(min(min_idx.x, min_idx.y), max(min_idx.x, min_idx.y));

    float3 t_isect = intersect_triangle(rayOrigin2, rayDir2, verts[min_idx.x], verts[min_idx.y], verts[min_idx.z]);
    //if (t_isect.x > 0.f)
    {
        float alpha = 1.f;
        float3 color = float3(1.f - t_isect.y - t_isect.z, t_isect.y, t_isect.z);
        if (sphere_isect.x < nominalT || nominalT == 0.f)
        {
            nominalT = sphere_isect.x;
            sum.rgb = alpha * color;
            sum.a = 0.f;
        }
    }
}

[shader("compute")][numthreads(16, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID, uint3 thread_idx : SV_GroupThreadID)
{
    int2 tidx = int2(dispatchThreadID.xy);

    float2 ndc = float2(2.f * ((float(tidx.x) + 0.5f) / float(editor_params.width)) - 1.f,
                        -2.f * ((float(tidx.y) + 0.5f) / float(editor_params.height)) + 1.f);

    float4 pos_d0 = mul(float4(ndc.xy, 0.f, 1.f), editor_params.projection_inv);
    float4 pos_d1 = mul(float4(ndc.xy, 1.f, 1.f), editor_params.projection_inv);

    float z_d0 = pos_d0.z * (1.f / pos_d0.w);
    float z_d1 = pos_d1.z * (1.f / pos_d1.w);
    bool is_reverse_z = abs(z_d0) > abs(z_d1);
    float4 ray_dir_near = is_reverse_z ? pos_d1 : pos_d0;

    float4 ray_dir_far = ray_dir_near + mul(float4(0.f, 0.f, 1.f, 0.f), editor_params.projection_inv);
    float3 rayDir = normalize((ray_dir_far.xyz / ray_dir_far.w) - (ray_dir_near.xyz / ray_dir_near.w));
    if (is_reverse_z)
    {
        rayDir = -rayDir;
    }

    rayDir = mul(float4(rayDir, 0.f), editor_params.view_inv).xyz;
    float3 rayDirInv = float3(1.f, 1.f, 1.f) / rayDir;

    float4 rayOrigin4 = is_reverse_z ? pos_d1 : pos_d0;
    rayOrigin4 = mul(rayOrigin4, editor_params.view_inv);
    float3 rayOrigin = rayOrigin4.xyz / rayOrigin4.w;

    float4 sum = float4(0.f, 0.f, 0.f, 1.f);
    float nominalT = 0.f;
    ray_intersect(rayOrigin, 0.f, rayDir, 1e9f, rayDirInv, sum, nominalT);

    texture_out[tidx] = sum;
}
