#include "Application.h"
#include "EditorSettings.h"
#include "lucent/gfx/DebugUtils.h"
#include "lucent/gfx/VkResultUtils.h"
#include "lucent/assets/MeshRegistry.h"
#include "lucent/scene/Components.h"
#include "lucent/material/MaterialAsset.h"
#include <GLFW/glfw3.h>
#include <cmath>

// GLFW native access (Win32 HWND)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <GLFW/glfw3native.h>
#include <Windows.h>

namespace lucent {

namespace {

struct CameraSnapshot {
    glm::vec3 position{};
    glm::vec3 target{};
    float fov = 0.0f;
    float aspect = 0.0f;
    float nearClip = 0.0f;
    float farClip = 0.0f;
};

static CameraSnapshot SnapshotCamera(const scene::EditorCamera& cam) {
    CameraSnapshot s{};
    s.position = cam.GetPosition();
    s.target = cam.GetTarget();
    s.fov = cam.GetFOV();
    s.aspect = cam.GetAspectRatio();
    s.nearClip = cam.GetNearClip();
    s.farClip = cam.GetFarClip();
    return s;
}

static bool NearlyEqual(float a, float b, float eps = 1e-6f) {
    return std::fabs(a - b) <= eps;
}

static bool NearlyEqualVec3(const glm::vec3& a, const glm::vec3& b, float eps = 1e-6f) {
    glm::vec3 d = a - b;
    return (d.x * d.x + d.y * d.y + d.z * d.z) <= (eps * eps);
}

static bool HasCameraChanged(const CameraSnapshot& prev, const scene::EditorCamera& cam) {
    // Position/target cover orbit/pan/rotate (and most focus/reset operations).
    // Projection params cover fly-mode scroll (FOV) and other projection tweaks.
    if (!NearlyEqualVec3(prev.position, cam.GetPosition())) return true;
    if (!NearlyEqualVec3(prev.target, cam.GetTarget())) return true;
    if (!NearlyEqual(prev.fov, cam.GetFOV())) return true;
    if (!NearlyEqual(prev.aspect, cam.GetAspectRatio())) return true;
    if (!NearlyEqual(prev.nearClip, cam.GetNearClip())) return true;
    if (!NearlyEqual(prev.farClip, cam.GetFarClip())) return true;
    return false;
}

} // namespace

Application::~Application() {
    Shutdown();
}

bool Application::Init(const ApplicationConfig& config) {
    m_Config = config;
    
    if (!InitWindow(config)) {
        LUCENT_CORE_ERROR("Failed to initialize window");
        return false;
    }
    
    // Initialize Vulkan context
    gfx::VulkanContextConfig vulkanConfig{};
    vulkanConfig.appName = config.title;
    vulkanConfig.enableValidation = config.enableValidation;
    vulkanConfig.enableRayTracing = true;
    
    // Preferred GPU selection (restart required to change)
    const EditorSettings settings = EditorSettings::Load();
    // Keep the string alive for the Init() call
    std::string preferredGpu = settings.preferredGpuName;
    vulkanConfig.preferredDeviceName = preferredGpu.empty() ? nullptr : preferredGpu.c_str();
    
    if (!m_VulkanContext.Init(vulkanConfig, m_Window)) {
        LUCENT_CORE_ERROR("Failed to initialize Vulkan context");
        return false;
    }
    
    // Initialize device
    if (!m_Device.Init(&m_VulkanContext)) {
        LUCENT_CORE_ERROR("Failed to initialize device");
        return false;
    }
    
    // Initialize renderer
    gfx::RendererConfig rendererConfig{};
    rendererConfig.width = config.width;
    rendererConfig.height = config.height;
    rendererConfig.vsync = config.vsync;
    
    if (!m_Renderer.Init(&m_VulkanContext, &m_Device, rendererConfig)) {
        LUCENT_CORE_ERROR("Failed to initialize renderer");
        return false;
    }
    
    // Initialize editor UI
    if (!m_EditorUI.Init(m_Window, &m_VulkanContext, &m_Device, &m_Renderer)) {
        LUCENT_CORE_ERROR("Failed to initialize editor UI");
        return false;
    }
    
    // Initialize scene with demo entities
    InitScene();
    
    // Initialize environment map (default sky)
    InitEnvironmentMap();
    
    // Connect scene and camera to UI
    m_EditorUI.SetScene(&m_Scene);
    m_EditorUI.SetEditorCamera(&m_EditorCamera);
    
    // Initialize material system
    // Set the offscreen render pass for legacy Vulkan 1.1/1.2 mode
    material::MaterialAssetManager::Get().SetRenderPass(m_Renderer.GetOffscreenRenderPass());
    
    // Use the same assets path as the content browser
    std::filesystem::path assetsPath = std::filesystem::current_path() / "Assets";
    material::MaterialAssetManager::Get().Init(&m_Device, assetsPath.string());
    
    m_Running = true;
    m_LastFrameTime = glfwGetTime();
    
    LUCENT_INFO("Application initialized successfully");
    return true;
}

void Application::CreatePrimitiveMeshes() {
    using PrimitiveType = scene::MeshRendererComponent::PrimitiveType;
    
    std::vector<assets::Vertex> vertices;
    std::vector<uint32_t> indices;
    
    // Cube
    assets::Primitives::GenerateCube(vertices, indices, 1.0f);
    auto cubeMesh = std::make_unique<assets::Mesh>();
    if (cubeMesh->Create(&m_Device, vertices, indices, "Primitive_Cube")) {
        m_PrimitiveMeshes[PrimitiveType::Cube] = std::move(cubeMesh);
    }
    
    // Sphere
    assets::Primitives::GenerateSphere(vertices, indices, 0.5f, 32, 16);
    auto sphereMesh = std::make_unique<assets::Mesh>();
    if (sphereMesh->Create(&m_Device, vertices, indices, "Primitive_Sphere")) {
        m_PrimitiveMeshes[PrimitiveType::Sphere] = std::move(sphereMesh);
    }
    
    // Plane
    assets::Primitives::GeneratePlane(vertices, indices, 1.0f, 1.0f, 1);
    auto planeMesh = std::make_unique<assets::Mesh>();
    if (planeMesh->Create(&m_Device, vertices, indices, "Primitive_Plane")) {
        m_PrimitiveMeshes[PrimitiveType::Plane] = std::move(planeMesh);
    }
    
    // Cylinder
    assets::Primitives::GenerateCylinder(vertices, indices, 0.5f, 1.0f, 32);
    auto cylinderMesh = std::make_unique<assets::Mesh>();
    if (cylinderMesh->Create(&m_Device, vertices, indices, "Primitive_Cylinder")) {
        m_PrimitiveMeshes[PrimitiveType::Cylinder] = std::move(cylinderMesh);
    }
    
    // Cone
    assets::Primitives::GenerateCone(vertices, indices, 0.5f, 1.0f, 32);
    auto coneMesh = std::make_unique<assets::Mesh>();
    if (coneMesh->Create(&m_Device, vertices, indices, "Primitive_Cone")) {
        m_PrimitiveMeshes[PrimitiveType::Cone] = std::move(coneMesh);
    }
    
    LUCENT_CORE_INFO("Created {} primitive meshes", m_PrimitiveMeshes.size());
}

void Application::UpdateEditableMeshGPU(scene::Entity entity) {
    auto* editMesh = entity.GetComponent<scene::EditableMeshComponent>();
    if (!editMesh || !editMesh->HasMesh()) return;
    
    // Get triangulated output
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> uvs;
    std::vector<glm::vec4> tangents;
    std::vector<uint32_t> indices;
    
    bool updated = editMesh->GetTriangulatedOutput(positions, normals, uvs, tangents, indices);
    
    if (!updated && m_EditableMeshGPU.count(entity.GetID()) > 0) {
        // Mesh not dirty and we already have GPU buffers
        return;
    }
    
    if (positions.empty() || indices.empty()) {
        return;
    }
    
    // Build vertex data in the format expected by assets::Mesh
    std::vector<assets::Vertex> vertices;
    vertices.reserve(positions.size());
    for (size_t i = 0; i < positions.size(); ++i) {
        assets::Vertex v;
        v.position = positions[i];
        v.normal = (i < normals.size()) ? normals[i] : glm::vec3(0, 1, 0);
        v.uv = (i < uvs.size()) ? uvs[i] : glm::vec2(0, 0);
        v.tangent = (i < tangents.size()) ? tangents[i] : glm::vec4(1, 0, 0, 1);
        vertices.push_back(v);
    }
    
    // Create or update GPU mesh
    auto& gpuMesh = m_EditableMeshGPU[entity.GetID()];
    if (!gpuMesh) {
        gpuMesh = std::make_unique<assets::Mesh>();
    }
    
    // Destroy old buffers and create new ones
    gpuMesh->Destroy();
    
    std::string meshName = "EditableMesh_" + std::to_string(entity.GetID());
    if (!gpuMesh->Create(&m_Device, vertices, indices, meshName)) {
        LUCENT_CORE_ERROR("Failed to create GPU mesh for editable mesh entity {}", entity.GetID());
        m_EditableMeshGPU.erase(entity.GetID());
    }
}

void Application::RenderMeshes(VkCommandBuffer cmd, const glm::mat4& viewProj) {
    // Get default render mode pipeline
    RenderMode mode = m_EditorUI.GetRenderMode();
    VkPipeline defaultPipeline = m_Renderer.GetSettings().enableBackfaceCulling
        ? m_Renderer.GetMeshPipeline()
        : m_Renderer.GetMeshDoubleSidedPipeline();
    VkPipelineLayout defaultLayout = m_Renderer.GetMeshPipelineLayout();
    
    if (mode == RenderMode::Wireframe && m_Renderer.GetMeshWireframePipeline()) {
        defaultPipeline = m_Renderer.GetMeshWireframePipeline();
    }
    
    // Bind shadow map descriptor set (set 0) for the default mesh pipeline only.
    // (Material pipelines have their own set 0 for textures.)
    VkDescriptorSet shadowSet = m_Renderer.GetShadowDescriptorSet();
    
    // Get camera position for specular calculations
    glm::vec3 camPos = m_EditorCamera.GetPosition();
    
    // Track currently bound pipeline for batching
    VkPipeline currentPipeline = VK_NULL_HANDLE;
    VkPipelineLayout currentLayout = VK_NULL_HANDLE;
    
    // Iterate all entities with MeshRendererComponent and TransformComponent
    auto view = m_Scene.GetView<scene::MeshRendererComponent, scene::TransformComponent>();
    view.Each([&](scene::Entity entity, scene::MeshRendererComponent& renderer, scene::TransformComponent& transform) {
        if (!renderer.visible) return;
        
        assets::Mesh* mesh = nullptr;
        
        // Check if entity has an EditableMeshComponent (use that for rendering instead)
        auto* editMesh = entity.GetComponent<scene::EditableMeshComponent>();
        if (editMesh && editMesh->HasMesh()) {
            // Update GPU mesh if dirty
            UpdateEditableMeshGPU(entity);
            
            auto it = m_EditableMeshGPU.find(entity.GetID());
            if (it != m_EditableMeshGPU.end() && it->second) {
                mesh = it->second.get();
            }
        }
        
        // Fall back to primitive or asset mesh if no editable mesh
        if (!mesh) {
            if (renderer.primitiveType != scene::MeshRendererComponent::PrimitiveType::None) {
                auto it = m_PrimitiveMeshes.find(renderer.primitiveType);
                if (it == m_PrimitiveMeshes.end() || !it->second) return;
                mesh = it->second.get();
            } else if (renderer.meshAssetID != UINT32_MAX) {
                mesh = lucent::assets::MeshRegistry::Get().GetMesh(renderer.meshAssetID);
                if (!mesh) return;
            } else {
                return;
            }
        }
        
        // Determine pipeline and layout to use
        VkPipeline pipeline = defaultPipeline;
        VkPipelineLayout layout = defaultLayout;
        bool usesMaterialPipeline = false;
        
        // Check if entity has a material asset assigned
        if (renderer.UsesMaterialAsset()) {
            auto* mat = material::MaterialAssetManager::Get().GetMaterial(renderer.materialPath);
            if (mat && mat->IsValid() && mat->GetPipeline()) {
                pipeline = mat->GetPipeline();
                layout = mat->GetPipelineLayout();
                usesMaterialPipeline = true;
            }
        }
        
        // Bind pipeline if changed
        if (pipeline != currentPipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            currentPipeline = pipeline;
            currentLayout = layout;
        }
        
        // Bind descriptor set(s)
        if (usesMaterialPipeline) {
            // Bind material texture set at set 0
            auto* mat = material::MaterialAssetManager::Get().GetMaterial(renderer.materialPath);
            if (mat && mat->HasDescriptorSet()) {
                VkDescriptorSet matSet = mat->GetDescriptorSet();
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                    0, 1, &matSet, 0, nullptr);
            }
        } else {
            // Bind shadow set for default pipeline
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, defaultLayout,
                0, 1, &shadowSet, 0, nullptr);
        }
        
        // Push constants with full material data
        struct PushConstants {
            glm::mat4 model;
            glm::mat4 viewProj;
            glm::vec4 baseColor;       // RGB + alpha
            glm::vec4 materialParams;  // metallic, roughness, emissiveIntensity, shadowBias
            glm::vec4 emissive;        // RGB + shadowEnabled
            glm::vec4 cameraPos;       // Camera world position
            glm::mat4 lightViewProj;   // Light space matrix for shadows
        } pc;
        
        pc.model = transform.GetLocalMatrix();
        pc.viewProj = viewProj;
        pc.baseColor = glm::vec4(renderer.baseColor, 1.0f);
        pc.materialParams = glm::vec4(renderer.metallic, renderer.roughness, renderer.emissiveIntensity, m_ShadowBias);
        pc.emissive = glm::vec4(renderer.emissive, m_ShadowsEnabled ? 1.0f : 0.0f);
        pc.cameraPos = glm::vec4(camPos, m_EditorUI.GetExposure());
        pc.lightViewProj = m_LightViewProj;
        
        vkCmdPushConstants(cmd, currentLayout, 
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants), &pc);
        
