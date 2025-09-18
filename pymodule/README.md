##  NanoVDB Editor

WIP

### Shader Parameters
Shaders can have defined struct with shader parameters which are intended to be shown in the editor's UI:
```hlsl
struct shader_params_t
{
    float4 color;
    bool use_color;
    bool3 _pad1;
    int _pad2;
};
ConstantBuffer<shader_params_t> shader_params;
```

Shader parameters can have defined default values in the json file:
```json
{
    "ShaderParams": {
        "color": {
            "value": [1.0, 0.0, 1.0, 1.0],
            "min": 0.0,
            "max": 1.0,
            "step": 0.01
        }
    }
}
```
Supported types: `bool`, `int`, `uint`, `int64`, `uint64`, `float` and its vectors and 4x4 matrix.
Variables with `_pad` in the name are not shown in the UI.
Those parameters can be interactively changed with generated UI in the editor's Params tab.

To display a group of shader parameters from different shaders define a json file with various shader paths:
```json
{
    "ShaderParams": [
        "editor/editor.slang",
        "test/test.slang"
    ]
}
```
