#pragma once

#include <memory>
#include <vector>
#include <string>
#include <functional>

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

} // namespace lucent