        mesh->Bind(cmd);
        mesh->Draw(cmd);
    });
}

void Application::InitScene() {
    // Create primitive meshes first
    CreatePrimitiveMeshes();
    
    m_Scene.SetName("Demo Scene");
    
    // Create a camera entity
    auto camera = m_Scene.CreateEntity("Main Camera");
    auto& camTransform = *camera.GetComponent<scene::TransformComponent>();
    camTransform.position = glm::vec3(5.0f, 5.0f, 5.0f);
    camTransform.rotation = glm::vec3(-30.0f, -45.0f, 0.0f);
    camera.AddComponent<scene::CameraComponent>();
    
    // Create a directional light
    auto light = m_Scene.CreateEntity("Directional Light");
    auto& lightTransform = *light.GetComponent<scene::TransformComponent>();
    lightTransform.rotation = glm::vec3(-45.0f, -45.0f, 0.0f);
    auto& lightComp = light.AddComponent<scene::LightComponent>();
    lightComp.type = scene::LightType::Directional;
    // Make the default directional light strong enough to visibly dominate the (simple) sky/environment in traced modes.
    lightComp.intensity = 10.0f;
    
    // Create some placeholder mesh entities
    // Red metallic cube
    auto cube = m_Scene.CreateEntity("Cube");
    auto& cubeRenderer = cube.AddComponent<scene::MeshRendererComponent>();
    cubeRenderer.primitiveType = scene::MeshRendererComponent::PrimitiveType::Cube;
    cubeRenderer.baseColor = glm::vec3(0.9f, 0.2f, 0.2f);
    cubeRenderer.metallic = 0.9f;
    cubeRenderer.roughness = 0.3f;
    
    // Blue plastic sphere
    auto sphere = m_Scene.CreateEntity("Sphere");
    auto& sphereTransform = *sphere.GetComponent<scene::TransformComponent>();
    sphereTransform.position = glm::vec3(3.0f, 0.0f, 0.0f);
    auto& sphereRenderer = sphere.AddComponent<scene::MeshRendererComponent>();
    sphereRenderer.primitiveType = scene::MeshRendererComponent::PrimitiveType::Sphere;
    sphereRenderer.baseColor = glm::vec3(0.2f, 0.4f, 0.9f);
    sphereRenderer.metallic = 0.0f;
    sphereRenderer.roughness = 0.4f;
    
    // Gold metallic sphere
    auto goldSphere = m_Scene.CreateEntity("Gold Sphere");
    auto& goldTransform = *goldSphere.GetComponent<scene::TransformComponent>();
    goldTransform.position = glm::vec3(-3.0f, 0.0f, 0.0f);
    auto& goldRenderer = goldSphere.AddComponent<scene::MeshRendererComponent>();
    goldRenderer.primitiveType = scene::MeshRendererComponent::PrimitiveType::Sphere;
    goldRenderer.baseColor = glm::vec3(1.0f, 0.84f, 0.0f); // Gold color
    goldRenderer.metallic = 1.0f;
    goldRenderer.roughness = 0.2f;
    
    // Rough gray ground plane
    auto plane = m_Scene.CreateEntity("Ground Plane");
    auto& planeTransform = *plane.GetComponent<scene::TransformComponent>();
    planeTransform.position = glm::vec3(0.0f, -1.0f, 0.0f);
    planeTransform.scale = glm::vec3(10.0f, 1.0f, 10.0f);
    auto& planeRenderer = plane.AddComponent<scene::MeshRendererComponent>();
    planeRenderer.primitiveType = scene::MeshRendererComponent::PrimitiveType::Plane;
    planeRenderer.baseColor = glm::vec3(0.5f, 0.5f, 0.5f);
    planeRenderer.metallic = 0.0f;
    planeRenderer.roughness = 0.9f;
    
    // Emissive cylinder
    auto cylinder = m_Scene.CreateEntity("Emissive Cylinder");
    auto& cylTransform = *cylinder.GetComponent<scene::TransformComponent>();
    cylTransform.position = glm::vec3(0.0f, 0.0f, 3.0f);
    auto& cylRenderer = cylinder.AddComponent<scene::MeshRendererComponent>();
    cylRenderer.primitiveType = scene::MeshRendererComponent::PrimitiveType::Cylinder;
    cylRenderer.baseColor = glm::vec3(0.1f, 0.1f, 0.1f);
    cylRenderer.emissive = glm::vec3(1.0f, 0.5f, 0.2f); // Orange glow
    cylRenderer.emissiveIntensity = 2.0f;
    
    // Setup editor camera
    m_EditorCamera.FocusOnPoint(glm::vec3(0.0f), 10.0f);
    
    LUCENT_CORE_INFO("Scene initialized with {} entities", m_Scene.GetEntityCount());
}

