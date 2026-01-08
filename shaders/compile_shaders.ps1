# PowerShell script to compile all shaders
# Usage: ./compile_shaders.ps1

$VulkanSDK = $env:VULKAN_SDK
if (-not $VulkanSDK) {
    Write-Error "VULKAN_SDK environment variable not set!"
    exit 1
}

$glslc = "$VulkanSDK\Bin\glslc.exe"
if (-not (Test-Path $glslc)) {
    Write-Error "glslc.exe not found at $glslc"
    exit 1
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$outputDir = Join-Path $scriptDir "compiled"

# Create output directory if it doesn't exist
if (-not (Test-Path $outputDir)) {
    New-Item -ItemType Directory -Path $outputDir | Out-Null
}

# Shader extensions to compile
$extensions = @("*.vert", "*.frag", "*.comp", "*.rgen", "*.rmiss", "*.rchit", "*.rahit", "*.rint")

$compiled = 0
$failed = 0

foreach ($ext in $extensions) {
    $shaders = Get-ChildItem -Path $scriptDir -Filter $ext -Recurse -File
    foreach ($shader in $shaders) {
        $outputFile = Join-Path $outputDir "$($shader.Name).spv"
        Write-Host "Compiling $($shader.Name)..."
        
        & $glslc -o $outputFile $shader.FullName --target-env=vulkan1.2 -g
        
        if ($LASTEXITCODE -eq 0) {
            $compiled++
        } else {
            Write-Error "Failed to compile $($shader.Name)"
            $failed++
        }
    }
}

Write-Host ""
Write-Host "Compiled: $compiled shaders"
if ($failed -gt 0) {
    Write-Host "Failed: $failed shaders" -ForegroundColor Red
    exit 1
}

# copy the compiled shaders to the output directory for the editor
Copy-Item -Path $outputDir\* -Destination $scriptDir\..\build\debug\bin\Debug\shaders -Recurse -Force

Write-Host "Done!" -ForegroundColor Green

