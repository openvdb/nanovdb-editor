# Flow smoke rendering

`editor/flow_smoke.slang` renders Flow NanoVDB CPU readback buffers. The first
raw Float32 grid contains smoke density. Append an optional raw Float32
temperature grid to the same buffer; each grid retains its header and transform.
The first grid controls the bounds. Without a second grid, the shader uses
`fallback_temperature` (zero by default). These are raw grids, not `.nvdb` file
containers.

The shader samples both fields with trilinear interpolation. Flow exports voxel
indices without translating its NanoVDB map by half a cell, so sample centers
are at index coordinates `ijk + 0.5`.

The material follows the non-cloud, non-raw path in Flow's
`nvflow/include/nvflowext/shaders/NvFlowRayMarch.hlsli` and
`NvFlowRayMarchUtils.h`:

```
world_step = step_size_scale * min(voxel_size)
color = filtered_colormap((temperature - colormap_min) / (colormap_max - colormap_min))
shadow_alpha = clamp(shadow_colormap.a * shadow_smoke, 0, 1) * (1 - exp(-shadow_attenuation * shadow_step))
shadow_transmittance *= 1 - shadow_alpha
light = shadow_min_intensity + (1 - shadow_min_intensity) * shadow_transmittance
color.rgb *= color_scale * (1 + shadow_factor * (light - 1))
alpha = clamp(color.a * smoke, 0, 1) * (1 - exp(-attenuation * world_step))
rgb += transmittance * alpha * color.rgb
transmittance *= 1 - alpha
```

The output alpha stores transmittance, as required by the editor compositor.
The colormap evaluates control points at texel centers, then linearly filters
adjacent texels, matching Flow's lookup construction and sampling. The shader
uses Float32 arithmetic and rounds lookup texels to Float16 before filtering,
as Flow does with its half-precision colormap texture.
Default parameters reproduce the SDK's six smoke control points, lookup
resolution, and color scale in `NvFlowRayMarchColormapParams`, with its default
ray march settings. The native NvFlow editor overrides `colorScale` to one in
`nvflow/source/nvfloweditor/EditorFlowStages.cpp`; use `color_scale=1` for that
preset. The legacy USD schema differs from the SDK at the fifth alpha control
point. This shader follows the SDK value, 0.904902.

Pass overrides through `Scene.set_shader(..., parameters=...)` or
`Scene.nanovdb_from_buffer(..., shader_parameters=...)`. The material accepts one
to eight control points. Set `point_count`, `point_positions0` (points 0–3),
`point_positions1` (points 4–7), and `point_color0` through `point_color7` (RGBA).
Fold Flow's per-point color scales into the point RGB values, and multiply its
colormap and ray march color scales into `color_scale`. This combined scale is
applied after lookup quantization; exact rounding matches the native editor's
unit colormap scale, but can differ for other colormap scales. Set
`colormap_resolution` to the authored lookup resolution. Positions outside the
control point range use the nearest endpoint.

Select the smoke object and open **Properties** to edit the **Color ramp**.
Select a stop to edit its RGBA color and position, drag a stop along the ramp,
and use **Add stop** or **Remove stop** to change the number of stops (one through eight).
New stops interpolate the current ramp. Color and alpha values can exceed one;
the preview clamps colors for display without changing the stored values.
Alpha controls the density multiplier used by the opacity calculation.

Ramp positions run from zero to one over the temperature range set by
`colormap_min` and `colormap_max`. The PR28 smoke scene has zero temperature,
so edit the leftmost stops to tint it. The filtered lookup blends nearby stops;
a color change at the hot end alone will not affect cold smoke. Live buffer
updates preserve ramp edits, just like other material parameters.

For constant smoke, set two points at zero and one with the same RGBA. To use
authored fire colors, supply both smoke and temperature grids with the authored
colormap. Fuel and burn are not required by this rendering path.

Directional self-shadowing uses the attenuation, minimum intensity, and color
modulation equations from `nvflow/source/nvflowext/shaders/ShadowUpdateCS.hlsl`
and `NvFlowRayMarch.hlsli`. It samples the smoke and temperature NanoVDBs along
the light direction. Defaults match the native directional light: direction
`(1,1,1)`, attenuation `0.045`, minimum intensity `0.125`, 16 samples, step scale
`0.75`, and origin offset scale `1`. `shadow_cell_size_scale=2` matches the
sample spacing and ray extent of the native default coarse shadow pass. Use
one for the native fine shadow spacing. `shadow_factor=0` disables shadowing.
Both camera and shadow rays use NvFlow's deterministic directional jitter.
Camera rays use at most 4096 samples. Longer rays increase sample spacing to
cover the full volume and use that spacing in the opacity calculation. Shadow
rays clamp `shadow_num_steps` to 128, the upper limit of its UI control. Camera
sample offsets are relative to the volume entry point so a distant camera does
not prevent small steps from advancing.

Lighting is computed per camera sample from the full-resolution NanoVDB
fields. The native renderer computes a shadow volume before rendering and, by
default, downsamples density then upsamples the resulting shadow field. This
shader does not reproduce that coarse filtering or shadow-volume interpolation,
so matching parameters do not establish pixel parity. Inline shadow rays also
cost more per displayed pixel than a reusable shadow prepass.

Cloud mode, point lights, raw mode, wireframes, motion vectors, temporal
accumulation, and tone mapping are not implemented. The raw burn channel is not
a shadow-lighting channel. USDA material extraction belongs to the caller;
selecting this shader does not import stage materials automatically.

`pytests/test_flow_smoke.py` compiles the renderer and executes its shared
material functions through Slang's CPU target. It checks temperature lookup,
step-dependent opacity, density clamping, compositing, transparent controls,
shadow transmittance, and the native minimum-light floor.