void Application::Run() {
    while (m_Running && !glfwWindowShouldClose(m_Window)) {
        // Calculate delta time
        double currentTime = glfwGetTime();
        m_DeltaTime = static_cast<float>(currentTime - m_LastFrameTime);
        m_LastFrameTime = currentTime;

        // Snapshot camera BEFORE polling events (camera is modified in GLFW callbacks).
        CameraSnapshot prevCam = SnapshotCamera(m_EditorCamera);
        
        // FPS counter
        m_FrameCount++;
        m_FpsTimer += m_DeltaTime;
        if (m_FpsTimer >= 1.0) {
            std::string title = std::string(m_Config.title) + " - " + 
                std::to_string(m_FrameCount) + " FPS";
            glfwSetWindowTitle(m_Window, title.c_str());
            m_FrameCount = 0;
            m_FpsTimer = 0.0;
        }
        
        glfwPollEvents();
        
        // Skip rendering if minimized
        int width, height;
        glfwGetFramebufferSize(m_Window, &width, &height);
        if (width == 0 || height == 0) {
            m_Minimized = true;
            continue;
        }
        m_Minimized = false;
        
        // Process input
        ProcessInput();
        
        // Update camera
        m_EditorCamera.Update(m_DeltaTime);
        
        // Check if camera has moved/changed (reset accumulation for traced modes)
        if (m_Renderer.GetRenderMode() != gfx::RenderMode::Simple && HasCameraChanged(prevCam, m_EditorCamera)) {
            m_Renderer.GetSettings().MarkDirty();
        }
        
        // Check if scene was modified in EditorUI (object transforms changed)
        if (m_EditorUI.ConsumeSceneDirty()) {
            m_TracerSceneDirty = true;
        }
        
        RenderFrame();
    }
    
    // Wait for GPU to finish before cleanup
    m_VulkanContext.WaitIdle();
}

void Application::ProcessInput() {
    // Camera keyboard input is handled via callbacks
    // Additional per-frame input processing can go here
}

