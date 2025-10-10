#  NanoVDB Editor

WIP

## Building and executing NanoVDB Editor

### Prerequisites
- git
- C++ compiler
- CMake at least 3.25
- Python 3.x
- Vulkan library `vulkan-1` (should be installed with graphics driver)
#### Streaming
- NVIDIA Driver Version at least 550.0 (https://developer.nvidia.com/vulkan/video/get-started)
- Vulkan video at least 1.3.275.0 (checked during editor startup, prints out a message if the upgrade is needed)

##### Upgrading Vulkan SDK on Linux
1. Download the latest Vulkan SDK from: https://vulkan.lunarg.com/sdk/home#linux
2. Follow the instructions for manual installation in section `Install the SDK`: https://vulkan.lunarg.com/doc/view/latest/linux/getting_started.html

```sh
VULKAN_SDK_VERSION="1.3.275.0"
cd ~
mkdir vulkan
cd vulkan
wget "https://sdk.lunarg.com/sdk/download/${VULKAN_SDK_VERSION}/linux/vulkansdk-linux-x86_64-${VULKAN_SDK_VERSION}.tar.xz"
tar xf "vulkansdk-linux-x86_64-${VULKAN_SDK_VERSION}.tar.xz"
echo "source ~/vulkan/${VULKAN_SDK_VERSION}/setup-env.sh" >> ~/.profile
. ~/.profile
# check the version
echo $VULKAN_SDK
```

#### Windows
- vcpkg (optional - recommended for e57 dependency)

### Dependencies
#### Python
```
pip install scikit-build wheel build
```
#### Linux
By default, editor is built with enabled `NANOVDB_EDITOR_USE_GLFW` which requires in Conda environment:
- mesalib

The `NANOVDB_EDITOR_USE_GLFW` option can be disabled when using the editor in haedless and streaming mode only. In that case, `libvulkan.so.1` is built locally to ensure compatibility.

For building openh264 enable `NANOVDB_EDITOR_USE_H264` and make sure you have:
```sh
sudo apt-get install make
```

#### macOS
```sh
# install Vulkan SDK from https://vulkan.lunarg.com/sdk/home#mac
# install homebrew from https://brew.sh
# brew install cmake
brew install glfw      # TODO: will be optional for streaming

```
#### Windows
Optionally, the project can use `vcpkg` for dependency management. If `NANOVDB_EDITOR_USE_VCPKG` is set, `vcpkg.json` automatically installs required dependencies.

To set up vcpkg:
```bat
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
bootstrap-vcpkg.bat
```

The following dependencies are automatically managed by `vcpkg.json`:
- blosc
- libe57format (and xerces-c dependency)

### Assets
Put any data files into the `data` folder, which is linked to the location next to the libraries.
Shaders are generated into the `shaders/_generated` folder next to the libraries.

### Build and Run
Run the build script with `-h` for available build options.

#### Linux
```sh
./build.sh
```

#### Windows
Optionally, rename the config file `config` next to the build script to `config.ini` and fill in the environment variables:
```
MSVS_VERSION="Visual Studio 17 2022"
USE_VCPKG=ON
VCPKG_ROOT=path/to/vcpkg
```

To select a different profile for Slang compiler (https://github.com/shader-slang/slang/blob/master/source/slang/slang-profile-defs.h):
```
SLANG_PROFILE="sm_5_1"
```
Then, run the build script:
```bat
./build.bat
```

### Editor Application
After building, run the editor app:
```sh
./build/Release/pnanovdbeditorapp
```

### Python

The libraries can be bundled into a Python package with a wrapper for the C-type functions. The following script will automatically install scikit-build, wheel, and build dependencies:

Build with `-p` to build and install the `nanovdb_editor` package.

#### Python Test Apps

```sh
./build.sh -p
python3 test/test_editor.py
```

```sh
./build.sh -p
python3 test/test_streaming.py
```

#### Debugging
Add this line to the Python test script to print the PID of the process:
```python
import os
print(os.getpid())
```

1. Build the debug configuration
2. Run the test script; the console output will print the PID of the process
3. Attach to the process:

##### Linux

With GDB:
```sh
gdb -p <PID>
```

## NanoVDB Editor GUI

### Shader Parameters
Shaders can have a defined struct with shader parameters that are intended to be shown in the editor's UI:

```hlsl
struct shader_params_t
{
    float4 color;
    bool use_color;
    bool3 _pad1;
    int _pad2;
};
```

Shader parameters can have defined default values in the JSON file:
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
These parameters can be interactively changed with the generated UI in the editor's Params tab.

To display a group of shader parameters from different shaders, define a JSON file with various shader paths:
```json
{
    "ShaderParams": [
        "editor/editor.slang",
        "test/test.slang"
    ]
}
```

## Acknowledgements

This project makes use of the following libraries:

- [zlib](https://github.com/madler/zlib) – Compression library
- [c-blosc](https://github.com/Blosc/c-blosc) – High-performance compressor optimized for binary data
- [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) – Vulkan API headers
- [Vulkan-Loader](https://github.com/KhronosGroup/Vulkan-Loader) – Vulkan ICD loader
- [GLFW](https://github.com/glfw/glfw) – Windowing, context, and input (optional)
- [Dear ImGui](https://github.com/ocornut/imgui) – Immediate-mode GUI
- [ImGuiFileDialog](https://github.com/aiekick/ImGuiFileDialog) – File dialog for Dear ImGui
- [ImGuiColorTextEdit](https://github.com/goossens/ImGuiColorTextEdit) – Syntax-highlighted text/code editor widget
- [Slang](https://github.com/shader-slang/slang) – Shading language and compiler
- [filewatch](https://github.com/ThomasMonkman/filewatch) – Cross-platform file watching
- [JSON for Modern C++](https://github.com/nlohmann/json) – JSON serialization for C++
- [cnpy](https://github.com/rogersce/cnpy) – Read/write NumPy .npy/.npz files from C++
- [zstr](https://github.com/mateidavid/zstr) – Transparent zlib iostream wrappers
- [llhttp](https://github.com/nodejs/llhttp) – High-performance HTTP parser
- [Asio](https://github.com/chriskohlhoff/asio) – Asynchronous networking and concurrency primitives
- [RESTinio](https://github.com/Stiffstream/restinio) – Lightweight HTTP server framework
- [fmt](https://github.com/fmtlib/fmt) – Modern formatting library
- [argparse](https://github.com/morrisfranken/argparse) – Header-only argument parser for C++17
- [expected-lite](https://github.com/martinmoene/expected-lite) – std::expected-like type for C++11/14/17
- [libE57Format](https://github.com/asmaloney/libE57Format) – E57 point cloud IO (optional)
- [OpenH264](https://github.com/cisco/openh264) – H.264 encoder (optional)
- [GoogleTest](https://github.com/google/googletest) – C++ testing framework

Many thanks to the authors and contributors of these projects.
