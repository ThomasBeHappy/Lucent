#pragma once

#include <string>
#include <vector>

namespace lucent {

// Simple file dialog filters
struct FileFilter {
    std::wstring name;        // e.g. "Scene Files"
    std::wstring extensions;  // e.g. "*.lucent"
};

namespace Win32FileDialogs {

// Open a single file dialog
// Returns empty string if cancelled
std::string OpenFile(const wchar_t* title, const std::vector<FileFilter>& filters, const wchar_t* defaultExt = nullptr);

// Save file dialog
// Returns empty string if cancelled
std::string SaveFile(const wchar_t* title, const std::vector<FileFilter>& filters, const wchar_t* defaultExt = nullptr);

// Open folder dialog
// Returns empty string if cancelled
std::string OpenFolder(const wchar_t* title);

// Message box helpers
enum class MsgBoxResult { Yes, No, Cancel };
MsgBoxResult ShowYesNoCancel(const wchar_t* title, const wchar_t* message);
void ShowInfo(const wchar_t* title, const wchar_t* message);
void ShowError(const wchar_t* title, const wchar_t* message);

} // namespace Win32FileDialogs

} // namespace lucent

