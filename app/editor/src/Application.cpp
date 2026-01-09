#include "Application.h"
#include "EditorSettings.h"
#include "lucent/gfx/DebugUtils.h"
#include "lucent/gfx/VkResultUtils.h"
#include "lucent/assets/MeshRegistry.h"
#include "lucent/scene/Components.h"
#include "lucent/material/MaterialAsset.h"
#include "lucent/material/MaterialGraphEval.h"
#include "lucent/material/MaterialIR.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>
#include <optional>

// GLFW native access (Win32 HWND)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <GLFW/glfw3native.h>
#include <Windows.h>
#include <commctrl.h>

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

#ifdef _WIN32
constexpr wchar_t kSplashClassName[] = L"LucentSplashWindow";

struct SplashIconResult {
    HICON icon = nullptr;
    bool owned = false;
};

static SplashIconResult LoadBestSplashIcon(HINSTANCE instance, int desiredSize) {
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    // Try to load icon at the desired size (best for splash screen)
    HICON icon = static_cast<HICON>(LoadImageW(
        instance,
        MAKEINTRESOURCEW(1),
        IMAGE_ICON,
        desiredSize,
        desiredSize,
        LR_DEFAULTCOLOR
    ));
    if (icon) {
        return { icon, true };
    }

    // Fallback: try LoadIconMetric for a large icon
    if (SUCCEEDED(LoadIconMetric(instance, MAKEINTRESOURCEW(1), LIM_LARGE, &icon)) && icon) {
        return { icon, true };
    }

    // Last resort: standard LoadIcon
    icon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    return { icon, false };
}

LRESULT CALLBACK SplashWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            auto* create = reinterpret_cast<CREATESTRUCT*>(lParam);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rect{};
            GetClientRect(hwnd, &rect);
            HBRUSH brush = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
            FillRect(hdc, &rect, brush);
            auto iconHandle = reinterpret_cast<HICON>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
            if (iconHandle) {
                int width = rect.right - rect.left;
                int height = rect.bottom - rect.top;
                int size = std::min(width, height) - 40;
                size = std::max(size, 64);
                int x = (width - size) / 2;
                int y = (height - size) / 2;
                DrawIconEx(hdc, x, y, iconHandle, size, size, 0, nullptr, DI_NORMAL);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}
#endif

} // namespace

Application::~Application() {
    Shutdown();
}