void Application::Shutdown() {
    if (!m_Window) return;
    
    material::MaterialAssetManager::Get().Shutdown();
    m_EditorUI.Shutdown();
    m_Renderer.Shutdown();
    m_Device.Shutdown();
    m_VulkanContext.Shutdown();
    
    glfwDestroyWindow(m_Window);
    glfwTerminate();
    
    m_Window = nullptr;
    m_Running = false;
    
    LUCENT_INFO("Application shutdown complete");
}

bool Application::InitWindow(const ApplicationConfig& config) {
    if (!glfwInit()) {
        LUCENT_CORE_ERROR("Failed to initialize GLFW");
        return false;
    }
    
    // Don't create OpenGL context
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    
    m_Window = glfwCreateWindow(
        static_cast<int>(config.width),
        static_cast<int>(config.height),
        config.title,
        nullptr,
        nullptr
    );
    
    if (!m_Window) {
        LUCENT_CORE_ERROR("Failed to create GLFW window");
        glfwTerminate();
        return false;
    }
    
    // Set user pointer for callbacks
    glfwSetWindowUserPointer(m_Window, this);
    glfwSetFramebufferSizeCallback(m_Window, FramebufferResizeCallback);
    glfwSetKeyCallback(m_Window, KeyCallback);
    glfwSetMouseButtonCallback(m_Window, MouseButtonCallback);
    glfwSetCursorPosCallback(m_Window, CursorPosCallback);
    glfwSetScrollCallback(m_Window, ScrollCallback);
    
    // Set window icon for taskbar
    HWND hwnd = glfwGetWin32Window(m_Window);
    if (hwnd) {
        HICON hIcon = LoadIcon(GetModuleHandle(nullptr), MAKEINTRESOURCE(1));
        if (hIcon) {
            SendMessage(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIcon));
            SendMessage(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hIcon));
        }
    }
    
    LUCENT_CORE_INFO("Window created: {}x{}", config.width, config.height);
    return true;
}

