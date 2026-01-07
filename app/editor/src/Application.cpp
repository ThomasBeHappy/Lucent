#include "Application.h"
#include "EditorSettings.h"
#include "lucent/gfx/DebugUtils.h"
#include "lucent/scene/Components.h"
#include "lucent/material/MaterialAsset.h"
#include <GLFW/glfw3.h>

// GLFW native access (Win32 HWND)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <GLFW/glfw3native.h>
#include <Windows.h>

namespace lucent {

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

void Application::RenderMeshes(VkCommandBuffer cmd, const glm::mat4& viewProj) {
    // Get default render mode pipeline
    RenderMode mode = m_EditorUI.GetRenderMode();
    VkPipeline defaultPipeline = m_Renderer.GetMeshPipeline();
    VkPipelineLayout defaultLayout = m_Renderer.GetMeshPipelineLayout();
    
    if (mode == RenderMode::Wireframe && m_Renderer.GetMeshWireframePipeline()) {
        defaultPipeline = m_Renderer.GetMeshWireframePipeline();
    }
    
    // Bind shadow map descriptor set (set 0)
    VkDescriptorSet shadowSet = m_Renderer.GetShadowDescriptorSet();
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, defaultLayout, 
        0, 1, &shadowSet, 0, nullptr);
    
    // Get camera position for specular calculations
    glm::vec3 camPos = m_EditorCamera.GetPosition();
    
    // Track currently bound pipeline for batching
    VkPipeline currentPipeline = VK_NULL_HANDLE;
    VkPipelineLayout currentLayout = VK_NULL_HANDLE;
    
    // Iterate all entities with MeshRendererComponent and TransformComponent
    auto view = m_Scene.GetView<scene::MeshRendererComponent, scene::TransformComponent>();
    view.Each([&](scene::Entity entity, scene::MeshRendererComponent& renderer, scene::TransformComponent& transform) {
        (void)entity; // Unused
        
        if (!renderer.visible) return;
        
        // Get mesh based on primitive type
        auto it = m_PrimitiveMeshes.find(renderer.primitiveType);
        if (it == m_PrimitiveMeshes.end() || !it->second) return;
        
        assets::Mesh* mesh = it->second.get();
        
        // Determine pipeline and layout to use
        VkPipeline pipeline = defaultPipeline;
        VkPipelineLayout layout = defaultLayout;
        
        // Check if entity has a material asset assigned
        if (renderer.UsesMaterialAsset()) {
            auto* mat = material::MaterialAssetManager::Get().GetMaterial(renderer.materialPath);
            if (mat && mat->IsValid() && mat->GetPipeline()) {
                pipeline = mat->GetPipeline();
                layout = mat->GetPipelineLayout();
            }
        }
        
        // Bind pipeline if changed
        if (pipeline != currentPipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            currentPipeline = pipeline;
            currentLayout = layout;
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
    lightComp.intensity = 1.0f;
    
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
        
        // Store camera state before update
        glm::mat4 prevViewMatrix = m_EditorCamera.GetViewMatrix();
        
        // Update camera
        m_EditorCamera.Update(m_DeltaTime);
        
        // Check if camera has moved (reset accumulation for traced modes)
        if (m_Renderer.GetRenderMode() != gfx::RenderMode::Simple) {
            glm::mat4 newViewMatrix = m_EditorCamera.GetViewMatrix();
            if (prevViewMatrix != newViewMatrix) {
                m_Renderer.GetSettings().MarkDirty();
            }
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
    
    // Update camera aspect ratio based on viewport size
    float aspectRatio = static_cast<float>(extent.width) / static_cast<float>(extent.height);
    m_EditorCamera.SetAspectRatio(aspectRatio);
    
    // Check render mode
    gfx::RenderMode renderMode = m_Renderer.GetRenderMode();
    
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
            // Transition offscreen to transfer dst
            offscreen->TransitionLayout(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            
            // Transition accumulation to transfer src
            tracer->GetAccumulationImage()->TransitionLayout(cmd, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            
            // Blit accumulation to offscreen
            VkImageBlit blitRegion{};
            blitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blitRegion.srcSubresource.layerCount = 1;
            blitRegion.srcOffsets[1] = { static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height), 1 };
            blitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blitRegion.dstSubresource.layerCount = 1;
            blitRegion.dstOffsets[1] = { static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height), 1 };
            
            vkCmdBlitImage(cmd, 
                tracer->GetAccumulationImage()->GetHandle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                offscreen->GetHandle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1, &blitRegion, VK_FILTER_NEAREST);
            
            // Transition back
            tracer->GetAccumulationImage()->TransitionLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
            offscreen->TransitionLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    } else {
        // =========================================================================
        // Simple Mode: Standard raster PBR
        // =========================================================================
        
        // Update light matrix for shadow mapping
        UpdateLightMatrix();
        
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
    // Find directional light in scene
    glm::vec3 lightDir = glm::normalize(glm::vec3(1.0f, 1.0f, 0.5f)); // Default
    
    auto lightEntities = m_Scene.GetView<scene::LightComponent, scene::TransformComponent>();
    lightEntities.Each([&](scene::Entity entity, scene::LightComponent& light, scene::TransformComponent& transform) {
        (void)entity;
        if (light.type == scene::LightType::Directional) {
            // Get light direction from rotation
            glm::mat4 rotMat = glm::mat4(1.0f);
            rotMat = glm::rotate(rotMat, glm::radians(transform.rotation.x), glm::vec3(1, 0, 0));
            rotMat = glm::rotate(rotMat, glm::radians(transform.rotation.y), glm::vec3(0, 1, 0));
            rotMat = glm::rotate(rotMat, glm::radians(transform.rotation.z), glm::vec3(0, 0, 1));
            lightDir = -glm::normalize(glm::vec3(rotMat * glm::vec4(0, 0, -1, 0)));
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
        
        auto it = m_PrimitiveMeshes.find(renderer.primitiveType);
        if (it == m_PrimitiveMeshes.end() || !it->second) return;
        
        assets::Mesh* mesh = it->second.get();
        
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
    gfx::TracerCompute* tracer = m_Renderer.GetTracerCompute();
    if (!tracer) return;
    
    // Build scene data for the tracer
    std::vector<gfx::BVHBuilder::Triangle> triangles;
    std::vector<gfx::GPUMaterial> materials;
    
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
        
        // Find mesh
        auto it = m_PrimitiveMeshes.find(renderer.primitiveType);
        if (it == m_PrimitiveMeshes.end() || !it->second) return;
        
        assets::Mesh* mesh = it->second.get();
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
    
    tracer->UpdateScene(triangles, materials);
    m_TracerSceneDirty = false;
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

} // namespace lucent
