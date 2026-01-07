#include "UndoStack.h"
#include "lucent/core/Log.h"

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

} // namespace lucent