void Application::RenderSceneToViewport(VkCommandBuffer cmd) {
    gfx::Image* offscreen = m_Renderer.GetOffscreenImage();
    VkExtent2D extent = { offscreen->GetWidth(), offscreen->GetHeight() };
    
    // Update camera aspect ratio based on viewport size (and reset accumulation if it changes)
    float aspectRatio = static_cast<float>(extent.width) / static_cast<float>(extent.height);
    if (!NearlyEqual(m_EditorCamera.GetAspectRatio(), aspectRatio)) {
        m_EditorCamera.SetAspectRatio(aspectRatio);
        if (m_Renderer.GetRenderMode() != gfx::RenderMode::Simple) {
            m_Renderer.GetSettings().MarkDirty();
        }
    }
    
    // Check render mode
    gfx::RenderMode renderMode = m_Renderer.GetRenderMode();
    // Keep settings mode in sync (used for convergence logic)
    m_Renderer.GetSettings().activeMode = renderMode;
    
    if (renderMode == gfx::RenderMode::Traced && m_Renderer.GetTracerCompute()) {
        // =========================================================================
        // Traced Mode: GPU compute path tracing
        // =========================================================================
        LUCENT_GPU_SCOPE(cmd, "TracedPass");
        
        // Clear offscreen to black first frame (before tracer populates it)
        gfx::RenderSettings& settings = m_Renderer.GetSettings();
        if (settings.accumulatedSamples == 0) {
            m_Renderer.BeginOffscreenPass(cmd, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
            m_Renderer.EndOffscreenPass(cmd);
        }
        
        // Render using compute tracer
        RenderTracedPath(cmd);
        
        // Copy accumulation image to offscreen for display
        gfx::TracerCompute* tracer = m_Renderer.GetTracerCompute();
        if (tracer && tracer->GetAccumulationImage() && tracer->GetAccumulationImage()->GetHandle() != VK_NULL_HANDLE) {
            // Determine which image to blit from (denoised or raw accumulation)
            gfx::Image* srcImage = tracer->GetAccumulationImage();
            
            bool skipBlit = false;
#ifdef LUCENT_ENABLE_OPTIX
            // Apply OptiX AI denoiser if enabled
            if (settings.denoiser == gfx::DenoiserType::OptiX && m_Renderer.IsOptiXDenoiserAvailable()) {
                auto* denoiser = m_Renderer.GetOptiXDenoiser();
                if (denoiser) {
                    denoiser->ResetDenoiseFlag();
                    // OptiX writes directly to offscreen, skip blit if successful
                    if (denoiser->Denoise(
                        tracer->GetAccumulationImage(),
                        tracer->GetAlbedoImage(),
                        tracer->GetNormalImage(),
                        offscreen, cmd, VK_NULL_HANDLE, VK_NULL_HANDLE
                    )) {
                        skipBlit = denoiser->WasDenoisePerformed();
                    }
                }
            }
#endif
            
            if (!skipBlit) {
                // Transition offscreen to transfer dst (from SHADER_READ_ONLY_OPTIMAL after EndOffscreenPass)
                offscreen->TransitionLayout(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                
                // Transition accumulation to transfer src
                srcImage->TransitionLayout(cmd, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                
                // Blit accumulation to offscreen
                VkImageBlit blitRegion{};
                blitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                blitRegion.srcSubresource.layerCount = 1;
                blitRegion.srcOffsets[1] = { static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height), 1 };
                blitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                blitRegion.dstSubresource.layerCount = 1;
                blitRegion.dstOffsets[1] = { static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height), 1 };
                
                vkCmdBlitImage(cmd, 
                    srcImage->GetHandle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    offscreen->GetHandle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1, &blitRegion, VK_FILTER_NEAREST);
                
                // Transition back to shader read for composite pass
                srcImage->TransitionLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
                offscreen->TransitionLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
        }
    } else if (renderMode == gfx::RenderMode::RayTraced &&
               m_Renderer.GetTracerRayKHR() &&
               m_Renderer.GetTracerRayKHR()->IsSupported()) {
        // =========================================================================
        // RayTraced Mode: Vulkan KHR ray tracing pipeline
        // =========================================================================
        LUCENT_GPU_SCOPE(cmd, "RayTracedPass");
        
        // Clear offscreen to black first frame (before tracer populates it)
        gfx::RenderSettings& settings = m_Renderer.GetSettings();
        if (settings.accumulatedSamples == 0) {
            m_Renderer.BeginOffscreenPass(cmd, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
            m_Renderer.EndOffscreenPass(cmd);
        }
        
        // Render using KHR ray tracer
        RenderRayTracedPath(cmd);
        
        // Copy accumulation image to offscreen for display
        gfx::TracerRayKHR* tracer = m_Renderer.GetTracerRayKHR();
        if (tracer && tracer->GetAccumulationImage() && tracer->GetAccumulationImage()->GetHandle() != VK_NULL_HANDLE) {
            // Determine which image to blit from (denoised or raw accumulation)
            gfx::Image* srcImage = tracer->GetAccumulationImage();
            
            bool skipBlit = false;
#ifdef LUCENT_ENABLE_OPTIX
            // Apply OptiX AI denoiser if enabled
            if (settings.denoiser == gfx::DenoiserType::OptiX && m_Renderer.IsOptiXDenoiserAvailable()) {
                auto* denoiser = m_Renderer.GetOptiXDenoiser();
                if (denoiser) {
                    denoiser->ResetDenoiseFlag();
                    // OptiX writes directly to offscreen, skip blit if successful
                    if (denoiser->Denoise(
                        tracer->GetAccumulationImage(),
                        tracer->GetAlbedoImage(),
                        tracer->GetNormalImage(),
                        offscreen, cmd, VK_NULL_HANDLE, VK_NULL_HANDLE
                    )) {
                        skipBlit = denoiser->WasDenoisePerformed();
                    }
                }
            }
#endif
            
            if (!skipBlit) {
                // Transition offscreen to transfer dst (from SHADER_READ_ONLY_OPTIMAL after EndOffscreenPass)
                offscreen->TransitionLayout(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                
                // Transition accumulation to transfer src
                srcImage->TransitionLayout(cmd, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                
                // Blit accumulation to offscreen
                VkImageBlit blitRegion{};
                blitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                blitRegion.srcSubresource.layerCount = 1;
                blitRegion.srcOffsets[1] = { static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height), 1 };
                blitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                blitRegion.dstSubresource.layerCount = 1;
                blitRegion.dstOffsets[1] = { static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height), 1 };
                
                vkCmdBlitImage(cmd, 
                    srcImage->GetHandle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    offscreen->GetHandle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1, &blitRegion, VK_FILTER_NEAREST);
                
                // Transition back to shader read for composite pass
                srcImage->TransitionLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
                offscreen->TransitionLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
        }
    } else {
        // =========================================================================
        // Simple Mode: Standard raster PBR
        // =========================================================================
        
        // Update light matrix for shadow mapping
        UpdateLightMatrix();
        
        // Update lights for rasterizer (collect scene lights)
        {
            std::vector<gfx::GPULight> lights;
            auto lightView = m_Scene.GetView<scene::LightComponent, scene::TransformComponent>();
            lightView.Each([&](scene::Entity entity, scene::LightComponent& light, scene::TransformComponent& transform) {
                (void)entity;
                
                gfx::GPULight gpuLight{};
                gpuLight.color = light.color;
                gpuLight.intensity = light.intensity;
                gpuLight.range = light.range;
                gpuLight.innerAngle = glm::radians(light.innerAngle);
                gpuLight.outerAngle = glm::radians(light.outerAngle);
                
                gpuLight.position = transform.position;
                
                // Area light properties
                gpuLight.areaWidth = light.areaWidth;
                gpuLight.areaHeight = light.areaHeight;
                gpuLight.areaShape = static_cast<uint32_t>(light.areaShape);
                gpuLight.areaTangent = transform.GetRight();
                
                // Use GetForward() for consistent rotation handling
                glm::vec3 forward = transform.GetForward();
                
                switch (light.type) {
                    case scene::LightType::Directional:
                        gpuLight.type = static_cast<uint32_t>(gfx::GPULightType::Directional);
                        // Direction FROM the light (opposite of where it points)
                        gpuLight.direction = -forward;
                        // Use shadowSoftness as angular radius for directional
                        gpuLight.areaWidth = light.shadowSoftness;
                        break;
                    case scene::LightType::Point:
                        gpuLight.type = static_cast<uint32_t>(gfx::GPULightType::Point);
                        gpuLight.direction = forward;
                        // Use shadowSoftness as point light radius
                        gpuLight.areaWidth = light.shadowSoftness;
                        break;
                    case scene::LightType::Spot:
                        gpuLight.type = static_cast<uint32_t>(gfx::GPULightType::Spot);
                        // Spot lights point in their forward direction
                        gpuLight.direction = forward;
                        gpuLight.areaWidth = light.shadowSoftness;
                        break;
                    case scene::LightType::Area:
                        gpuLight.type = static_cast<uint32_t>(gfx::GPULightType::Area);
                        gpuLight.direction = forward;  // Area normal
                        break;
                    default:
                        gpuLight.type = static_cast<uint32_t>(gfx::GPULightType::Point);
                        gpuLight.direction = forward;
                        break;
                }
                
                lights.push_back(gpuLight);
            });
            m_Renderer.SetLights(lights);
        }
        
        // Render shadow pass first
        RenderShadowPass(cmd);
        
        LUCENT_GPU_SCOPE(cmd, "ScenePass");
        
        // Begin offscreen render pass (handles transitions and viewport setup)
        m_Renderer.BeginOffscreenPass(cmd, glm::vec4(0.02f, 0.02f, 0.03f, 1.0f));
        
        // Get camera view-projection matrix
        glm::mat4 viewProj = m_EditorCamera.GetViewProjectionMatrix();
        
        // Draw skybox first (renders at far plane, no depth write)
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Renderer.GetSkyboxPipeline());
        vkCmdPushConstants(cmd, m_Renderer.GetSkyboxPipelineLayout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &viewProj);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        
        // Draw grid
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Renderer.GetGridPipeline());
        vkCmdPushConstants(cmd, m_Renderer.GetGridPipelineLayout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &viewProj);
        vkCmdDraw(cmd, 6, 1, 0, 0);
        
        // Render scene meshes
        RenderMeshes(cmd, viewProj);
        
        // End offscreen render pass
        m_Renderer.EndOffscreenPass(cmd);
    }
}

void Application::RenderFrame() {
    if (!m_Renderer.BeginFrame()) {
        return;
    }
    
    VkCommandBuffer cmd = m_Renderer.GetCurrentCommandBuffer();
    gfx::Image* offscreen = m_Renderer.GetOffscreenImage();

    // Apply any finished background material compiles on the main thread.
    material::MaterialAssetManager::Get().PumpAsyncCompiles();
    
    // =========================================================================
    // Pass 1: Render scene to offscreen image (viewport content)
    // =========================================================================
    RenderSceneToViewport(cmd);
    
    // Update viewport texture for ImGui (once per resize)
    if (!m_ViewportTextureReady) {
        m_EditorUI.SetViewportTexture(offscreen->GetView(), m_Renderer.GetOffscreenSampler());
        m_ViewportTextureReady = true;
    }
    
    // =========================================================================
    // Pass 2: Begin ImGui frame and prepare UI
    // =========================================================================
    m_EditorUI.BeginFrame();
    m_EditorUI.EndFrame();
    
    // =========================================================================
    // Pass 3: Render ImGui to swapchain
    // =========================================================================
    {
        LUCENT_GPU_SCOPE(cmd, "ImGuiPass");
        
        // Transition swapchain to render target (only needed for Vulkan 1.3 path)
        m_Renderer.TransitionSwapchainToRenderTarget(cmd);
        
        // Begin swapchain render pass (handles transitions and viewport setup)
        m_Renderer.BeginSwapchainPass(cmd, glm::vec4(0.1f, 0.1f, 0.1f, 1.0f));
        
        // Render ImGui (PostFX is applied in composite shader)
        m_EditorUI.Render(cmd);
        
        // End swapchain render pass
        m_Renderer.EndSwapchainPass(cmd);
        
        // Transition swapchain to present (only needed for Vulkan 1.3 path)
        m_Renderer.TransitionSwapchainToPresent(cmd);
    }
    
    m_Renderer.EndFrame();

    // Stop cleanly on fatal Vulkan errors (prevents infinite retry loops / driver resets)
    if (m_Renderer.HasFatalError()) {
        LUCENT_CORE_ERROR("Fatal renderer error, stopping: {} ({})",
            gfx::VkResultToString(m_Renderer.GetLastError()),
            static_cast<int>(m_Renderer.GetLastError()));
        m_Running = false;
    }
}

void Application::FramebufferResizeCallback(GLFWwindow* window, int width, int height) {
    auto* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
    if (app && width > 0 && height > 0) {
        app->m_Renderer.OnResize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
        app->m_ViewportTextureReady = false; // Need to update viewport texture
    }
}

void Application::KeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    (void)scancode;
    (void)mods;
    
    auto* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
    if (!app) return;
    
    // Let ImGui handle key input first
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureKeyboard) {
        return;
    }
    
    // Forward to editor camera
    if (action == GLFW_PRESS) {
        app->m_EditorCamera.OnKeyInput(key, true);
        
        if (key == GLFW_KEY_ESCAPE) {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
        
        // Gizmo operation shortcuts (W/E/R)
        if (key == GLFW_KEY_W && !io.WantTextInput) {
            app->m_EditorUI.SetGizmoOperation(GizmoOperation::Translate);
        }
        if (key == GLFW_KEY_E && !io.WantTextInput) {
            app->m_EditorUI.SetGizmoOperation(GizmoOperation::Rotate);
        }
        if (key == GLFW_KEY_R && !io.WantTextInput) {
            app->m_EditorUI.SetGizmoOperation(GizmoOperation::Scale);
        }
        
        // Reset camera on Home key
        if (key == GLFW_KEY_HOME) {
            app->m_EditorCamera.Reset();
        }
        
        // Toggle camera mode with F key
        if (key == GLFW_KEY_F && !io.WantTextInput) {
            auto mode = app->m_EditorCamera.GetMode();
            if (mode == scene::EditorCamera::Mode::Orbit) {
                app->m_EditorCamera.SetMode(scene::EditorCamera::Mode::Fly);
                LUCENT_CORE_DEBUG("Camera mode: Fly");
            } else {
                app->m_EditorCamera.SetMode(scene::EditorCamera::Mode::Orbit);
                LUCENT_CORE_DEBUG("Camera mode: Orbit");
            }
        }
    } else if (action == GLFW_RELEASE) {
        app->m_EditorCamera.OnKeyInput(key, false);
    }
}

void Application::MouseButtonCallback(GLFWwindow* window, int button, int action, int mods) {
    (void)mods;
    (void)button;
    (void)action;
    
    auto* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
    if (!app) return;
    
    // ImGui handles mouse through its GLFW backend
}

void Application::CursorPosCallback(GLFWwindow* window, double xpos, double ypos) {
    auto* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
    if (!app) return;
    
    // Calculate delta first (always track mouse position)
    float xOffset = static_cast<float>(xpos - app->m_LastMouseX);
    float yOffset = static_cast<float>(ypos - app->m_LastMouseY); // Standard Y (down = positive)
    
    app->m_LastMouseX = xpos;
    app->m_LastMouseY = ypos;
    
    if (app->m_FirstMouse) {
        app->m_FirstMouse = false;
        return;
    }
    
    // Only process camera input if viewport is hovered and not using gizmo
    if (!app->m_EditorUI.IsViewportHovered() || app->m_EditorUI.IsUsingGizmo()) {
        return;
    }
    
    bool leftButton = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    bool middleButton = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
    bool rightButton = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    
    if (leftButton || middleButton || rightButton) {
        app->m_EditorCamera.OnMouseMove(xOffset, yOffset, leftButton, middleButton, rightButton);
    }
}

void Application::ScrollCallback(GLFWwindow* window, double xoffset, double yoffset) {
    (void)xoffset;
    
    auto* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
    if (!app) return;
    
    // Let ImGui handle scroll first
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse && !app->m_EditorUI.IsViewportHovered()) {
        return;
    }
    
    if (app->m_EditorUI.IsViewportHovered()) {
        app->m_EditorCamera.OnMouseScroll(static_cast<float>(yoffset));
    }
}

