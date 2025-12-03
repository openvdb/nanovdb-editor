FROM nvidia/cuda:12.8.1-base-ubuntu24.04

## Build examples:
# docker build --progress=plain . --network=host -t nanovdb-editor                                              # Default: nanovdb-editor from PyPI
# docker build --progress=plain . --network=host --build-arg PYPI_PACKAGE=nanovdb-editor-dev -t nanovdb-editor  # Use dev release of nanovdb-editor
# docker build --progress=plain . --network=host --build-arg USE_LOCAL_WHEEL=true -t nanovdb-editor             # Use local wheel
# docker build --progress=plain . --network=host --build-arg INSTALL_GRAPHICS_LIBS=false -t nanovdb-editor      # Minimal headless build for CPU encode

## Run NanoVDB Editor in docker:
# docker run --runtime=nvidia --net=host --gpus=all -it nanovdb-editor bash
# python3 ./run_editor.py --headless --stream

# Set environment variables to prevent interactive prompts during installation.
ENV DEBIAN_FRONTEND=noninteractive

# RUN sed -i 's/archive.ubuntu.com/mirrors.ocf.berkeley.edu/g' /etc/apt/sources.list

# Install Python
# skipping python3-dev python3-venv python-is-python3
RUN apt-get update && \
    apt-get install -y python3-pip && \
    rm -rf /var/lib/apt/lists/*
#    && \
#    python -m pip install --upgrade pip

WORKDIR /workspace

## Build arguments:
#  - installation mode (default: false = use PyPI)
ARG USE_LOCAL_WHEEL=false
#  - PyPI package (default: nanovdb-editor)
ARG PYPI_PACKAGE=nanovdb-editor
#  - installation of graphics dependencies
ARG INSTALL_GRAPHICS_LIBS=true

COPY ./pymodule/dist/ ./dist/
RUN if [ "$USE_LOCAL_WHEEL" = "true" ]; then \
        echo "Installing from local wheel files..."; \
        pip install --break-system-packages numpy ./dist/*.whl; \
    else \
        echo "Installing from PyPI package: $PYPI_PACKAGE"; \
        pip install --break-system-packages numpy $PYPI_PACKAGE; \
    fi

# Find the nanovdb-editor installation directory and copy run_editor.py when available
RUN python3 - <<'PY'
import importlib
import os
import shutil

mod = importlib.import_module("nanovdb_editor")
src = os.path.join(os.path.dirname(mod.__file__), "run_editor.py")
dst = "./run_editor.py"
if os.path.exists(src):
    shutil.copy2(src, dst)
    print(f"Copied {src} -> {dst}")
else:
    print("run_editor.py not present in package; skipping copy")
PY

COPY ./test.py ./test.py

EXPOSE 8080

ENV NVIDIA_DRIVER_CAPABILITIES compute,graphics,utility

RUN if [ "$INSTALL_GRAPHICS_LIBS" = "true" ]; then \
        # Install X11/EGL libraries needed by host NVIDIA Vulkan driver (when using --gpus) \
        apt-get update && apt-get install -y \
            libxext6 \
            libegl1 \
        && rm -rf /var/lib/apt/lists/*; \
    else \
        # Install lavapipe for headless CPU encoding \
        apt-get update && apt-get install -y \
            mesa-vulkan-drivers \
        && rm -rf /var/lib/apt/lists/*; \
    fi
