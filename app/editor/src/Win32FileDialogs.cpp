#include "Win32FileDialogs.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <commdlg.h>
#include <ShlObj.h>

#include <codecvt>
#include <locale>

namespace lucent {

// Helper to convert wstring to string (UTF-8)
static std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(size - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &result[0], size, nullptr, nullptr);
    return result;
}

// Build filter string for GetOpenFileName/GetSaveFileName
static std::wstring BuildFilterString(const std::vector<FileFilter>& filters) {
    std::wstring result;
    for (const auto& f : filters) {
        result += f.name;
        result += L'\0';
        result += f.extensions;
        result += L'\0';
    }
    result += L'\0';
    return result;
}

namespace Win32FileDialogs {

std::string OpenFile(const wchar_t* title, const std::vector<FileFilter>& filters, const wchar_t* defaultExt) {
    wchar_t filename[MAX_PATH] = {};
    
    std::wstring filterStr = BuildFilterString(filters);
    
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = filterStr.c_str();
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.lpstrDefExt = defaultExt;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    
    if (GetOpenFileNameW(&ofn)) {
        return WideToUtf8(filename);
    }
    return "";
}

std::string SaveFile(const wchar_t* title, const std::vector<FileFilter>& filters, const wchar_t* defaultExt) {
    wchar_t filename[MAX_PATH] = {};
    
    std::wstring filterStr = BuildFilterString(filters);
    
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = filterStr.c_str();
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.lpstrDefExt = defaultExt;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    
    if (GetSaveFileNameW(&ofn)) {
        return WideToUtf8(filename);
    }
    return "";
}

std::string OpenFolder(const wchar_t* title) {
    wchar_t path[MAX_PATH] = {};
    
    BROWSEINFOW bi = {};
    bi.hwndOwner = GetActiveWindow();
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        SHGetPathFromIDListW(pidl, path);
        CoTaskMemFree(pidl);
        return WideToUtf8(path);
    }
    return "";
}

MsgBoxResult ShowYesNoCancel(const wchar_t* title, const wchar_t* message) {
    int result = MessageBoxW(GetActiveWindow(), message, title, MB_YESNOCANCEL | MB_ICONQUESTION);
    switch (result) {
        case IDYES: return MsgBoxResult::Yes;
        case IDNO: return MsgBoxResult::No;
        default: return MsgBoxResult::Cancel;
    }
}

void ShowInfo(const wchar_t* title, const wchar_t* message) {
    MessageBoxW(GetActiveWindow(), message, title, MB_OK | MB_ICONINFORMATION);
}

void ShowError(const wchar_t* title, const wchar_t* message) {
    MessageBoxW(GetActiveWindow(), message, title, MB_OK | MB_ICONERROR);
}

} // namespace Win32FileDialogs
} // namespace lucent