void Application::UpdateLightMatrix() {
    // Find first directional light in scene
    glm::vec3 lightDir = glm::normalize(glm::vec3(1.0f, 1.0f, 0.5f)); // Default
    bool foundLight = false;
    
    auto lightEntities = m_Scene.GetView<scene::LightComponent, scene::TransformComponent>();
    lightEntities.Each([&](scene::Entity entity, scene::LightComponent& light, scene::TransformComponent& transform) {
        (void)entity;
        if (light.type == scene::LightType::Directional && !foundLight) {
            // Use GetForward() for consistency with light collection
            // GetForward returns the -Z axis in world space (where light points)
            // We want the direction FROM the light, so we negate it
            lightDir = -transform.GetForward();
            foundLight = true;
        }
    });
    
    // Calculate light space matrix for orthographic shadow projection
    // The light "looks at" the scene from a distance
    float shadowDistance = 30.0f;
    float shadowSize = 20.0f;
    
    glm::vec3 lightPos = -lightDir * shadowDistance;
    glm::mat4 lightViewMat = glm::lookAt(lightPos, glm::vec3(0.0f), glm::vec3(0, 1, 0));
    glm::mat4 lightProj = glm::ortho(-shadowSize, shadowSize, -shadowSize, shadowSize, 0.1f, shadowDistance * 2.0f);
    
    m_LightViewProj = lightProj * lightViewMat;
}

void Application::RenderShadowPass(VkCommandBuffer cmd) {
    if (!m_ShadowsEnabled) return;
    
    LUCENT_GPU_SCOPE(cmd, "ShadowPass");
    
    // Begin shadow render pass
    m_Renderer.BeginShadowPass(cmd);
    
    // Bind shadow pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Renderer.GetShadowPipeline());
    
    // Render all meshes to shadow map
    auto view = m_Scene.GetView<scene::MeshRendererComponent, scene::TransformComponent>();
    view.Each([&](scene::Entity entity, scene::MeshRendererComponent& renderer, scene::TransformComponent& transform) {
        (void)entity;
        
        if (!renderer.visible || !renderer.castShadows) return;
 
        assets::Mesh* mesh = nullptr;
        if (renderer.primitiveType != scene::MeshRendererComponent::PrimitiveType::None) {
            auto it = m_PrimitiveMeshes.find(renderer.primitiveType);
            if (it == m_PrimitiveMeshes.end() || !it->second) return;
            mesh = it->second.get();
        } else if (renderer.meshAssetID != UINT32_MAX) {
            mesh = lucent::assets::MeshRegistry::Get().GetMesh(renderer.meshAssetID);
            if (!mesh) return;
        } else {
            return;
        }
        
        struct ShadowPushConstants {
            glm::mat4 model;
            glm::mat4 lightViewProj;
        } pc;
        
        pc.model = transform.GetLocalMatrix();
        pc.lightViewProj = m_LightViewProj;
        
        vkCmdPushConstants(cmd, m_Renderer.GetShadowPipelineLayout(), 
            VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ShadowPushConstants), &pc);
        
        mesh->Bind(cmd);
        mesh->Draw(cmd);
    });
    
    // End shadow render pass
    m_Renderer.EndShadowPass(cmd);
}

