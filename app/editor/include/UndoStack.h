#pragma once

#include <memory>
#include <vector>
#include <string>
#include <functional>
#include <glm/glm.hpp>

namespace lucent {

// Base command interface for undo/redo
class ICommand {
public:
    virtual ~ICommand() = default;
    
    // Execute the command (called on first do and redo)
    virtual void Execute() = 0;
    
    // Undo the command
    virtual void Undo() = 0;
    
    // Get a description for UI
    virtual std::string GetDescription() const = 0;
    
    // Can this command merge with another of the same type?
    // Used for continuous edits like gizmo dragging
    virtual bool CanMergeWith(const ICommand* other) const { (void)other; return false; }
    
    // Merge another command into this one (absorb its final state)
    virtual void MergeWith(const ICommand* other) { (void)other; }
    
    // Get command type ID for merge matching
    virtual size_t GetTypeId() const = 0;
    
    // Get target ID (e.g., entity ID) for merge matching
    virtual uint64_t GetTargetId() const { return 0; }
};

// Helper macro for type ID
#define COMMAND_TYPE_ID(TypeName) \
    size_t GetTypeId() const override { return typeid(TypeName).hash_code(); }

// Undo/Redo stack manager
class UndoStack {
public:
    static UndoStack& Get() {
        static UndoStack instance;
        return instance;
    }
    
    // Execute a command and push it onto the stack
    void Execute(std::unique_ptr<ICommand> command);
    
    // Push a command without executing (for when state is already applied)
    void Push(std::unique_ptr<ICommand> command);
    
    // Execute without adding to stack (for internal use during undo/redo)
    void ExecuteWithoutPush(ICommand* command);
    
    // Undo the last command
    bool Undo();
    
    // Redo the last undone command
    bool Redo();
    
    // Check if undo/redo is available
    bool CanUndo() const { return !m_UndoStack.empty(); }
    bool CanRedo() const { return !m_RedoStack.empty(); }
    
    // Get descriptions for UI
    std::string GetUndoDescription() const;
    std::string GetRedoDescription() const;
    
    // Clear all history
    void Clear();
    
    // Get stack sizes
    size_t GetUndoCount() const { return m_UndoStack.size(); }
    size_t GetRedoCount() const { return m_RedoStack.size(); }
    
    // Begin/end a mergeable operation (for continuous edits like dragging)
    void BeginMergeWindow() { m_InMergeWindow = true; }
    void EndMergeWindow() { m_InMergeWindow = false; }
    bool IsInMergeWindow() const { return m_InMergeWindow; }
    
    // Set maximum stack size (0 = unlimited)
    void SetMaxStackSize(size_t size) { m_MaxStackSize = size; }
    
private:
    UndoStack() = default;
    
    std::vector<std::unique_ptr<ICommand>> m_UndoStack;
    std::vector<std::unique_ptr<ICommand>> m_RedoStack;
    
    bool m_InMergeWindow = false;
    size_t m_MaxStackSize = 100;
};

// ============================================================================
// Common Command Types
// ============================================================================

// Forward declarations for scene types
namespace scene {
    class Scene;
    struct TransformComponent;
}

// Transform edit command (for gizmo operations)
class TransformCommand : public ICommand {
public:
    struct TransformState {
        glm::vec3 position;
        glm::vec3 rotation;
        glm::vec3 scale;
    };
    
    TransformCommand(scene::Scene* scene, uint32_t entityId, 
                     const TransformState& before, const TransformState& after)
        : m_Scene(scene)
        , m_EntityId(entityId)
        , m_Before(before)
        , m_After(after) {}
    
    void Execute() override;
    void Undo() override;
    std::string GetDescription() const override { return "Transform"; }
    
    COMMAND_TYPE_ID(TransformCommand)
    uint64_t GetTargetId() const override { return m_EntityId; }
    
    bool CanMergeWith(const ICommand* other) const override {
        auto* o = dynamic_cast<const TransformCommand*>(other);
        return o && o->m_EntityId == m_EntityId;
    }
    
    void MergeWith(const ICommand* other) override {
        auto* o = dynamic_cast<const TransformCommand*>(other);
        if (o) {
            m_After = o->m_After;
        }
    }
    
    static TransformState CaptureState(scene::TransformComponent* transform);
    
private:
    scene::Scene* m_Scene;
    uint32_t m_EntityId;
    TransformState m_Before;
    TransformState m_After;
};

// Generic lambda-based command for simple cases
class LambdaCommand : public ICommand {
public:
    using DoFunc = std::function<void()>;
    using UndoFunc = std::function<void()>;
    
    LambdaCommand(std::string description, DoFunc doFunc, UndoFunc undoFunc)
        : m_Description(std::move(description))
        , m_DoFunc(std::move(doFunc))
        , m_UndoFunc(std::move(undoFunc)) {}
    
    void Execute() override { if (m_DoFunc) m_DoFunc(); }
    void Undo() override { if (m_UndoFunc) m_UndoFunc(); }
    std::string GetDescription() const override { return m_Description; }
    COMMAND_TYPE_ID(LambdaCommand)
    
private:
    std::string m_Description;
    DoFunc m_DoFunc;
    UndoFunc m_UndoFunc;
};

// Forward declaration
namespace material {
    class MaterialAsset;
    struct MaterialNode;
}

// Material parameter edit command
class MaterialParamCommand : public ICommand {
public:
    enum class ParamType { Float, Vec3, ColorRamp };
    
    // Float parameter constructor
    MaterialParamCommand(material::MaterialAsset* material, uint64_t nodeId, 
                         const std::string& paramName, float before, float after)
        : m_Material(material)
        , m_NodeId(nodeId)
        , m_ParamName(paramName)
        , m_Type(ParamType::Float)
        , m_FloatBefore(before)
        , m_FloatAfter(after) {}
    
    // Vec3 parameter constructor
    MaterialParamCommand(material::MaterialAsset* material, uint64_t nodeId,
                         const std::string& paramName, const glm::vec3& before, const glm::vec3& after)
        : m_Material(material)
        , m_NodeId(nodeId)
        , m_ParamName(paramName)
        , m_Type(ParamType::Vec3)
        , m_Vec3Before(before)
        , m_Vec3After(after) {}
    
    void Execute() override;
    void Undo() override;
    std::string GetDescription() const override { return "Material: " + m_ParamName; }
    
    COMMAND_TYPE_ID(MaterialParamCommand)
    uint64_t GetTargetId() const override { return m_NodeId; }
    
    bool CanMergeWith(const ICommand* other) const override {
        auto* o = dynamic_cast<const MaterialParamCommand*>(other);
        return o && o->m_NodeId == m_NodeId && o->m_ParamName == m_ParamName;
    }
    
    void MergeWith(const ICommand* other) override {
        auto* o = dynamic_cast<const MaterialParamCommand*>(other);
        if (o) {
            if (m_Type == ParamType::Float) m_FloatAfter = o->m_FloatAfter;
            else if (m_Type == ParamType::Vec3) m_Vec3After = o->m_Vec3After;
        }
    }
    
private:
    material::MaterialAsset* m_Material;
    uint64_t m_NodeId;
    std::string m_ParamName;
    ParamType m_Type;
    
    float m_FloatBefore = 0.0f;
    float m_FloatAfter = 0.0f;
    glm::vec3 m_Vec3Before{0.0f};
    glm::vec3 m_Vec3After{0.0f};
};

} // namespace lucent

