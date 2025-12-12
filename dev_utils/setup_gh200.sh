#!/bin/bash
# NanoVDB Editor Setup Script for Ubuntu 24.04 with NVIDIA GH200
# https://github.com/openvdb/nanovdb-editor
#
# USAGE:
#   1. Clone the repository first:
#      git clone https://github.com/openvdb/nanovdb-editor.git
#      cd nanovdb-editor
#
#   2. Run this script:
#      ./dev_utils/setup_gh200.sh
#
#   3. Run the editor:
#      ./build/Release/pnanovdbeditorapp

set -e

# Get the project root (parent of dev_utils)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "=============================================="
echo "NanoVDB Editor Setup for NVIDIA GH200"
echo "=============================================="
echo "Project root: $PROJECT_ROOT"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

print_status() { echo -e "${GREEN}[✓]${NC} $1"; }
print_warning() { echo -e "${YELLOW}[!]${NC} $1"; }
print_error() { echo -e "${RED}[✗]${NC} $1"; }

# Check if running as root for package installation
check_sudo() {
    if ! sudo -n true 2>/dev/null; then
        echo "This script requires sudo access for package installation."
        sudo -v
    fi
}

# Fix broken apt sources if needed
fix_apt_sources() {
    if sudo apt-get update 2>&1 | grep -q "is not known"; then
        print_warning "Fixing broken apt sources..."
        sudo mv /etc/apt/sources.list.d/nvidia-container-toolkit.list \
               /etc/apt/sources.list.d/nvidia-container-toolkit.list.bak 2>/dev/null || true
        sudo apt-get update
    fi
}

# Install build dependencies
install_dependencies() {
    print_status "Installing build dependencies..."
    sudo apt-get update
    sudo apt-get install -y \
        cmake \
        pkg-config \
        libglfw3-dev \
        libvulkan-dev \
        libgl1-mesa-dev
}

# Install NVIDIA driver (open kernel modules for GH200)
install_nvidia_driver() {
    if dpkg -l | grep -q "nvidia-driver-.*-open"; then
        print_status "NVIDIA open driver already installed"
        return 0
    fi

    print_status "Installing NVIDIA driver (open kernel modules for GH200)..."
    sudo apt-get install -y nvidia-driver-580-open vulkan-tools
}

# Setup GH200 memory configuration
setup_gh200_memory() {
    local current_mode=$(cat /sys/devices/system/memory/auto_online_blocks 2>/dev/null || echo "unknown")

    if [ "$current_mode" != "online_movable" ]; then
        print_status "Setting memory online mode for GH200..."
        echo "online_movable" | sudo tee /sys/devices/system/memory/auto_online_blocks > /dev/null
    else
        print_status "Memory online mode already configured"
    fi
}

# Load NVIDIA kernel modules
load_nvidia_modules() {
    print_status "Loading NVIDIA kernel modules..."
    sudo rmmod nvidia_uvm nvidia_drm nvidia_modeset nvidia 2>/dev/null || true
    sudo modprobe nvidia
    sudo modprobe nvidia_uvm
    sudo modprobe nvidia_drm
}

# Verify GPU is working
verify_gpu() {
    print_status "Verifying GPU..."
    if nvidia-smi --query-gpu=name,memory.total --format=csv,noheader 2>/dev/null; then
        print_status "GPU detected and working!"
        return 0
    else
        print_error "GPU not detected. Check dmesg for errors."
        return 1
    fi
}

# Make GPU settings persistent
make_persistent() {
    print_status "Making GPU settings persistent..."

    # Add kernel parameter if not already present
    if ! grep -q "memhp_default_state=online_movable" /etc/default/grub 2>/dev/null; then
        sudo sed -i 's/GRUB_CMDLINE_LINUX_DEFAULT="\([^"]*\)"/GRUB_CMDLINE_LINUX_DEFAULT="\1 memhp_default_state=online_movable"/' /etc/default/grub
        sudo update-grub
        print_status "Added kernel parameter to GRUB"
    else
        print_status "Kernel parameter already configured"
    fi
}

# Build the project
build_project() {
    print_status "Building nanovdb-editor (Release)..."
    cd "$PROJECT_ROOT"
    ./build.sh -r
}

# Main execution
main() {
    # Verify we're in a valid nanovdb-editor checkout
    if [ ! -f "$PROJECT_ROOT/build.sh" ] || [ ! -f "$PROJECT_ROOT/CMakeLists.txt" ]; then
        print_error "This script must be run from within a nanovdb-editor repository."
        echo ""
        echo "Please clone the repository first:"
        echo "  git clone https://github.com/openvdb/nanovdb-editor.git"
        echo "  cd nanovdb-editor"
        echo "  ./dev_utils/setup_gh200.sh"
        exit 1
    fi

    echo ""
    check_sudo

    echo ""
    echo "Step 1: Fixing apt sources..."
    fix_apt_sources

    echo ""
    echo "Step 2: Installing dependencies..."
    install_dependencies

    echo ""
    echo "Step 3: Installing NVIDIA driver..."
    install_nvidia_driver

    echo ""
    echo "Step 4: Setting up GH200 memory..."
    setup_gh200_memory

    echo ""
    echo "Step 5: Loading NVIDIA modules..."
    load_nvidia_modules

    echo ""
    echo "Step 6: Verifying GPU..."
    verify_gpu || exit 1

    echo ""
    echo "Step 7: Making settings persistent..."
    make_persistent

    echo ""
    echo "Step 8: Building project..."
    build_project

    echo ""
    echo "=============================================="
    print_status "Setup complete!"
    echo "=============================================="
    echo ""
    echo "Run the editor with:"
    echo "  $PROJECT_ROOT/build/Release/pnanovdbeditorapp"
    echo ""
    echo "GPU Info:"
    nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv
    echo ""
}

main "$@"