void Application::UpdateTracerScene() {
    // Build scene data for the tracer
    std::vector<gfx::BVHBuilder::Triangle> triangles;
    std::vector<gfx::GPUMaterial> materials;
    std::vector<gfx::GPULight> lights;
    
    // Collect lights from scene
    auto lightView = m_Scene.GetView<scene::LightComponent, scene::TransformComponent>();
    lightView.Each([&](scene::Entity entity, scene::LightComponent& light, scene::TransformComponent& transform) {
        (void)entity;
        
        gfx::GPULight gpuLight{};
        gpuLight.color = light.color;
        gpuLight.intensity = light.intensity;
        gpuLight.range = light.range;
        gpuLight.innerAngle = glm::radians(light.innerAngle);
        gpuLight.outerAngle = glm::radians(light.outerAngle);
        gpuLight.position = transform.position;
        
        // Area light properties
        gpuLight.areaWidth = light.areaWidth;
        gpuLight.areaHeight = light.areaHeight;
        gpuLight.areaShape = static_cast<uint32_t>(light.areaShape);
        gpuLight.areaTangent = transform.GetRight();
        
        // Use GetForward() for consistent rotation handling
        glm::vec3 forward = transform.GetForward();
        
        switch (light.type) {
            case scene::LightType::Directional:
                gpuLight.type = static_cast<uint32_t>(gfx::GPULightType::Directional);
                // Direction FROM the light (opposite of where it points)
                gpuLight.direction = -forward;
                gpuLight.areaWidth = light.shadowSoftness;
                break;
            case scene::LightType::Point:
                gpuLight.type = static_cast<uint32_t>(gfx::GPULightType::Point);
                gpuLight.direction = forward;
                gpuLight.areaWidth = light.shadowSoftness;
                break;
            case scene::LightType::Spot:
                gpuLight.type = static_cast<uint32_t>(gfx::GPULightType::Spot);
                gpuLight.direction = forward;
                gpuLight.areaWidth = light.shadowSoftness;
                break;
            case scene::LightType::Area:
                gpuLight.type = static_cast<uint32_t>(gfx::GPULightType::Area);
                gpuLight.direction = forward;  // Area normal
                break;
        }
        
        lights.push_back(gpuLight);
    });
    
    // Default material
    gfx::GPUMaterial defaultMat{};
    defaultMat.baseColor = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
    defaultMat.emissive = glm::vec4(0.0f);
    defaultMat.metallic = 0.0f;
    defaultMat.roughness = 0.5f;
    defaultMat.ior = 1.5f;
    defaultMat.flags = 0;
    materials.push_back(defaultMat);
    
    auto view = m_Scene.GetView<scene::MeshRendererComponent, scene::TransformComponent>();
    view.Each([&](scene::Entity entity, scene::MeshRendererComponent& renderer, scene::TransformComponent& transform) {
        (void)entity;
        
        if (!renderer.visible) return;
        
        assets::Mesh* mesh = nullptr;
        if (renderer.primitiveType != scene::MeshRendererComponent::PrimitiveType::None) {
            auto it = m_PrimitiveMeshes.find(renderer.primitiveType);
            if (it == m_PrimitiveMeshes.end() || !it->second) return;
            mesh = it->second.get();
        } else if (renderer.meshAssetID != UINT32_MAX) {
            mesh = lucent::assets::MeshRegistry::Get().GetMesh(renderer.meshAssetID);
            if (!mesh) return;
        } else {
            return;
        }
        const auto& vertices = mesh->GetCPUVertices();
        const auto& indices = mesh->GetCPUIndices();
        
        if (vertices.empty() || indices.empty()) return;
        
        glm::mat4 modelMatrix = transform.GetLocalMatrix();
        glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(modelMatrix)));
        
        // Add material for this mesh
        uint32_t matId = static_cast<uint32_t>(materials.size());
        gfx::GPUMaterial mat{};
        mat.baseColor = glm::vec4(renderer.baseColor, 1.0f);
        mat.emissive = glm::vec4(renderer.emissive, renderer.emissiveIntensity);
        mat.metallic = renderer.metallic;
        mat.roughness = renderer.roughness;
        mat.ior = 1.5f;
        mat.flags = 0;
        materials.push_back(mat);
        
        // Add triangles using the Vertex struct
        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
            gfx::BVHBuilder::Triangle tri;
            
            const assets::Vertex& v0 = vertices[indices[i + 0]];
            const assets::Vertex& v1 = vertices[indices[i + 1]];
            const assets::Vertex& v2 = vertices[indices[i + 2]];
            
            // Transform positions to world space
            tri.v0 = glm::vec3(modelMatrix * glm::vec4(v0.position, 1.0f));
            tri.v1 = glm::vec3(modelMatrix * glm::vec4(v1.position, 1.0f));
            tri.v2 = glm::vec3(modelMatrix * glm::vec4(v2.position, 1.0f));
            
            // Transform normals to world space
            tri.n0 = glm::normalize(normalMatrix * v0.normal);
            tri.n1 = glm::normalize(normalMatrix * v1.normal);
            tri.n2 = glm::normalize(normalMatrix * v2.normal);
            
            tri.uv0 = v0.uv;
            tri.uv1 = v1.uv;
            tri.uv2 = v2.uv;
            
            tri.materialId = matId;
            
            triangles.push_back(tri);
        }
    });
    
    // Update the currently active tracer backend
    gfx::RenderMode mode = m_Renderer.GetRenderMode();
    if (mode == gfx::RenderMode::RayTraced) {
        if (gfx::TracerRayKHR* rt = m_Renderer.GetTracerRayKHR(); rt && rt->IsSupported()) {
            rt->UpdateScene(triangles, materials, lights);
        }
    } else {
        if (gfx::TracerCompute* compute = m_Renderer.GetTracerCompute()) {
            compute->UpdateScene(triangles, materials, lights);
        }
    }
    
    m_LastTracerLights = lights;
    m_TracerSceneDirty = false;
}

