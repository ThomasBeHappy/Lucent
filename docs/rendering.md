# Rendering Pipeline

Lucent supports multiple render modes that trade speed for quality. The renderer is built on Vulkan and can run either a fast raster path or progressive traced modes when supported by the GPU.

## Render Modes

- **Simple**
  - Fast raster preview for editing and layout.
- **Traced**
  - Progressive path tracing (software or hybrid pipeline, GPU dependent).
- **Ray Traced**
  - Hardware-accelerated path tracing (requires Vulkan ray tracing support).

These modes are surfaced in the **Render Properties** panel and synchronized with engine capabilities.

## Render Settings

The Render Properties panel allows tuning parameters:

- **Sampling**
  - Viewport samples (progressive accumulation).
  - Final samples (high quality output).
  - Max frame time (progressive budget).
  - Half-resolution toggle for performance.
- **Light Paths** (Traced/Ray Traced)
  - Max bounces and per-lobe bounce limits (diffuse/specular/transmission).
- **Clamping**
  - Direct and indirect clamp values to control fireflies.
- **Film**
  - Exposure, tonemapping, and gamma.
- **Denoise**
  - Box and edge-aware denoise are built-in.
  - OptiX denoise can be enabled when available.

## Material Integration

Materials are authored in the Material Graph editor as `.lmat` assets and compiled into Vulkan pipelines. The material system is responsible for:

- Graph serialization and file I/O (`Assets/materials`).
- Shader compilation from nodes to SPIR-V.
- Pipeline creation and caching.

## Progressive Updates

Traced render modes accumulate samples over time. Camera movement, edits, and material changes invalidate accumulation to keep the viewport responsive.

## Future Work

Planned rendering improvements include:

- GPU light transport optimizations.
- Expanded denoiser integrations.
- Final frame render pipeline and export.
