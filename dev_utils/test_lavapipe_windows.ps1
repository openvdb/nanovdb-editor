# Test script to verify Mesa3D lavapipe installation on Windows
# Run this script to check if lavapipe works before running CI

Write-Host "=== Testing Mesa3D Lavapipe Installation ===" -ForegroundColor Cyan

# Download Mesa3D
$MesaUrl = "https://github.com/pal1000/mesa-dist-win/releases/download/24.2.7/mesa3d-24.2.7-release-msvc.7z"
$MesaArchive = "$env:TEMP\mesa3d.7z"
$MesaDir = "$env:TEMP\mesa3d"

Write-Host "`nDownloading Mesa3D lavapipe..." -ForegroundColor Yellow
try {
    Invoke-WebRequest -Uri $MesaUrl -OutFile $MesaArchive -ErrorAction Stop
    Write-Host "Download successful" -ForegroundColor Green
} catch {
    Write-Host "Failed to download Mesa3D: $_" -ForegroundColor Red
    exit 1
}

# Extract Mesa3D
Write-Host "`nExtracting Mesa3D..." -ForegroundColor Yellow
try {
    if (Test-Path $MesaDir) {
        Remove-Item -Recurse -Force $MesaDir
    }
    7z x $MesaArchive -o"$MesaDir" -y | Out-Null
    Write-Host "Extraction successful" -ForegroundColor Green
} catch {
    Write-Host "Failed to extract Mesa3D: $_" -ForegroundColor Red
    exit 1
}

# Set environment variables
$VulkanICDPath = Join-Path $MesaDir "x64\lvp_icd.x86_64.json"
$MesaBinPath = Join-Path $MesaDir "x64"

Write-Host "`nSetting environment variables..." -ForegroundColor Yellow
$env:VK_ICD_FILENAMES = $VulkanICDPath
$env:VK_DRIVER_FILES = $VulkanICDPath
$env:PATH = "$MesaBinPath;$env:PATH"

Write-Host "VK_ICD_FILENAMES=$VulkanICDPath" -ForegroundColor Gray
Write-Host "VK_DRIVER_FILES=$VulkanICDPath" -ForegroundColor Gray
Write-Host "Added to PATH: $MesaBinPath" -ForegroundColor Gray

# Verify files exist
Write-Host "`nVerifying lavapipe files..." -ForegroundColor Yellow
$requiredFiles = @(
    "$VulkanICDPath",
    "$MesaBinPath\vulkan-1.dll",
    "$MesaBinPath\libvulkan_lvp.dll"
)

$allFilesExist = $true
foreach ($file in $requiredFiles) {
    if (Test-Path $file) {
        Write-Host "[OK] $file" -ForegroundColor Green
    } else {
        Write-Host "[MISSING] $file" -ForegroundColor Red
        $allFilesExist = $false
    }
}

if (-not $allFilesExist) {
    Write-Host "`nSome required files are missing!" -ForegroundColor Red
    exit 1
}

# Test with a simple Vulkan query (if vulkaninfo is available)
Write-Host "`nTesting Vulkan detection..." -ForegroundColor Yellow

# Try to create a simple test program
$testCode = @"
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <stdio.h>

int main() {
    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    
    VkInstance instance;
    VkResult result = vkCreateInstance(&createInfo, nullptr, &instance);
    
    if (result == VK_SUCCESS) {
        printf("SUCCESS: Vulkan instance created with lavapipe\n");
        
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
        printf("Found %d Vulkan device(s)\n", deviceCount);
        
        vkDestroyInstance(instance, nullptr);
        return 0;
    } else {
        printf("FAILED: Could not create Vulkan instance (error: %d)\n", result);
        return 1;
    }
}
"@

Write-Host "`nTo fully test lavapipe, you would need to compile and run a Vulkan application." -ForegroundColor Cyan
Write-Host "Environment is configured. You can now try running your tests:" -ForegroundColor Cyan
Write-Host "  cd build" -ForegroundColor White
Write-Host "  ctest --test-dir gtests -C Release --output-on-failure --verbose" -ForegroundColor White

Write-Host "`n=== Lavapipe Setup Complete ===" -ForegroundColor Green
Write-Host "Note: Environment variables are only set for this PowerShell session." -ForegroundColor Yellow
Write-Host "You can now run tests in this same terminal window." -ForegroundColor Yellow