void Application::UpdateTracerLightsOnly() {
    if (m_TracerSceneDirty) return; // full update pending
    gfx::RenderMode mode = m_Renderer.GetRenderMode();
    if (mode == gfx::RenderMode::Simple) return;

    std::vector<gfx::GPULight> lights;

    auto lightView = m_Scene.GetView<scene::LightComponent, scene::TransformComponent>();
    lightView.Each([&](scene::Entity entity, scene::LightComponent& light, scene::TransformComponent& transform) {
        (void)entity;

        gfx::GPULight gpuLight{};
        gpuLight.color = light.color;
        gpuLight.intensity = light.intensity;
        gpuLight.range = light.range;
        gpuLight.innerAngle = glm::radians(light.innerAngle);
        gpuLight.outerAngle = glm::radians(light.outerAngle);
        gpuLight.position = transform.position;

        // Area light properties
        gpuLight.areaWidth = light.areaWidth;
        gpuLight.areaHeight = light.areaHeight;
        gpuLight.areaShape = static_cast<uint32_t>(light.areaShape);
        gpuLight.areaTangent = transform.GetRight();

        glm::vec3 forward = transform.GetForward();
        switch (light.type) {
            case scene::LightType::Directional:
                gpuLight.type = static_cast<uint32_t>(gfx::GPULightType::Directional);
                gpuLight.direction = -forward;
                gpuLight.areaWidth = light.shadowSoftness; // angular radius
                break;
            case scene::LightType::Point:
                gpuLight.type = static_cast<uint32_t>(gfx::GPULightType::Point);
                gpuLight.direction = forward;
                gpuLight.areaWidth = light.shadowSoftness; // radius
                break;
            case scene::LightType::Spot:
                gpuLight.type = static_cast<uint32_t>(gfx::GPULightType::Spot);
                gpuLight.direction = forward;
                gpuLight.areaWidth = light.shadowSoftness; // radius
                break;
            case scene::LightType::Area:
                gpuLight.type = static_cast<uint32_t>(gfx::GPULightType::Area);
                gpuLight.direction = forward;
                break;
        }

        lights.push_back(gpuLight);
    });

    auto lightsEqual = [&](const std::vector<gfx::GPULight>& a, const std::vector<gfx::GPULight>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++) {
            const auto& x = a[i];
            const auto& y = b[i];
            if (x.type != y.type) return false;
            if (!NearlyEqualVec3(x.position, y.position, 1e-4f)) return false;
            if (!NearlyEqualVec3(x.color, y.color, 1e-4f)) return false;
            if (!NearlyEqual(x.intensity, y.intensity, 1e-4f)) return false;
            if (!NearlyEqualVec3(x.direction, y.direction, 1e-4f)) return false;
            if (!NearlyEqual(x.range, y.range, 1e-4f)) return false;
            if (!NearlyEqual(x.innerAngle, y.innerAngle, 1e-4f)) return false;
            if (!NearlyEqual(x.outerAngle, y.outerAngle, 1e-4f)) return false;
            if (!NearlyEqual(x.areaWidth, y.areaWidth, 1e-4f)) return false;
            if (!NearlyEqual(x.areaHeight, y.areaHeight, 1e-4f)) return false;
            if (!NearlyEqualVec3(x.areaTangent, y.areaTangent, 1e-4f)) return false;
            if (x.areaShape != y.areaShape) return false;
        }
        return true;
    };

    if (lightsEqual(lights, m_LastTracerLights)) return;

    m_LastTracerLights = lights;
    m_Renderer.GetSettings().MarkDirty(); // reset accumulation on light changes

    if (mode == gfx::RenderMode::RayTraced) {
        if (gfx::TracerRayKHR* rt = m_Renderer.GetTracerRayKHR(); rt && rt->IsSupported()) {
            rt->UpdateLights(lights);
        }
    } else {
        if (gfx::TracerCompute* compute = m_Renderer.GetTracerCompute()) {
            compute->UpdateLights(lights);
        }
    }
}

void Application::RenderTracedPath(VkCommandBuffer cmd) {
    gfx::TracerCompute* tracer = m_Renderer.GetTracerCompute();
    if (!tracer) return;
    
    gfx::RenderSettings& settings = m_Renderer.GetSettings();
    
    // Check if we need to reset accumulation
    if (settings.ConsumeReset()) {
        tracer->ResetAccumulation();
    }
    
    // Check if scene needs to be updated
    if (m_TracerSceneDirty) {
        UpdateTracerScene();
    } else {
        UpdateTracerLightsOnly();
    }
    
    // Check if already converged
    if (settings.IsConverged()) {
        return; // No more samples needed
    }
    
    // Build GPU camera data
    gfx::GPUCamera gpuCamera{};
    gpuCamera.invView = glm::inverse(m_EditorCamera.GetViewMatrix());
    gpuCamera.invProj = glm::inverse(m_EditorCamera.GetProjectionMatrix());
    gpuCamera.position = m_EditorCamera.GetPosition();
    gpuCamera.fov = m_EditorCamera.GetFOV();
    
    gfx::Image* offscreen = m_Renderer.GetOffscreenImage();
    gpuCamera.resolution = glm::vec2(offscreen->GetWidth(), offscreen->GetHeight());
    gpuCamera.nearPlane = m_EditorCamera.GetNearClip();
    gpuCamera.farPlane = m_EditorCamera.GetFarClip();
    
    // Trace
    tracer->Trace(cmd, gpuCamera, settings, offscreen);
    
    // Increment sample count
    settings.IncrementSamples(1);
}

void Application::RenderRayTracedPath(VkCommandBuffer cmd) {
    gfx::TracerRayKHR* tracer = m_Renderer.GetTracerRayKHR();
    if (!tracer || !tracer->IsSupported()) return;
    
    gfx::RenderSettings& settings = m_Renderer.GetSettings();
    
    // Check if we need to reset accumulation
    if (settings.ConsumeReset()) {
        tracer->ResetAccumulation();
    }
    
    // Check if scene needs to be updated
    if (m_TracerSceneDirty) {
        UpdateTracerScene();
    } else {
        UpdateTracerLightsOnly();
    }
    
    // Check if already converged
    if (settings.IsConverged()) {
        return; // No more samples needed
    }
    
    // Build GPU camera data
    gfx::GPUCamera gpuCamera{};
    gpuCamera.invView = glm::inverse(m_EditorCamera.GetViewMatrix());
    gpuCamera.invProj = glm::inverse(m_EditorCamera.GetProjectionMatrix());
    gpuCamera.position = m_EditorCamera.GetPosition();
    gpuCamera.fov = m_EditorCamera.GetFOV();
    
    gfx::Image* offscreen = m_Renderer.GetOffscreenImage();
    gpuCamera.resolution = glm::vec2(offscreen->GetWidth(), offscreen->GetHeight());
    gpuCamera.nearPlane = m_EditorCamera.GetNearClip();
    gpuCamera.farPlane = m_EditorCamera.GetFarClip();
    
    // Trace
    tracer->Trace(cmd, gpuCamera, settings, offscreen);
    
    // Increment sample count
    settings.IncrementSamples(1);
}

void Application::InitEnvironmentMap() {
    // Create a default procedural sky environment
    if (!m_EnvironmentMap.CreateDefaultSky(&m_Device)) {
        LUCENT_CORE_WARN("Failed to create default environment map");
        return;
    }
    
    // Set the environment map on both tracers
    if (auto* tracer = m_Renderer.GetTracerCompute()) {
        tracer->SetEnvironmentMap(&m_EnvironmentMap);
    }
    if (auto* tracer = m_Renderer.GetTracerRayKHR()) {
        tracer->SetEnvironmentMap(&m_EnvironmentMap);
    }
    
    LUCENT_CORE_INFO("Environment map initialized");
}

} // namespace lucent