bool Application::Init(const ApplicationConfig& config) {
    m_Config = config;

#ifdef _WIN32
    ShowSplashScreen();
#endif
    
    if (!InitWindow(config)) {
        LUCENT_CORE_ERROR("Failed to initialize window");
#ifdef _WIN32
        HideSplashScreen();
#endif
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
#ifdef _WIN32
        HideSplashScreen();
#endif
        return false;
    }
    
    // Initialize device
    if (!m_Device.Init(&m_VulkanContext)) {
        LUCENT_CORE_ERROR("Failed to initialize device");
#ifdef _WIN32
        HideSplashScreen();
#endif
        return false;
    }

    gfx::EnvironmentMapLibrary::Get().Init(&m_Device);
    
    // Initialize renderer
    gfx::RendererConfig rendererConfig{};
    rendererConfig.width = config.width;
    rendererConfig.height = config.height;
    rendererConfig.vsync = config.vsync;
    
    if (!m_Renderer.Init(&m_VulkanContext, &m_Device, rendererConfig)) {
        LUCENT_CORE_ERROR("Failed to initialize renderer");
#ifdef _WIN32
        HideSplashScreen();
#endif
        return false;
    }
    
    // Initialize editor UI
    if (!m_EditorUI.Init(m_Window, &m_VulkanContext, &m_Device, &m_Renderer)) {
        LUCENT_CORE_ERROR("Failed to initialize editor UI");
#ifdef _WIN32
        HideSplashScreen();
#endif
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

    glfwShowWindow(m_Window);
    glfwFocusWindow(m_Window);

#ifdef _WIN32
    HideSplashScreen();
#endif
    
    LUCENT_INFO("Application initialized successfully");
    return true;
}

void Application::CreatePrimitiveMeshes() {
    using PrimitiveType = scene::MeshRendererComponent::PrimitiveType;
    
    std::vector<assets::Vertex> vertices;
    std::vector<uint32_t> indices;
    
    // Cube
    assets::Primitives::GenerateCube(vertices, indices, 1.0f, /*mergedVertices=*/true);
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
    
    // Push constants structure (shared between both passes)
    struct PushConstants {
        glm::mat4 model;
        glm::mat4 viewProj;
        glm::vec4 baseColor;       // RGB + alpha
        glm::vec4 materialParams;  // metallic, roughness, emissiveIntensity, shadowBias
        glm::vec4 emissive;        // RGB + shadowEnabled
        glm::vec4 cameraPos;       // Camera world position
        glm::mat4 lightViewProj;   // Light space matrix for shadows
    };
    
    // Helper lambda to render an entity
    auto renderEntity = [&](scene::Entity entity, scene::MeshRendererComponent& renderer, 
                            scene::TransformComponent& transform, VkPipeline& currentPipeline, 
                            VkPipelineLayout& currentLayout, bool volumePass) {
        if (!renderer.visible) return;
        
        // Check if this is a volume material
        bool isVolumeMaterial = false;
        material::MaterialAsset* mat = nullptr;
        if (renderer.UsesMaterialAsset()) {
            mat = material::MaterialAssetManager::Get().GetMaterial(renderer.materialPath);
            if (mat && mat->IsValid()) {
                isVolumeMaterial = mat->IsVolumeMaterial();
            }
        }
        
        // Skip based on pass
        if (volumePass && !isVolumeMaterial) return;
        if (!volumePass && isVolumeMaterial) return;
        
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
        if (mat && mat->GetPipeline()) {
            pipeline = mat->GetPipeline();
            layout = mat->GetPipelineLayout();
            usesMaterialPipeline = true;
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
        PushConstants pc;
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
    };
    
    // Track currently bound pipeline for batching
    VkPipeline currentPipeline = VK_NULL_HANDLE;
    VkPipelineLayout currentLayout = VK_NULL_HANDLE;
    
    // PASS 1: Render opaque (surface) materials first
    auto view = m_Scene.GetView<scene::MeshRendererComponent, scene::TransformComponent>();
    view.Each([&](scene::Entity entity, scene::MeshRendererComponent& renderer, scene::TransformComponent& transform) {
        renderEntity(entity, renderer, transform, currentPipeline, currentLayout, /*volumePass=*/false);
    });
    
    // PASS 2: Render volume materials (after opaque, for correct alpha blending)
    view.Each([&](scene::Entity entity, scene::MeshRendererComponent& renderer, scene::TransformComponent& transform) {
        renderEntity(entity, renderer, transform, currentPipeline, currentLayout, /*volumePass=*/true);
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
#ifdef _WIN32
    HideSplashScreen();
#endif
    if (!m_Window) return;
    
    material::MaterialAssetManager::Get().Shutdown();
    gfx::EnvironmentMapLibrary::Get().Shutdown();
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
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    
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

#ifdef _WIN32
void Application::ShowSplashScreen() {
    if (m_SplashWindow) return;

    HINSTANCE instance = GetModuleHandle(nullptr);

    int width = 420;
    int height = 300;
    int iconSize = std::min(width, height) - 40;  // Match the size used in WM_PAINT

    SplashIconResult iconResult = LoadBestSplashIcon(instance, iconSize);
    HICON icon = iconResult.icon;

    WNDCLASSW wc{};
    wc.lpfnWndProc = SplashWindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kSplashClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    if (!RegisterClassW(&wc)) {
        DWORD error = GetLastError();
        if (error != ERROR_CLASS_ALREADY_EXISTS) {
            return;
        }
    }
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    int x = (screenWidth - width) / 2;
    int y = (screenHeight - height) / 2;

    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kSplashClassName,
        L"Lucent",
        WS_POPUP,
        x,
        y,
        width,
        height,
        nullptr,
        nullptr,
        instance,
        icon
    );

    if (!hwnd) {
        if (icon && iconResult.owned) {
            DestroyIcon(icon);
        }
        return;
    }

    if (icon) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
    }


    m_SplashWindow = hwnd;
    m_SplashIcon = icon;
    m_SplashIconOwned = iconResult.owned;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg{};
    while (PeekMessage(&msg, hwnd, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

void Application::HideSplashScreen() {
    if (m_SplashWindow) {
        DestroyWindow(reinterpret_cast<HWND>(m_SplashWindow));
        m_SplashWindow = nullptr;
    }
    if (m_SplashIcon) {
        if (m_SplashIconOwned) {
            DestroyIcon(reinterpret_cast<HICON>(m_SplashIcon));
        }
        m_SplashIcon = nullptr;
    }
    m_SplashIconOwned = false;
}
#endif

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
    UpdateEnvironmentMapFromSettings();
    
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

    if (auto* finalRender = m_Renderer.GetFinalRender();
        finalRender && finalRender->GetStatus() == gfx::FinalRenderStatus::Rendering) {
        finalRender->RenderSample();
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
    
    // Update render preview texture if final render image changed
    if (m_EditorUI.IsRenderPreviewVisible()) {
        gfx::FinalRender* finalRender = m_Renderer.GetFinalRender();
        if (finalRender) {
            gfx::Image* renderImage = finalRender->GetRenderImage();
            if (renderImage && renderImage->GetView() != VK_NULL_HANDLE) {
                // Update texture when image is available (recreate if image changed)
                static VkImageView lastRenderView = VK_NULL_HANDLE;
                if (!m_RenderPreviewTextureReady || lastRenderView != renderImage->GetView()) {
                    m_EditorUI.SetRenderPreviewTexture(renderImage->GetView(), m_Renderer.GetOffscreenSampler());
                    lastRenderView = renderImage->GetView();
                    m_RenderPreviewTextureReady = true;
                }
            }
        }
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

    // Render ImGui platform windows after the main swapchain pass.
    m_EditorUI.RenderPlatformWindows();
    
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
    
    ImGuiIO& io = ImGui::GetIO();
    
    // Check if this is a shortcut key that should work even when ImGui has focus
    bool isShortcutKey = (action == GLFW_PRESS) && (
        key == GLFW_KEY_ESCAPE ||
        key == GLFW_KEY_HOME ||
        key == GLFW_KEY_F12 ||
        key == GLFW_KEY_W ||
        key == GLFW_KEY_E ||
        key == GLFW_KEY_R ||
        key == GLFW_KEY_F
    );
    
    // For shortcut keys, only skip if ImGui is actively using text input
    // For other keys, respect ImGui's keyboard capture
    if (!isShortcutKey && io.WantCaptureKeyboard) {
        return;
    }
    
    if (isShortcutKey && io.WantTextInput) {
        // Don't process shortcuts if user is typing in a text field
        return;
    }
    
    // Forward to editor camera (always, unless ImGui wants keyboard for non-shortcuts)
    if (!io.WantCaptureKeyboard || isShortcutKey) {
        if (action == GLFW_PRESS) {
            app->m_EditorCamera.OnKeyInput(key, true);
        } else if (action == GLFW_RELEASE) {
            app->m_EditorCamera.OnKeyInput(key, false);
        }
    }
    
    // Handle shortcuts
    if (action == GLFW_PRESS) {
        if (key == GLFW_KEY_ESCAPE) {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
        
        // Gizmo operation shortcuts (W/E/R)
        if (key == GLFW_KEY_W) {
            app->m_EditorUI.SetGizmoOperation(GizmoOperation::Translate);
        }
        if (key == GLFW_KEY_E) {
            app->m_EditorUI.SetGizmoOperation(GizmoOperation::Rotate);
        }
        if (key == GLFW_KEY_R) {
            app->m_EditorUI.SetGizmoOperation(GizmoOperation::Scale);
        }
        
        // Reset camera on Home key
        if (key == GLFW_KEY_HOME) {
            app->m_EditorCamera.Reset();
        }

        // Final render from primary camera - toggle preview window
        if (key == GLFW_KEY_F12) {
            bool wasVisible = app->m_EditorUI.IsRenderPreviewVisible();
            app->m_EditorUI.ShowRenderPreview(!wasVisible);
            // Start render if not already rendering
            if (!wasVisible) {
                gfx::FinalRender* finalRender = app->m_Renderer.GetFinalRender();
                if (finalRender && finalRender->GetStatus() != gfx::FinalRenderStatus::Rendering) {
                    app->StartFinalRenderFromMainCamera();
                }
            }
        }
        
        // Toggle camera mode with F key
        if (key == GLFW_KEY_F) {
            auto mode = app->m_EditorCamera.GetMode();
            if (mode == scene::EditorCamera::Mode::Orbit) {
                app->m_EditorCamera.SetMode(scene::EditorCamera::Mode::Fly);
                LUCENT_CORE_DEBUG("Camera mode: Fly");
            } else {
                app->m_EditorCamera.SetMode(scene::EditorCamera::Mode::Orbit);
                LUCENT_CORE_DEBUG("Camera mode: Orbit");
            }
        }
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

void Application::BuildTracerSceneData(std::vector<gfx::BVHBuilder::Triangle>& triangles,
                                       std::vector<gfx::GPUMaterial>& materials,
                                       std::vector<gfx::GPULight>& lights,
                                       std::vector<gfx::GPUVolume>& volumes,
                                       std::vector<gfx::RTTextureKey>* outRTTextures,
                                       std::vector<gfx::RTMaterialHeader>* outRTHeaders,
                                       std::vector<gfx::RTMaterialInstr>* outRTInstrs) {
    triangles.clear();
    materials.clear();
    lights.clear();
    volumes.clear();

    // Optional RT material evaluation outputs (raytraced KHR backend)
    std::unordered_map<std::string, uint32_t> texKeyToIndex;
    if (outRTTextures) {
        outRTTextures->clear();
        // Index 0 reserved for "fallback" (empty path). TracerRayKHR will map it to a valid magenta texture.
        outRTTextures->push_back(gfx::RTTextureKey{ "", true });
        texKeyToIndex["S:"] = 0;
        texKeyToIndex["U:"] = 0;
    }
    if (outRTHeaders) outRTHeaders->clear();
    if (outRTInstrs) outRTInstrs->clear();

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

    if (outRTHeaders) {
        outRTHeaders->push_back(gfx::RTMaterialHeader{}); // default material has no IR
    }

    // Minimal RT material IR compiler (Surface domain only, UV + Texture2D + basic math).
    // This feeds the raytracing closest-hit interpreter in `shaders/rt_closesthit.rchit`.
    auto compileGraphToRTIR = [&](const material::MaterialGraph& graph,
                                  gfx::RTMaterialHeader& outHdr,
                                  std::vector<gfx::RTMaterialInstr>& outInstrsLocal,
                                  std::string& outErr) -> bool {
        outHdr = gfx::RTMaterialHeader{};
        outInstrsLocal.clear();
        outErr.clear();

        // V1: surface-only
        if (graph.GetDomain() == material::MaterialDomain::Volume) {
            outErr = "Volume domain not supported for RT per-hit evaluation";
            return false;
        }

        const material::NodeID outNodeId = graph.GetOutputNodeId();
        const material::MaterialNode* outNode = graph.GetNode(outNodeId);
        if (!outNode || outNode->type != material::NodeType::PBROutput) {
            outErr = "Missing PBR output node";
            return false;
        }

        auto emit = [&](uint32_t type,
                        uint32_t a = 0, uint32_t b = 0, uint32_t c = 0,
                        uint32_t texIndex = 0,
                        const glm::vec4& imm = glm::vec4(0.0f)) -> uint32_t {
            gfx::RTMaterialInstr ins{};
            ins.type = type;
            ins.a = a;
            ins.b = b;
            ins.c = c;
            ins.texIndex = texIndex;
            ins.imm = imm;
            outInstrsLocal.push_back(ins);
            return static_cast<uint32_t>(outInstrsLocal.size()); // reg = instrIndex+1 => size
        };

        auto emitConstFromValue = [&](const material::PinValue& v) -> uint32_t {
            if (std::holds_alternative<float>(v)) {
                return emit(1u, 0, 0, 0, 0, glm::vec4(std::get<float>(v), 0.0f, 0.0f, 0.0f));
            }
            if (std::holds_alternative<glm::vec2>(v)) {
                auto vv = std::get<glm::vec2>(v);
                return emit(1u, 0, 0, 0, 0, glm::vec4(vv.x, vv.y, 0.0f, 0.0f));
            }
            if (std::holds_alternative<glm::vec3>(v)) {
                auto vv = std::get<glm::vec3>(v);
                return emit(1u, 0, 0, 0, 0, glm::vec4(vv, 1.0f));
            }
            if (std::holds_alternative<glm::vec4>(v)) {
                return emit(1u, 0, 0, 0, 0, std::get<glm::vec4>(v));
            }
            // String / other: not constant-evaluable here
            return emit(1u, 0, 0, 0, 0, glm::vec4(0.0f));
        };

        // Cache per-pin output register
        std::unordered_map<material::PinID, uint32_t> pinToReg;
        std::unordered_map<material::PinID, uint8_t> state; // 0=unvisited, 1=visiting, 2=done

        std::function<uint32_t(material::PinID)> compilePin;
        compilePin = [&](material::PinID pinId) -> uint32_t {
            if (pinId == material::INVALID_PIN_ID) return 0u;

            auto itCached = pinToReg.find(pinId);
            if (itCached != pinToReg.end()) return itCached->second;

            uint8_t& st = state[pinId];
            if (st == 1) {
                // cycle
                return 0u;
            }
            st = 1;

            const material::MaterialPin* pin = graph.GetPin(pinId);
            if (!pin) { st = 2; return 0u; }

            // Input pins resolve to their connected source, or default value
            if (pin->direction == material::PinDirection::Input) {
                material::LinkID linkId = graph.FindLinkByEndPin(pinId);
                if (linkId != material::INVALID_LINK_ID) {
                    const material::MaterialLink* link = graph.GetLink(linkId);
                    if (link) {
                        uint32_t r = compilePin(link->startPinId);
                        pinToReg[pinId] = r;
                        st = 2;
                        return r;
                    }
                }
                uint32_t r = emitConstFromValue(pin->defaultValue);
                pinToReg[pinId] = r;
                st = 2;
                return r;
            }

            const material::MaterialNode* node = graph.GetNode(pin->nodeId);
            if (!node) { st = 2; return 0u; }

            // Determine which output index this pin is
            int outIdx = -1;
            for (int i = 0; i < (int)node->outputPins.size(); ++i) {
                if (node->outputPins[i] == pinId) { outIdx = i; break; }
            }

            uint32_t r = 0u;
            switch (node->type) {
                case material::NodeType::ConstFloat:
                case material::NodeType::ConstVec2:
                case material::NodeType::ConstVec3:
                case material::NodeType::ConstVec4:
                    r = emitConstFromValue(node->parameter);
                    break;

                case material::NodeType::UV:
                    r = emit(2u);
                    break;

                case material::NodeType::Texture2D:
                case material::NodeType::NormalMap: {
                    uint32_t uvReg = 0u;
                    if (!node->inputPins.empty()) {
                        // If UV is unconnected, leave uvReg = 0 -> shader defaults to mesh UV input
                        material::LinkID linkId = graph.FindLinkByEndPin(node->inputPins[0]);
                        if (linkId != material::INVALID_LINK_ID) {
                            uvReg = compilePin(node->inputPins[0]);
                        }
                    }

                    std::string path;
                    if (std::holds_alternative<std::string>(node->parameter)) path = std::get<std::string>(node->parameter);

                    bool sRGB = (node->type == material::NodeType::Texture2D);
                    if (!path.empty()) {
                        const auto& slots = graph.GetTextureSlots();
                        for (const auto& slot : slots) {
                            if (slot.path == path) { sRGB = slot.sRGB; break; }
                        }
                    }

                    uint32_t texIndex = 0u;
                    if (outRTTextures) {
                        const std::string key = std::string(sRGB ? "S:" : "U:") + path;
                        auto it = texKeyToIndex.find(key);
                        if (it != texKeyToIndex.end()) {
                            texIndex = it->second;
                        } else {
                            // Reserve index 0 for fallback; array size is 256.
                            if (outRTTextures->size() < 256) {
                                texIndex = static_cast<uint32_t>(outRTTextures->size());
                                outRTTextures->push_back(gfx::RTTextureKey{ path, sRGB });
                                texKeyToIndex[key] = texIndex;
                            } else {
                                texIndex = 0u;
                            }
                        }
                    }

                    uint32_t sampleReg = emit(3u, uvReg, 0u, 0u, texIndex);

                    // outputs: 0=RGB, 1=R, 2=G, 3=B, 4=A (see MaterialGraph SetupNodePins)
                    uint32_t swz = 4u;
                    if (outIdx == 1) swz = 0u;
                    else if (outIdx == 2) swz = 1u;
                    else if (outIdx == 3) swz = 2u;
                    else if (outIdx == 4) swz = 3u;
                    r = emit(10u, sampleReg, 0u, 0u, swz);
                    break;
                }

                case material::NodeType::Add:
                    r = emit(4u, compilePin(node->inputPins[0]), compilePin(node->inputPins[1]));
                    break;

                case material::NodeType::Multiply:
                    r = emit(5u, compilePin(node->inputPins[0]), compilePin(node->inputPins[1]));
                    break;

                case material::NodeType::Lerp:
                    r = emit(6u, compilePin(node->inputPins[0]), compilePin(node->inputPins[1]), compilePin(node->inputPins[2]));
                    break;

                case material::NodeType::Clamp:
                    r = emit(7u, compilePin(node->inputPins[0]), compilePin(node->inputPins[1]), compilePin(node->inputPins[2]));
                    break;

                case material::NodeType::Saturate:
                    r = emit(8u, compilePin(node->inputPins[0]));
                    break;

                case material::NodeType::OneMinus:
                    r = emit(9u, compilePin(node->inputPins[0]));
                    break;

                case material::NodeType::SeparateVec3: {
                    uint32_t v = compilePin(node->inputPins[0]);
                    uint32_t swz = (outIdx == 1) ? 1u : (outIdx == 2) ? 2u : 0u;
                    r = emit(10u, v, 0u, 0u, swz);
                    break;
                }

                case material::NodeType::CombineVec3:
                    r = emit(11u, compilePin(node->inputPins[0]), compilePin(node->inputPins[1]), compilePin(node->inputPins[2]));
                    break;

                case material::NodeType::FloatToVec3: {
                    uint32_t f = compilePin(node->inputPins[0]);
                    r = emit(11u, f, f, f);
                    break;
                }

                case material::NodeType::Vec3ToFloat: {
                    uint32_t v = compilePin(node->inputPins[0]);
                    r = emit(10u, v, 0u, 0u, 0u);
                    break;
                }

                case material::NodeType::Vec4ToVec3: {
                    uint32_t v = compilePin(node->inputPins[0]);
                    r = emit(10u, v, 0u, 0u, 4u);
                    break;
                }

                case material::NodeType::Reroute:
                    r = compilePin(node->inputPins[0]);
                    break;

                default:
                    // Unsupported for this minimal RT interpreter (yet)
                    outErr = "Unsupported node for RT per-hit eval";
                    r = emit(1u, 0, 0, 0, 0, glm::vec4(0.0f));
                    break;
            }

            pinToReg[pinId] = r;
            st = 2;
            return r;
        };

        // Find PBR output pins by name (compile from inputs so defaults work)
        material::PinID baseColorIn = material::INVALID_PIN_ID;
        material::PinID metallicIn = material::INVALID_PIN_ID;
        material::PinID roughnessIn = material::INVALID_PIN_ID;
        material::PinID emissiveIn = material::INVALID_PIN_ID;
        material::PinID normalIn = material::INVALID_PIN_ID;

        for (material::PinID pid : outNode->inputPins) {
            const material::MaterialPin* p = graph.GetPin(pid);
            if (!p) continue;
            if (p->name == "Base Color") baseColorIn = pid;
            else if (p->name == "Metallic") metallicIn = pid;
            else if (p->name == "Roughness") roughnessIn = pid;
            else if (p->name == "Emissive") emissiveIn = pid;
            else if (p->name == "Normal") normalIn = pid;
        }

        outHdr.baseColorReg = compilePin(baseColorIn);
        outHdr.metallicReg = compilePin(metallicIn);
        outHdr.roughnessReg = compilePin(roughnessIn);
        outHdr.emissiveReg = compilePin(emissiveIn);
        outHdr.normalReg = compilePin(normalIn);

        // Clamp instruction count to shader interpreter limit
        if (outInstrsLocal.size() > 128) {
            outErr = "Material graph too complex for RT interpreter (instr limit)";
            return false;
        }

        outHdr.instrCount = static_cast<uint32_t>(outInstrsLocal.size());
        return true;
    };

    auto view = m_Scene.GetView<scene::MeshRendererComponent, scene::TransformComponent>();
    view.Each([&](scene::Entity entity, scene::MeshRendererComponent& renderer, scene::TransformComponent& transform) {
        (void)entity;

        if (!renderer.visible) return;

        // Prefer editable mesh topology when present (Edit Mode / converted primitives).
        // Tracers operate on triangles, so we triangulate ngons here.
        std::vector<assets::Vertex> tempVertices;
        std::vector<uint32_t> tempIndices;
        const std::vector<assets::Vertex>* verticesPtr = nullptr;
        const std::vector<uint32_t>* indicesPtr = nullptr;

        if (auto* editMesh = entity.GetComponent<scene::EditableMeshComponent>(); editMesh && editMesh->HasMesh()) {
            auto triOut = editMesh->mesh->ToTriangles();
            if (!triOut.vertices.empty() && !triOut.indices.empty()) {
                tempVertices.reserve(triOut.vertices.size());
                for (const auto& v : triOut.vertices) {
                    assets::Vertex av{};
                    av.position = v.position;
                    av.normal = v.normal;
                    av.uv = v.uv;
                    av.tangent = v.tangent;
                    tempVertices.push_back(av);
                }
                tempIndices = std::move(triOut.indices);
                verticesPtr = &tempVertices;
                indicesPtr = &tempIndices;
            }
        }

        assets::Mesh* mesh = nullptr;
        if (!verticesPtr || !indicesPtr) {
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

            verticesPtr = &mesh->GetCPUVertices();
            indicesPtr = &mesh->GetCPUIndices();
        }

        const auto& vertices = *verticesPtr;
        const auto& indices = *indicesPtr;

        if (vertices.empty() || indices.empty()) return;

        glm::mat4 modelMatrix = transform.GetLocalMatrix();
        glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(modelMatrix)));

        // Resolve material asset (if any) once per entity
        material::MaterialAsset* matAsset = nullptr;
        if (renderer.UsesMaterialAsset()) {
            matAsset = material::MaterialAssetManager::Get().GetMaterial(renderer.materialPath);
        }

        // If this mesh uses a volume material, add a volume instance and SKIP surface triangles
        if (matAsset && matAsset->IsValid() && matAsset->IsVolumeMaterial()) {
            gfx::GPUVolume vol{};
            vol.transform = glm::inverse(modelMatrix);

            // Pull volume parameters from the VolumetricOutput inputs.
            // We prefer evaluating connected constant/math subgraphs so users can drive density, etc.,
            // and fall back to pin defaults when the graph isn't constant-evaluable.
            const auto& graph = matAsset->GetGraph();
            const auto* volNode = graph.GetNode(graph.GetVolumeOutputNodeId());
            if (volNode && volNode->type == material::NodeType::VolumetricOutput) {
                // Helpers to read defaults
                auto getFloatDefault = [&](material::PinID pinId, float fallback) -> float {
                    const auto* pin = graph.GetPin(pinId);
                    if (!pin) return fallback;
                    if (std::holds_alternative<float>(pin->defaultValue)) return std::get<float>(pin->defaultValue);
                    return fallback;
                };
                auto getVec3Default = [&](material::PinID pinId, glm::vec3 fallback) -> glm::vec3 {
                    const auto* pin = graph.GetPin(pinId);
                    if (!pin) return fallback;
                    if (std::holds_alternative<glm::vec3>(pin->defaultValue)) return std::get<glm::vec3>(pin->defaultValue);
                    if (std::holds_alternative<glm::vec4>(pin->defaultValue)) return glm::vec3(std::get<glm::vec4>(pin->defaultValue));
                    return fallback;
                };

                // Constant evaluation for traced volumes (supports const + simple math chains)
                std::function<std::optional<float>(material::PinID)> evalFloat;
                std::function<std::optional<glm::vec3>(material::PinID)> evalVec3;
                std::unordered_set<material::PinID> visitingF;
                std::unordered_set<material::PinID> visitingV3;

                auto evalInputLink = [&](material::PinID inputPin) -> material::PinID {
                    material::LinkID linkId = graph.FindLinkByEndPin(inputPin);
                    if (linkId != material::INVALID_LINK_ID) {
                        const auto* link = graph.GetLink(linkId);
                        if (link) return link->startPinId;
                    }
                    return material::INVALID_PIN_ID;
                };

                evalFloat = [&](material::PinID pinId) -> std::optional<float> {
                    if (pinId == material::INVALID_PIN_ID) return std::nullopt;
                    if (visitingF.count(pinId)) return std::nullopt;
                    visitingF.insert(pinId);
                    const auto* pin = graph.GetPin(pinId);
                    if (!pin) { visitingF.erase(pinId); return std::nullopt; }

                    if (pin->direction == material::PinDirection::Input) {
                        material::PinID src = evalInputLink(pinId);
                        if (src != material::INVALID_PIN_ID) {
                            auto v = evalFloat(src);
                            visitingF.erase(pinId);
                            return v;
                        }
                        // Default
                        float v = 0.0f;
                        if (std::holds_alternative<float>(pin->defaultValue)) v = std::get<float>(pin->defaultValue);
                        else if (std::holds_alternative<glm::vec3>(pin->defaultValue)) v = std::get<glm::vec3>(pin->defaultValue).x;
                        else if (std::holds_alternative<glm::vec4>(pin->defaultValue)) v = std::get<glm::vec4>(pin->defaultValue).x;
                        visitingF.erase(pinId);
                        return v;
                    }

                    const auto* node = graph.GetNode(pin->nodeId);
                    if (!node) { visitingF.erase(pinId); return std::nullopt; }

                    // Determine output index when needed
                    auto outIndex = [&]() -> int {
                        for (int i = 0; i < (int)node->outputPins.size(); ++i) {
                            if (node->outputPins[i] == pinId) return i;
                        }
                        return -1;
                    };

                    std::optional<float> result;
                    switch (node->type) {
                        case material::NodeType::ConstFloat:
                            if (std::holds_alternative<float>(node->parameter)) result = std::get<float>(node->parameter);
                            break;
                        case material::NodeType::Vec3ToFloat: {
                            auto v3 = evalVec3(node->inputPins[0]);
                            if (v3) result = v3->x;
                            break;
                        }
                        case material::NodeType::SeparateVec3: {
                            auto v3 = evalVec3(node->inputPins[0]);
                            int idx = outIndex();
                            if (v3 && idx >= 0 && idx < 3) {
                                result = (idx == 0) ? v3->x : (idx == 1) ? v3->y : v3->z;
                            }
                            break;
                        }
                        case material::NodeType::Clamp: {
                            auto v = evalFloat(node->inputPins[0]);
                            auto mi = evalFloat(node->inputPins[1]);
                            auto ma = evalFloat(node->inputPins[2]);
                            if (v && mi && ma) result = std::clamp(*v, *mi, *ma);
                            break;
                        }
                        case material::NodeType::OneMinus: {
                            auto v = evalFloat(node->inputPins[0]);
                            if (v) result = 1.0f - *v;
                            break;
                        }
                        case material::NodeType::Abs: {
                            auto v = evalFloat(node->inputPins[0]);
                            if (v) result = std::fabs(*v);
                            break;
                        }
                        case material::NodeType::Sin: {
                            auto v = evalFloat(node->inputPins[0]);
                            if (v) result = std::sin(*v);
                            break;
                        }
                        case material::NodeType::Cos: {
                            auto v = evalFloat(node->inputPins[0]);
                            if (v) result = std::cos(*v);
                            break;
                        }
                        case material::NodeType::Power: {
                            auto a = evalFloat(node->inputPins[0]);
                            auto b = evalFloat(node->inputPins[1]);
                            if (a && b) result = std::pow(*a, *b);
                            break;
                        }
                        case material::NodeType::Remap: {
                            auto v = evalFloat(node->inputPins[0]);
                            auto inMin = evalFloat(node->inputPins[1]);
                            auto inMax = evalFloat(node->inputPins[2]);
                            auto outMin = evalFloat(node->inputPins[3]);
                            auto outMax = evalFloat(node->inputPins[4]);
                            if (v && inMin && inMax && outMin && outMax) {
                                float denom = std::max(*inMax - *inMin, 1e-6f);
                                float t = std::clamp((*v - *inMin) / denom, 0.0f, 1.0f);
                                result = *outMin + t * (*outMax - *outMin);
                            }
                            break;
                        }
                        case material::NodeType::Step: {
                            auto edge = evalFloat(node->inputPins[0]);
                            auto x = evalFloat(node->inputPins[1]);
                            if (edge && x) result = (*x >= *edge) ? 1.0f : 0.0f;
                            break;
                        }
                        case material::NodeType::Smoothstep: {
                            auto e0 = evalFloat(node->inputPins[0]);
                            auto e1 = evalFloat(node->inputPins[1]);
                            auto x = evalFloat(node->inputPins[2]);
                            if (e0 && e1 && x) {
                                float t = std::clamp((*x - *e0) / std::max(*e1 - *e0, 1e-6f), 0.0f, 1.0f);
                                result = t * t * (3.0f - 2.0f * t);
                            }
                            break;
                        }
                        case material::NodeType::Reroute: {
                            auto v = evalFloat(node->inputPins[0]);
                            if (v) result = *v;
                            break;
                        }
                        default:
                            break;
                    }

                    visitingF.erase(pinId);
                    return result;
                };

                evalVec3 = [&](material::PinID pinId) -> std::optional<glm::vec3> {
                    if (pinId == material::INVALID_PIN_ID) return std::nullopt;
                    if (visitingV3.count(pinId)) return std::nullopt;
                    visitingV3.insert(pinId);
                    const auto* pin = graph.GetPin(pinId);
                    if (!pin) { visitingV3.erase(pinId); return std::nullopt; }

                    if (pin->direction == material::PinDirection::Input) {
                        material::PinID src = evalInputLink(pinId);
                        if (src != material::INVALID_PIN_ID) {
                            auto v = evalVec3(src);
                            visitingV3.erase(pinId);
                            return v;
                        }
                        glm::vec3 v = glm::vec3(0.0f);
                        if (std::holds_alternative<glm::vec3>(pin->defaultValue)) v = std::get<glm::vec3>(pin->defaultValue);
                        else if (std::holds_alternative<glm::vec4>(pin->defaultValue)) v = glm::vec3(std::get<glm::vec4>(pin->defaultValue));
                        else if (std::holds_alternative<float>(pin->defaultValue)) v = glm::vec3(std::get<float>(pin->defaultValue));
                        visitingV3.erase(pinId);
                        return v;
                    }

                    const auto* node = graph.GetNode(pin->nodeId);
                    if (!node) { visitingV3.erase(pinId); return std::nullopt; }

                    std::optional<glm::vec3> result;
                    switch (node->type) {
                        case material::NodeType::ConstVec3:
                            if (std::holds_alternative<glm::vec3>(node->parameter)) result = std::get<glm::vec3>(node->parameter);
                            break;
                        case material::NodeType::ConstVec4:
                            if (std::holds_alternative<glm::vec4>(node->parameter)) result = glm::vec3(std::get<glm::vec4>(node->parameter));
                            break;
                        case material::NodeType::FloatToVec3: {
                            auto v = evalFloat(node->inputPins[0]);
                            if (v) result = glm::vec3(*v);
                            break;
                        }
                        case material::NodeType::Vec4ToVec3: {
                            // Not directly evaluatable here; fall back to defaults
                            break;
                        }
                        case material::NodeType::Add:
                        case material::NodeType::Subtract:
                        case material::NodeType::Multiply:
                        case material::NodeType::Divide: {
                            auto a = evalVec3(node->inputPins[0]);
                            auto b = evalVec3(node->inputPins[1]);
                            if (a && b) {
                                if (node->type == material::NodeType::Add) result = *a + *b;
                                else if (node->type == material::NodeType::Subtract) result = *a - *b;
                                else if (node->type == material::NodeType::Multiply) result = *a * *b;
                                else result = glm::vec3(
                                    a->x / std::max(b->x, 1e-6f),
                                    a->y / std::max(b->y, 1e-6f),
                                    a->z / std::max(b->z, 1e-6f)
                                );
                            }
                            break;
                        }
                        case material::NodeType::Lerp: {
                            auto a = evalVec3(node->inputPins[0]);
                            auto b = evalVec3(node->inputPins[1]);
                            auto t = evalFloat(node->inputPins[2]);
                            if (a && b && t) result = (*a) * (1.0f - *t) + (*b) * (*t);
                            break;
                        }
                        case material::NodeType::CombineVec3: {
                            auto r = evalFloat(node->inputPins[0]);
                            auto g = evalFloat(node->inputPins[1]);
                            auto b = evalFloat(node->inputPins[2]);
                            if (r && g && b) result = glm::vec3(*r, *g, *b);
                            break;
                        }
                        case material::NodeType::Reroute: {
                            auto v = evalVec3(node->inputPins[0]);
                            if (v) result = *v;
                            break;
                        }
                        default:
                            break;
                    }

                    visitingV3.erase(pinId);
                    return result;
                };

                auto getFloat = [&](size_t pinIdx, float fallback) -> float {
                    if (pinIdx >= volNode->inputPins.size()) return fallback;
                    material::PinID pinId = volNode->inputPins[pinIdx];
                    if (auto v = evalFloat(pinId)) return *v;
                    return getFloatDefault(pinId, fallback);
                };
                auto getVec3 = [&](size_t pinIdx, glm::vec3 fallback) -> glm::vec3 {
                    if (pinIdx >= volNode->inputPins.size()) return fallback;
                    material::PinID pinId = volNode->inputPins[pinIdx];
                    if (auto v = evalVec3(pinId)) return *v;
                    return getVec3Default(pinId, fallback);
                };

                vol.scatterColor = getVec3(0, glm::vec3(0.8f));
                vol.density = getFloat(1, 1.0f);
                vol.anisotropy = getFloat(2, 0.0f);
                vol.absorption = getVec3(3, glm::vec3(0.0f));
                vol.emission = getVec3(4, glm::vec3(0.0f));
                vol.emissionStrength = getFloat(5, 1.0f);
            } else {
                vol.scatterColor = glm::vec3(renderer.baseColor);
                vol.density = 1.0f;
                vol.anisotropy = 0.0f;
                vol.absorption = glm::vec3(0.0f);
                vol.emission = glm::vec3(renderer.emissive);
                vol.emissionStrength = renderer.emissiveIntensity;
            }

            // Compute world-space AABB from mesh vertices (V1)
            glm::vec3 aabbMin(FLT_MAX);
            glm::vec3 aabbMax(-FLT_MAX);
            for (const auto& vtx : vertices) {
                glm::vec3 wp = glm::vec3(modelMatrix * glm::vec4(vtx.position, 1.0f));
                aabbMin = glm::min(aabbMin, wp);
                aabbMax = glm::max(aabbMax, wp);
            }
            vol.aabbMin = aabbMin;
            vol.aabbMax = aabbMax;

            volumes.push_back(vol);
            return; // IMPORTANT: don't also add surface triangles/material for volume containers
        }

        // Add material for this mesh
        uint32_t matId = static_cast<uint32_t>(materials.size());
        gfx::GPUMaterial mat{};

        // Traced material pipeline:
        // If the entity uses a MaterialAsset, evaluate constant channels for the tracer backends (V1).
        if (matAsset && matAsset->IsValid()) {
            material::TracerMaterialConstants baked{};
            std::string bakeErr;
            if (material::EvaluateTracerConstants(matAsset->GetGraph(), baked, bakeErr)) {
                mat.baseColor = baked.baseColor;
                mat.emissive = baked.emissive;
                mat.metallic = baked.metallic;
                mat.roughness = baked.roughness;
                mat.ior = baked.ior;
                mat.flags = baked.flags;
            } else {
                // If evaluation fails (unsupported nodes), fallback to IR constant evaluation,
                // then to component values.
                material::MaterialIR ir{};
                std::string irErr;
                if (material::MaterialIRCompiler::Compile(matAsset->GetGraph(), ir, irErr) && ir.IsValid()) {
                    auto data = ir.EvaluateConstant();
                    mat.baseColor = data.baseColor;
                    mat.emissive = data.emissive;
                    mat.metallic = data.metallic;
                    mat.roughness = data.roughness;
                    mat.ior = data.ior;
                    mat.flags = data.flags;
                } else {
                    mat.baseColor = glm::vec4(renderer.baseColor, 1.0f);
                    mat.emissive = glm::vec4(renderer.emissive, renderer.emissiveIntensity);
                    mat.metallic = renderer.metallic;
                    mat.roughness = renderer.roughness;
                    mat.ior = 1.5f;
                    mat.flags = 0;
                }
            }
        } else {
            // No material asset: use component values
            mat.baseColor = glm::vec4(renderer.baseColor, 1.0f);
            mat.emissive = glm::vec4(renderer.emissive, renderer.emissiveIntensity);
            mat.metallic = renderer.metallic;
            mat.roughness = renderer.roughness;
            mat.ior = 1.5f;
            mat.flags = 0;
        }

        materials.push_back(mat);

        // Optional RT per-hit material evaluation (UV-driven)
        if (outRTHeaders) {
            gfx::RTMaterialHeader hdr{};

            if (outRTInstrs && outRTTextures && matAsset && matAsset->IsValid() && !matAsset->IsVolumeMaterial()) {
                std::vector<gfx::RTMaterialInstr> localInstrs;
                std::string irErr;
                if (compileGraphToRTIR(matAsset->GetGraph(), hdr, localInstrs, irErr) && hdr.instrCount > 0) {
                    hdr.instrOffset = static_cast<uint32_t>(outRTInstrs->size());
                    outRTInstrs->insert(outRTInstrs->end(), localInstrs.begin(), localInstrs.end());
                } else {
                    // Leave hdr empty; constants buffer will be used
                    (void)irErr;
                    hdr = gfx::RTMaterialHeader{};
                }
            }

            outRTHeaders->push_back(hdr);
        }

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
}

void Application::UpdateTracerScene() {
    // Build scene data for the tracer
    std::vector<gfx::BVHBuilder::Triangle> triangles;
    std::vector<gfx::GPUMaterial> materials;
    std::vector<gfx::GPULight> lights;
    std::vector<gfx::GPUVolume> volumes;

    // Optional RT per-hit material evaluation data (only used by RayTraced backend)
    std::vector<gfx::RTTextureKey> rtTextures;
    std::vector<gfx::RTMaterialHeader> rtHeaders;
    std::vector<gfx::RTMaterialInstr> rtInstrs;

    // Update the currently active tracer backend
    gfx::RenderMode mode = m_Renderer.GetRenderMode();
    if (mode == gfx::RenderMode::RayTraced) {
        BuildTracerSceneData(triangles, materials, lights, volumes, &rtTextures, &rtHeaders, &rtInstrs);
        if (gfx::TracerRayKHR* rt = m_Renderer.GetTracerRayKHR(); rt && rt->IsSupported()) {
            rt->UpdateScene(triangles, materials, rtTextures, rtHeaders, rtInstrs, lights, volumes);
        }
    } else {
        BuildTracerSceneData(triangles, materials, lights, volumes);
        if (gfx::TracerCompute* compute = m_Renderer.GetTracerCompute()) {
            compute->UpdateScene(triangles, materials, lights, volumes);
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

void Application::StartFinalRenderFromMainCamera() {
    gfx::FinalRender* finalRender = m_Renderer.GetFinalRender();
    if (!finalRender) {
        LUCENT_CORE_WARN("Final render is not available");
        return;
    }

    if (finalRender->GetStatus() == gfx::FinalRenderStatus::Rendering) {
        LUCENT_CORE_WARN("Final render already in progress");
        return;
    }

    scene::Entity cameraEntity = m_Scene.GetPrimaryCamera();
    if (!cameraEntity.IsValid()) {
        LUCENT_CORE_WARN("Final render aborted: no primary camera found");
        return;
    }

    auto* camera = cameraEntity.GetComponent<scene::CameraComponent>();
    auto* transform = cameraEntity.GetComponent<scene::TransformComponent>();
    if (!camera || !transform) {
        LUCENT_CORE_WARN("Final render aborted: primary camera missing components");
        return;
    }

    gfx::RenderSettings& settings = m_Renderer.GetSettings();
    uint32_t width = std::max(settings.renderWidth, 16u);
    uint32_t height = std::max(settings.renderHeight, 16u);
    float aspect = static_cast<float>(width) / static_cast<float>(height);

    glm::mat4 view = glm::lookAt(transform->position,
                                 transform->position + transform->GetForward(),
                                 transform->GetUp());
    glm::mat4 proj = camera->GetProjection(aspect);

    gfx::GPUCamera gpuCamera{};
    gpuCamera.invView = glm::inverse(view);
    gpuCamera.invProj = glm::inverse(proj);
    gpuCamera.position = transform->position;
    gpuCamera.fov = camera->fov;
    gpuCamera.resolution = glm::vec2(width, height);
    gpuCamera.nearPlane = camera->nearClip;
    gpuCamera.farPlane = camera->farClip;

    const bool canRayTrace = m_Renderer.GetTracerRayKHR() && m_Renderer.GetTracerRayKHR()->IsSupported();

    std::vector<gfx::BVHBuilder::Triangle> triangles;
    std::vector<gfx::GPUMaterial> materials;
    std::vector<gfx::GPULight> lights;
    std::vector<gfx::GPUVolume> volumes;

    // Optional RT per-hit material evaluation data (only used by RayTraced backend)
    std::vector<gfx::RTTextureKey> rtTextures;
    std::vector<gfx::RTMaterialHeader> rtHeaders;
    std::vector<gfx::RTMaterialInstr> rtInstrs;

    // Build scene data. If we intend to raytrace, also build RT IR + texture key buffers so materials vary per-hit.
    if ((m_Renderer.GetRenderMode() == gfx::RenderMode::RayTraced) && canRayTrace) {
        BuildTracerSceneData(triangles, materials, lights, volumes, &rtTextures, &rtHeaders, &rtInstrs);
    } else {
        BuildTracerSceneData(triangles, materials, lights, volumes);
    }

    gfx::FinalRenderConfig config;
    config.width = width;
    config.height = height;
    config.samples = settings.finalSamples;
    config.maxBounces = settings.maxBounces;
    config.exposure = settings.exposure;
    config.tonemap = settings.tonemapOperator;
    config.gamma = settings.gamma;
    config.denoiser = settings.denoiser;
    config.denoiseStrength = settings.denoiseStrength;
    config.denoiseRadius = settings.denoiseRadius;
    config.transparentBackground = settings.transparentBackground;
    config.outputPath.clear();
    config.useRayTracing = (m_Renderer.GetRenderMode() == gfx::RenderMode::RayTraced) && canRayTrace;

    if (!finalRender->Start(config, gpuCamera, triangles, materials, rtTextures, rtHeaders, rtInstrs, lights, volumes)) {
        LUCENT_CORE_WARN("Final render failed to start");
        return;
    }

    LUCENT_CORE_INFO("Final render started from primary camera (F12)");
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

void Application::ApplyEnvironmentMapHandle(uint32_t handle) {
    auto* envMap = gfx::EnvironmentMapLibrary::Get().Get(handle);
    if (!envMap) {
        return;
    }

    if (auto* tracer = m_Renderer.GetTracerCompute()) {
        tracer->SetEnvironmentMap(envMap);
    }
    if (auto* tracer = m_Renderer.GetTracerRayKHR()) {
        tracer->SetEnvironmentMap(envMap);
    }

    m_ActiveEnvMapHandle = handle;
}

void Application::UpdateEnvironmentMapFromSettings() {
    gfx::RenderSettings& settings = m_Renderer.GetSettings();
    if (settings.envMapHandle == m_ActiveEnvMapHandle) {
        return;
    }

    uint32_t desiredHandle = settings.envMapHandle;
    if (desiredHandle == gfx::EnvironmentMapLibrary::InvalidHandle) {
        desiredHandle = m_DefaultEnvMapHandle;
    }

    auto* envMap = gfx::EnvironmentMapLibrary::Get().Get(desiredHandle);
    if (!envMap && m_DefaultEnvMapHandle != gfx::EnvironmentMapLibrary::InvalidHandle) {
        desiredHandle = m_DefaultEnvMapHandle;
        envMap = gfx::EnvironmentMapLibrary::Get().Get(desiredHandle);
    }

    if (!envMap) {
        return;
    }

    ApplyEnvironmentMapHandle(desiredHandle);
}

void Application::InitEnvironmentMap() {
    m_DefaultEnvMapHandle = gfx::EnvironmentMapLibrary::Get().CreateDefaultSky();
    if (m_DefaultEnvMapHandle == gfx::EnvironmentMapLibrary::InvalidHandle) {
        LUCENT_CORE_WARN("Failed to create default environment map");
        return;
    }

    gfx::RenderSettings& settings = m_Renderer.GetSettings();
    settings.envMapHandle = m_DefaultEnvMapHandle;
    settings.envMapPath.clear();
    ApplyEnvironmentMapHandle(m_DefaultEnvMapHandle);

    LUCENT_CORE_INFO("Environment map initialized");
}

} // namespace lucent
