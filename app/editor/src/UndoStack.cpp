#include "UndoStack.h"
#include "lucent/core/Log.h"
#include "lucent/scene/Scene.h"
#include "lucent/scene/Components.h"
#include "lucent/material/MaterialAsset.h"
#include "lucent/material/MaterialGraph.h"
#include "lucent/mesh/EditableMesh.h"

namespace lucent {

void UndoStack::Execute(std::unique_ptr<ICommand> command) {
    if (!command) return;
    
    // Check if we can merge with the last command (for continuous edits)
    if (m_InMergeWindow && !m_UndoStack.empty()) {
        ICommand* last = m_UndoStack.back().get();
        if (last->GetTypeId() == command->GetTypeId() &&
            last->GetTargetId() == command->GetTargetId() &&
            last->CanMergeWith(command.get())) {
            // Merge: update the last command with new final state
            last->MergeWith(command.get());
            LUCENT_CORE_DEBUG("Merged command: {}", last->GetDescription());
            return; // Don't push, just merged
        }
    }
    
    // Execute the command
    command->Execute();
    
    // Clear redo stack (new action invalidates redo history)
    m_RedoStack.clear();
    
    // Push onto undo stack
    m_UndoStack.push_back(std::move(command));
    
    // Trim if exceeding max size
    if (m_MaxStackSize > 0 && m_UndoStack.size() > m_MaxStackSize) {
        m_UndoStack.erase(m_UndoStack.begin());
    }
    
    LUCENT_CORE_DEBUG("Executed command: {} (undo stack: {})", 
        m_UndoStack.back()->GetDescription(), m_UndoStack.size());
}

void UndoStack::Push(std::unique_ptr<ICommand> command) {
    if (!command) return;
    
    // Clear redo stack (new action invalidates redo history)
    m_RedoStack.clear();
    
    // Push onto undo stack without executing
    m_UndoStack.push_back(std::move(command));
    
    // Trim if exceeding max size
    if (m_MaxStackSize > 0 && m_UndoStack.size() > m_MaxStackSize) {
        m_UndoStack.erase(m_UndoStack.begin());
    }
    
    LUCENT_CORE_DEBUG("Pushed command: {} (undo stack: {})", 
        m_UndoStack.back()->GetDescription(), m_UndoStack.size());
}

void UndoStack::ExecuteWithoutPush(ICommand* command) {
    if (command) {
        command->Execute();
    }
}

bool UndoStack::Undo() {
    if (m_UndoStack.empty()) {
        return false;
    }
    
    // Pop from undo stack
    auto command = std::move(m_UndoStack.back());
    m_UndoStack.pop_back();
    
    // Undo the command
    command->Undo();
    
    LUCENT_CORE_DEBUG("Undid command: {}", command->GetDescription());
    
    // Push onto redo stack
    m_RedoStack.push_back(std::move(command));
    
    return true;
}

bool UndoStack::Redo() {
    if (m_RedoStack.empty()) {
        return false;
    }
    
    // Pop from redo stack
    auto command = std::move(m_RedoStack.back());
    m_RedoStack.pop_back();
    
    // Re-execute the command
    command->Execute();
    
    LUCENT_CORE_DEBUG("Redid command: {}", command->GetDescription());
    
    // Push onto undo stack
    m_UndoStack.push_back(std::move(command));
    
    return true;
}

std::string UndoStack::GetUndoDescription() const {
    if (m_UndoStack.empty()) {
        return "";
    }
    return m_UndoStack.back()->GetDescription();
}

std::string UndoStack::GetRedoDescription() const {
    if (m_RedoStack.empty()) {
        return "";
    }
    return m_RedoStack.back()->GetDescription();
}

void UndoStack::Clear() {
    m_UndoStack.clear();
    m_RedoStack.clear();
    m_InMergeWindow = false;
    LUCENT_CORE_DEBUG("Undo stack cleared");
}

// ============================================================================
// TransformCommand Implementation
// ============================================================================

void TransformCommand::Execute() {
    if (!m_Scene) return;
    
    scene::Entity entity = m_Scene->GetEntity(m_EntityId);
    if (!entity.IsValid()) return;
    
    auto* transform = entity.GetComponent<scene::TransformComponent>();
    if (!transform) return;
    
    transform->position = m_After.position;
    transform->rotation = m_After.rotation;
    transform->scale = m_After.scale;
}

void TransformCommand::Undo() {
    if (!m_Scene) return;
    
    scene::Entity entity = m_Scene->GetEntity(m_EntityId);
    if (!entity.IsValid()) return;
    
    auto* transform = entity.GetComponent<scene::TransformComponent>();
    if (!transform) return;
    
    transform->position = m_Before.position;
    transform->rotation = m_Before.rotation;
    transform->scale = m_Before.scale;
}

TransformCommand::TransformState TransformCommand::CaptureState(scene::TransformComponent* transform) {
    TransformState state;
    if (transform) {
        state.position = transform->position;
        state.rotation = transform->rotation;
        state.scale = transform->scale;
    }
    return state;
}

// ============================================================================
// MaterialParamCommand Implementation
// ============================================================================

void MaterialParamCommand::Execute() {
    if (!m_Material) return;
    
    auto& graph = m_Material->GetGraph();
    material::MaterialNode* node = graph.GetNode(m_NodeId);
    if (!node) return;
    
    switch (m_Type) {
        case ParamType::Float:
            node->parameter = m_FloatAfter;
            break;
        case ParamType::Vec3:
            node->parameter = m_Vec3After;
            break;
        case ParamType::ColorRamp:
            // ColorRamp handled separately if needed
            break;
    }
    
    // Trigger recompile
    m_Material->MarkDirty();
}

void MaterialParamCommand::Undo() {
    if (!m_Material) return;
    
    auto& graph = m_Material->GetGraph();
    material::MaterialNode* node = graph.GetNode(m_NodeId);
    if (!node) return;
    
    switch (m_Type) {
        case ParamType::Float:
            node->parameter = m_FloatBefore;
            break;
        case ParamType::Vec3:
            node->parameter = m_Vec3Before;
            break;
        case ParamType::ColorRamp:
            // ColorRamp handled separately if needed
            break;
    }
    
    // Trigger recompile
    m_Material->MarkDirty();
}

// ============================================================================
// MeshEditCommand Implementation
// ============================================================================

MeshEditCommand::MeshSnapshot MeshEditCommand::CaptureSnapshot(scene::EditableMeshComponent* meshComp) {
    MeshSnapshot snapshot;
    
    if (!meshComp || !meshComp->mesh) {
        return snapshot;
    }
    
    auto data = meshComp->mesh->Serialize();
    snapshot.positions = std::move(data.positions);
    snapshot.uvs = std::move(data.uvs);
    snapshot.faceVertexIndices = std::move(data.faceVertexIndices);
    
    return snapshot;
}

void MeshEditCommand::ApplySnapshot(scene::EditableMeshComponent* meshComp, const MeshSnapshot& snapshot) {
    if (!meshComp) return;
    
    mesh::EditableMesh::SerializedData data;
    data.positions = snapshot.positions;
    data.uvs = snapshot.uvs;
    data.faceVertexIndices = snapshot.faceVertexIndices;
    
    meshComp->mesh = std::make_unique<mesh::EditableMesh>(
        mesh::EditableMesh::Deserialize(data)
    );
    meshComp->MarkDirty();
}

void MeshEditCommand::Execute() {
    if (!m_Scene) return;
    
    scene::Entity entity = m_Scene->GetEntity(m_EntityId);
    if (!entity.IsValid()) return;
    
    auto* meshComp = entity.GetComponent<scene::EditableMeshComponent>();
    if (meshComp) {
        ApplySnapshot(meshComp, m_After);
    }
}

void MeshEditCommand::Undo() {
    if (!m_Scene) return;
    
    scene::Entity entity = m_Scene->GetEntity(m_EntityId);
    if (!entity.IsValid()) return;
    
    auto* meshComp = entity.GetComponent<scene::EditableMeshComponent>();
    if (meshComp) {
        ApplySnapshot(meshComp, m_Before);
    }
}

} // namespace lucent

