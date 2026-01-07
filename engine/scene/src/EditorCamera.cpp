#include "lucent/scene/EditorCamera.h"
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <GLFW/glfw3.h>

namespace lucent::scene {

void EditorCamera::Update(float deltaTime) {
    if (m_Mode == Mode::Fly) {
        // Calculate movement direction
        glm::vec3 direction(0.0f);
        
        if (m_MoveForward) direction += GetForward();
        if (m_MoveBackward) direction -= GetForward();
        if (m_MoveRight) direction += GetRight();
        if (m_MoveLeft) direction -= GetRight();
        if (m_MoveUp) direction += m_WorldUp;
        if (m_MoveDown) direction -= m_WorldUp;
        
        if (glm::length(direction) > 0.0f) {
            direction = glm::normalize(direction);
            m_Position += direction * m_MoveSpeed * deltaTime;
        }
        
        // Update target to stay in front of camera
        m_Target = m_Position + GetForward() * m_OrbitDistance;
    }
}

void EditorCamera::OnMouseMove(float xOffset, float yOffset, bool leftButton, bool middleButton, bool rightButton) {
    if (m_Mode == Mode::Orbit) {
        if (middleButton || (leftButton && rightButton)) {
            // Pan
            glm::vec3 right = GetRight();
            glm::vec3 up = GetUp();
            
            m_Target -= right * xOffset * m_PanSpeed * m_OrbitDistance;
            m_Target += up * yOffset * m_PanSpeed * m_OrbitDistance;
            
            UpdateOrbitPosition();
        } else if (leftButton || rightButton) {
            // Rotate
            m_Yaw -= xOffset * m_RotateSpeed;
            m_Pitch += yOffset * m_RotateSpeed;
            
            // Clamp pitch to avoid gimbal lock
            m_Pitch = std::clamp(m_Pitch, -89.0f, 89.0f);
            
            UpdateOrbitPosition();
        }
    } else if (m_Mode == Mode::Fly) {
        if (rightButton) {
            // Rotate camera
            m_Yaw -= xOffset * m_RotateSpeed;
            m_Pitch += yOffset * m_RotateSpeed;
            
            m_Pitch = std::clamp(m_Pitch, -89.0f, 89.0f);
        }
    } else if (m_Mode == Mode::Pan) {
        glm::vec3 right = GetRight();
        glm::vec3 up = GetUp();
        
        m_Target -= right * xOffset * m_PanSpeed * m_OrbitDistance;
        m_Target += up * yOffset * m_PanSpeed * m_OrbitDistance;
        m_Position -= right * xOffset * m_PanSpeed * m_OrbitDistance;
        m_Position += up * yOffset * m_PanSpeed * m_OrbitDistance;
    }
}

void EditorCamera::OnMouseScroll(float yOffset) {
    if (m_Mode == Mode::Orbit) {
        m_OrbitDistance -= yOffset * m_ZoomSpeed;
        m_OrbitDistance = std::max(m_OrbitDistance, 0.5f);
        UpdateOrbitPosition();
    } else if (m_Mode == Mode::Fly) {
        // Adjust FOV in fly mode
        m_FOV -= yOffset * 2.0f;
        m_FOV = std::clamp(m_FOV, 10.0f, 120.0f);
    }
}

void EditorCamera::OnKeyInput(int key, bool pressed) {
    // WASD + QE for fly mode
    switch (key) {
        case GLFW_KEY_W: m_MoveForward = pressed; break;
        case GLFW_KEY_S: m_MoveBackward = pressed; break;
        case GLFW_KEY_A: m_MoveLeft = pressed; break;
        case GLFW_KEY_D: m_MoveRight = pressed; break;
        case GLFW_KEY_Q: m_MoveDown = pressed; break;
        case GLFW_KEY_E: m_MoveUp = pressed; break;
    }
}

void EditorCamera::FocusOnPoint(const glm::vec3& point, float distance) {
    m_Target = point;
    m_OrbitDistance = distance;
    UpdateOrbitPosition();
}

void EditorCamera::Reset() {
    m_Position = glm::vec3(5.0f, 5.0f, 5.0f);
    m_Target = glm::vec3(0.0f, 0.0f, 0.0f);
    m_OrbitDistance = 10.0f;
    m_Yaw = -45.0f;
    m_Pitch = 30.0f;
    m_FOV = 60.0f;
    
    UpdateOrbitPosition();
}

glm::mat4 EditorCamera::GetViewMatrix() const {
    return glm::lookAt(m_Position, m_Target, m_WorldUp);
}

glm::mat4 EditorCamera::GetProjectionMatrix() const {
    return glm::perspective(glm::radians(m_FOV), m_AspectRatio, m_NearClip, m_FarClip);
}

glm::mat4 EditorCamera::GetViewProjectionMatrix() const {
    return GetProjectionMatrix() * GetViewMatrix();
}

glm::vec3 EditorCamera::GetForward() const {
    float yawRad = glm::radians(m_Yaw);
    float pitchRad = glm::radians(m_Pitch);
    
    return glm::normalize(glm::vec3(
        cos(yawRad) * cos(pitchRad),
        sin(pitchRad),
        sin(yawRad) * cos(pitchRad)
    ));
}

glm::vec3 EditorCamera::GetRight() const {
    return glm::normalize(glm::cross(GetForward(), m_WorldUp));
}

glm::vec3 EditorCamera::GetUp() const {
    return glm::normalize(glm::cross(GetRight(), GetForward()));
}

void EditorCamera::UpdateOrbitPosition() {
    float yawRad = glm::radians(m_Yaw);
    float pitchRad = glm::radians(m_Pitch);
    
    glm::vec3 offset;
    offset.x = cos(yawRad) * cos(pitchRad);
    offset.y = sin(pitchRad);
    offset.z = sin(yawRad) * cos(pitchRad);
    
    m_Position = m_Target + glm::normalize(offset) * m_OrbitDistance;
}

} // namespace lucent::scene

