# Milestone A: Foundations (Boot to Triangle)

## Goal

Establish the core Vulkan rendering infrastructure with an offscreen-first architecture:
- Create a window with GLFW
- Initialize Vulkan 1.3 with RT extension probing
- Implement swapchain management
- Render to an offscreen HDR image
- Composite the offscreen image to the swapchain

## Hardware Requirements

- **GPU**: NVIDIA RTX 2000 series or newer, AMD RX 6000 series or newer
- **OS**: Windows 10/11 (64-bit)
- **Vulkan SDK**: 1.3.x with validation layers
- **Ray Tracing**: Optional for Milestone A (probed but not required)

## Build Instructions

> **Important**: This project currently targets **Windows only**. Do not build in WSL or Linux.

### Prerequisites

1. Install [Visual Studio 2022](https://visualstudio.microsoft.com/) with C++ workload
2. Install [Vulkan SDK](https://vulkan.lunarg.com/sdk/home) (1.3.x or later)
3. Install [vcpkg](https://github.com/microsoft/vcpkg)
4. Set `VCPKG_ROOT` environment variable to your vcpkg installation path:
   ```powershell
   # In PowerShell (run as Administrator for system-wide)
   [Environment]::SetEnvironmentVariable("VCPKG_ROOT", "C:\path\to\vcpkg", "User")
   
   # Or temporarily in current session
   $env:VCPKG_ROOT = "C:\path\to\vcpkg"
   ```

### Build Steps (Windows PowerShell)

```powershell
# From the project root directory in Windows PowerShell (NOT WSL)

# Verify VCPKG_ROOT is set
echo $env:VCPKG_ROOT

# Configure (Debug build with Visual Studio)
cmake --preset debug

# Build
cmake --build build/debug --config Debug

# The editor executable will be in build/debug/bin/Debug/editor.exe
```

### Build with Ninja (faster incremental builds)

If you have Ninja installed:

```powershell
cmake --preset debug-ninja
cmake --build build/debug-ninja
# Executable: build/debug-ninja/bin/editor.exe
```

### Release Build

```powershell
cmake --preset release
cmake --build build/release --config Release
```

### Common Build Errors

**"Could not find toolchain file"**
- `VCPKG_ROOT` environment variable is not set
- Run `echo $env:VCPKG_ROOT` to verify it points to your vcpkg directory

**"Preset not available on this platform"**
- You're trying to build on Linux/WSL - use Windows PowerShell instead

## Shader Compilation

Before running, compile the shaders:

```powershell
cd shaders
./compile_shaders.ps1
```

Or use CMake's shader target (builds automatically with the project).

## Running the Editor

```powershell
# From project root
cd build/debug/bin
./editor.exe
```

The editor should open a 1280x720 window displaying a gradient triangle.

### Controls

- **ESC**: Close the application

## Expected Output

On successful startup, you should see:

1. Console output showing:
   - "Lucent Engine initialized"
   - Selected GPU name and driver version
   - Ray tracing support status
   - "Renderer initialized"

2. A window displaying:
   - Dark background (near-black)
   - A gradient triangle (red-green-blue gradient)
   - Smooth 60+ FPS (with vsync) or higher

## Acceptance Criteria

### Functional Requirements

- [ ] Window opens and displays content
- [ ] Triangle renders correctly with gradient colors
- [ ] No Vulkan validation errors in debug build
- [ ] Window can be resized without crashes
- [ ] Application closes cleanly with ESC or window close button
- [ ] FPS counter shows in window title

### Technical Requirements

- [ ] Vulkan 1.3 instance created successfully
- [ ] Physical device selected with required features
- [ ] RT extensions probed (optional feature)
- [ ] Swapchain created and managed correctly
- [ ] Offscreen HDR (RGBA16F) render target created
- [ ] Depth buffer created
- [ ] Dynamic rendering (VK_KHR_dynamic_rendering) used
- [ ] Synchronization2 (VK_KHR_synchronization2) used for barriers
- [ ] Frame synchronization with fences and semaphores working

## RenderDoc Capture Instructions

### Setup

1. Install [RenderDoc](https://renderdoc.org/)
2. Launch RenderDoc
3. Go to **File > Launch Application**
4. Set **Executable Path** to: `<project>/build/debug/bin/editor.exe`
5. Set **Working Directory** to: `<project>`

### Capture a Frame

1. Click **Launch** in RenderDoc
2. The editor window should open
3. Press **F12** or **Print Screen** to capture a frame
4. Close the editor or switch back to RenderDoc
5. Double-click the capture in the "Captures collected" panel

### What to Verify in RenderDoc

1. **Pipeline State**:
   - Two draw calls (triangle + composite)
   - Correct render targets

2. **Texture Viewer**:
   - "OffscreenColor" should show HDR triangle
   - Swapchain image should show tonemapped result

3. **Event Browser**:
   - Look for "OffscreenPass" and "CompositePass" debug labels
   - No validation errors

4. **Resource Inspector**:
   - Verify buffer and image names (set via debug utils)

### Expected Capture Structure

```
Frame
├── OffscreenPass
│   ├── BeginRendering
│   ├── Draw(3, 1, 0, 0)  <- Triangle
│   └── EndRendering
└── CompositePass
    ├── BeginRendering
    ├── Draw(3, 1, 0, 0)  <- Fullscreen quad
    └── EndRendering
```

## Troubleshooting

### "Failed to create Vulkan instance"

- Ensure Vulkan SDK is installed
- Check that your GPU supports Vulkan 1.3

### "Failed to find suitable GPU"

- Update GPU drivers
- Verify GPU supports required extensions

### "Failed to load shaders"

- Run `compile_shaders.ps1` in the shaders directory
- Ensure Vulkan SDK bin is in PATH (for glslc)

### Validation Errors

- Check console output for specific error messages
- Most validation issues indicate driver or code bugs
- Report issues with the full validation message

### Black Screen

- Verify shaders compiled successfully
- Check that offscreen image is being transitioned correctly
- Use RenderDoc to inspect intermediate render targets

## Performance Notes

- Target: 60+ FPS with vsync, 1000+ FPS without
- The offscreen pass should be nearly instant (single triangle)
- The composite pass is a simple texture sample + tonemap

## Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                         Frame Loop                               │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐       │
│  │  Offscreen   │───>│   Composite  │───>│   Present    │       │
│  │    Pass      │    │     Pass     │    │              │       │
│  └──────────────┘    └──────────────┘    └──────────────┘       │
│         │                   │                   │                │
│         v                   v                   v                │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐       │
│  │ HDR RGBA16F  │    │  Swapchain   │    │   Display    │       │
│  │   Image      │    │    Image     │    │              │       │
│  └──────────────┘    └──────────────┘    └──────────────┘       │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

## Files Created in This Milestone

### Core Engine

- `engine/core/include/lucent/core/Log.h`
- `engine/core/include/lucent/core/Assert.h`
- `engine/core/include/lucent/core/Base.h`
- `engine/core/src/Log.cpp`

### Graphics Engine

- `engine/gfx/include/lucent/gfx/VulkanContext.h`
- `engine/gfx/include/lucent/gfx/Device.h`
- `engine/gfx/include/lucent/gfx/Swapchain.h`
- `engine/gfx/include/lucent/gfx/Buffer.h`
- `engine/gfx/include/lucent/gfx/Image.h`
- `engine/gfx/include/lucent/gfx/DescriptorAllocator.h`
- `engine/gfx/include/lucent/gfx/PipelineBuilder.h`
- `engine/gfx/include/lucent/gfx/Renderer.h`
- `engine/gfx/include/lucent/gfx/DebugUtils.h`
- `engine/gfx/src/*.cpp`

### Editor Application

- `app/editor/include/Application.h`
- `app/editor/src/Application.cpp`
- `app/editor/src/main.cpp`

### Shaders

- `shaders/triangle.vert`
- `shaders/triangle.frag`
- `shaders/composite.vert`
- `shaders/composite.frag`

## Next Steps (Milestone B)

- Integrate ImGui with docking support
- Create panel system (Viewport, Outliner, Inspector, Content Browser)
- Persist layout across sessions

