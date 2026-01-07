#pragma once

#include "lucent/core/Core.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace lucent::scene {

class EditorCamera {
public:
    enum class Mode {
        Orbit,
        Fly,
        Pan
    };
    
    EditorCamera() = default;
    
    void Update(float deltaTime);
    
    // Input handling
    void OnMouseMove(float xOffset, float yOffset, bool leftButton, bool middleButton, bool rightButton);
    void OnMouseScroll(float yOffset);
    void OnKeyInput(int key, bool pressed);
    
    // Focus on point/object
    void FocusOnPoint(const glm::vec3& point, float distance = 5.0f);
    void Reset();
    
    // Matrices
    glm::mat4 GetViewMatrix() const;
    glm::mat4 GetProjectionMatrix() const;
    glm::mat4 GetViewProjectionMatrix() const;
    
    void SetAspectRatio(float aspectRatio) { m_AspectRatio = aspectRatio; }
    float GetAspectRatio() const { return m_AspectRatio; }
    
    // Properties
    glm::vec3 GetPosition() const { return m_Position; }
    glm::vec3 GetTarget() const { return m_Target; }
    glm::vec3 GetForward() const;
    glm::vec3 GetRight() const;
    glm::vec3 GetUp() const;
    
    void SetPosition(const glm::vec3& pos) { m_Position = pos; }
    void SetTarget(const glm::vec3& target) { m_Target = target; }
    
    float GetFOV() const { return m_FOV; }
    void SetFOV(float fov) { m_FOV = fov; }
    
    float GetNearClip() const { return m_NearClip; }
    float GetFarClip() const { return m_FarClip; }
    void SetClipPlanes(float nearClip, float farClip) { m_NearClip = nearClip; m_FarClip = farClip; }
    
    float GetOrbitDistance() const { return m_OrbitDistance; }
    void SetOrbitDistance(float dist) { m_OrbitDistance = dist; }
    
    Mode GetMode() const { return m_Mode; }
    void SetMode(Mode mode) { m_Mode = mode; }
    
    // Speed settings
    float GetMoveSpeed() const { return m_MoveSpeed; }
    void SetMoveSpeed(float speed) { m_MoveSpeed = speed; }
    
    float GetRotateSpeed() const { return m_RotateSpeed; }
    void SetRotateSpeed(float speed) { m_RotateSpeed = speed; }
    
    float GetZoomSpeed() const { return m_ZoomSpeed; }
    void SetZoomSpeed(float speed) { m_ZoomSpeed = speed; }
    
private:
    void UpdateOrbitPosition();
    
private:
    // Position and orientation
    glm::vec3 m_Position = glm::vec3(5.0f, 5.0f, 5.0f);
    glm::vec3 m_Target = glm::vec3(0.0f, 0.0f, 0.0f);
    glm::vec3 m_WorldUp = glm::vec3(0.0f, 1.0f, 0.0f);
    
    // Orbit parameters
    float m_OrbitDistance = 10.0f;
    float m_Yaw = -45.0f;   // degrees
    float m_Pitch = 30.0f;  // degrees
    
    // Projection
    float m_FOV = 60.0f;
    float m_NearClip = 0.1f;
    float m_FarClip = 1000.0f;
    float m_AspectRatio = 16.0f / 9.0f;
    
    // Speed settings
    float m_MoveSpeed = 5.0f;
    float m_RotateSpeed = 0.3f;
    float m_ZoomSpeed = 1.0f;
    float m_PanSpeed = 0.01f;
    
    // Current mode
    Mode m_Mode = Mode::Orbit;
    
    // Fly mode velocity
    glm::vec3 m_Velocity = glm::vec3(0.0f);
    
    // Input state for fly mode
    bool m_MoveForward = false;
    bool m_MoveBackward = false;
    bool m_MoveLeft = false;
    bool m_MoveRight = false;
    bool m_MoveUp = false;
    bool m_MoveDown = false;
};

} // namespace lucent::scene

