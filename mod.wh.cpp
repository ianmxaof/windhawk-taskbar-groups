// ==WindhawkMod==
// @id              taskbar-app-groups
// @name            Taskbar App Groups
// @description     Create grouped, nested app launchers directly on the taskbar
// @version         0.1
// @author          Ian
// @include         explorer.exe
// @compilerOptions -lpsapi -lcomctl32 -lshell32
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Taskbar App Groups

Create configurable groups of applications that appear on the Windows 11 taskbar. 
Clicking a group intelligently focuses running apps or launches them if not running.

## Features

- Configurable app groups via settings
- Smart focus/launch behavior (focus if running, launch if not)
- On-demand window resolution (no continuous monitoring)
- Ctrl+Right-click taskbar to access groups (MVP phase)

## Getting Started

1. **Configure groups in the mod settings** – Use apps you have installed (e.g. "System Tools" with `notepad.exe;calc.exe;mspaint.exe`, or "File Management" with `explorer.exe;cmd.exe`). Click **Save settings** to persist.
2. **Click the Taskbar Groups icon in the system tray** (bottom-right near the clock) to open the group menu.
3. Click a group name to focus running apps in that group or launch the preferred app.
4. *(Optional)* Ctrl+Right-click on the taskbar may also open the menu on some systems.

## Future Enhancements

- Direct taskbar button injection (Phase 2)
- Modifier key support (Shift+Click, Ctrl+Click)
- Window thumbnail previews

Check out the documentation
[here](https://github.com/ramensoftware/windhawk/wiki/Creating-a-new-mod).
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- group0Name: System Tools
  $name: Group 1 - Name
  $description: Name for the first application group
- group0Apps: notepad.exe;calc.exe;mspaint.exe
  $name: Group 1 - Applications
  $description: Semicolon-separated list of executables
- group0PreferredApp: notepad.exe
  $name: Group 1 - Preferred App
  $description: App to launch when group is clicked (leave empty for first app)
- group1Name: File Management
  $name: Group 2 - Name
  $description: Name for the second application group
- group1Apps: explorer.exe;cmd.exe
  $name: Group 2 - Applications
  $description: Semicolon-separated list of executables
- group1PreferredApp: explorer.exe
  $name: Group 2 - Preferred App
  $description: App to launch when group is clicked
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <vector>
#include <string>
#include <psapi.h>
#include <shellapi.h>

// Core data structures
struct AppGroup {
    std::wstring name;
    std::vector<std::wstring> apps;
    std::wstring preferredApp;
    std::wstring icon;
};

struct RunningApp {
    DWORD processId;
    HWND windowHandle;
    std::wstring exeName;
};

struct Settings {
    std::vector<AppGroup> groups;
};

Settings g_settings;
HWND g_taskbarWindow = NULL;
HWND g_trayIconWindow = NULL;  // Created and owned by the tray thread
NOTIFYICONDATAW g_notifyIcon = {0};
HANDLE g_trayThread = NULL;
DWORD g_trayThreadId = 0;

#define TRAY_ICON_WINDOW_CLASS L"TaskbarGroupsTrayWindow"
#define TRAY_ICON_CALLBACK_MSG (WM_USER + 1)

// Forward declarations
void LoadSettings();
void HandleGroupAction(int groupIndex);
std::vector<RunningApp> GetRunningApps(int groupIndex);
HWND GetMostRecentWindow(int groupIndex);
bool IsProcessRunning(const std::wstring& exeName);
void LaunchApp(const std::wstring& appPath);
void FocusWindow(HWND hwnd);
void ShowGroupMenu(HWND hwnd);

// Window enumeration callback for finding windows by process
struct EnumWindowsData {
    DWORD targetProcessId;
    std::vector<HWND> windows;
};

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    EnumWindowsData* data = (EnumWindowsData*)lParam;
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    
    if (processId == data->targetProcessId) {
        if (IsWindowVisible(hwnd)) {
            // Filter out tool windows and owned windows
            LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
            if (!(exStyle & WS_EX_TOOLWINDOW)) {
                data->windows.push_back(hwnd);
            }
        }
    }
    
    return TRUE;
}

// Check if a process is running by executable name
bool IsProcessRunning(const std::wstring& exeName) {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    
    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);
    
    bool found = false;
    if (Process32FirstW(hSnapshot, &pe32)) {
        do {
            // Compare executable name (case-insensitive)
            if (_wcsicmp(pe32.szExeFile, exeName.c_str()) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(hSnapshot, &pe32));
    }
    
    CloseHandle(hSnapshot);
    return found;
}

// Get running apps for a specific group
std::vector<RunningApp> GetRunningApps(int groupIndex) {
    std::vector<RunningApp> runningApps;
    
    if (groupIndex < 0 || groupIndex >= (int)g_settings.groups.size()) {
        return runningApps;
    }
    
    const AppGroup& group = g_settings.groups[groupIndex];
    
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        Wh_Log(L"Failed to create process snapshot for group %d", groupIndex);
        return runningApps;
    }
    
    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);
    
    if (Process32FirstW(hSnapshot, &pe32)) {
        do {
            // Check if this process matches any app in the group
            for (const auto& appName : group.apps) {
                if (_wcsicmp(pe32.szExeFile, appName.c_str()) == 0) {
                    // Found matching process, enumerate its windows
                    EnumWindowsData data;
                    data.targetProcessId = pe32.th32ProcessID;
                    EnumWindows(EnumWindowsProc, (LPARAM)&data);
                    
                    for (HWND hwnd : data.windows) {
                        RunningApp app;
                        app.processId = pe32.th32ProcessID;
                        app.windowHandle = hwnd;
                        app.exeName = appName;
                        runningApps.push_back(app);
                    }
                    break;
                }
            }
        } while (Process32NextW(hSnapshot, &pe32));
    }
    
    CloseHandle(hSnapshot);
    return runningApps;
}

// Get the most recently active window from a list
HWND GetMostRecentWindow(int groupIndex) {
    std::vector<RunningApp> runningApps = GetRunningApps(groupIndex);
    
    if (runningApps.empty()) {
        return NULL;
    }
    
    // Get foreground window first
    HWND foregroundWindow = GetForegroundWindow();
    DWORD foregroundProcessId = 0;
    if (foregroundWindow) {
        GetWindowThreadProcessId(foregroundWindow, &foregroundProcessId);
    }
    
    // Check if foreground window is in the group
    for (const auto& app : runningApps) {
        if (app.windowHandle == foregroundWindow) {
            return foregroundWindow;
        }
    }
    
    // Use Z-order to find most recent
    // Walk through windows in Z-order
    HWND topWindow = GetTopWindow(NULL);
    HWND candidate = NULL;
    
    while (topWindow) {
        DWORD processId = 0;
        GetWindowThreadProcessId(topWindow, &processId);
        
        for (const auto& app : runningApps) {
            if (app.processId == processId && app.windowHandle == topWindow) {
                candidate = topWindow;
                break;
            }
        }
        
        if (candidate) {
            break;
        }
        
        topWindow = GetWindow(topWindow, GW_HWNDNEXT);
    }
    
    // If no candidate found, return first window
    return candidate ? candidate : runningApps[0].windowHandle;
}

// Focus a window, bringing it to foreground
void FocusWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
        return;
    }
    
    // Restore if minimized
    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }
    
    // Bring to foreground
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    SetFocus(hwnd);
}

// Launch an app
void LaunchApp(const std::wstring& appPath) {
    Wh_Log(L"Launching app: %s", appPath.c_str());
    
    // Try to find the executable in common paths
    std::wstring fullPath = appPath;
    
    // If no path specified, try to find it in PATH
    if (appPath.find(L'\\') == std::wstring::npos && appPath.find(L'/') == std::wstring::npos) {
        // Search in system directories
        WCHAR systemPath[MAX_PATH];
        GetSystemDirectoryW(systemPath, MAX_PATH);
        
        WCHAR programFiles[MAX_PATH];
        ExpandEnvironmentStringsW(L"%ProgramFiles%", programFiles, MAX_PATH);
        
        WCHAR programFilesX86[MAX_PATH];
        ExpandEnvironmentStringsW(L"%ProgramFiles(x86)%", programFilesX86, MAX_PATH);
        
        WCHAR localAppData[MAX_PATH];
        ExpandEnvironmentStringsW(L"%LocalAppData%", localAppData, MAX_PATH);
        
        // Try common paths
        std::vector<std::wstring> searchPaths = {
            std::wstring(systemPath) + L"\\" + appPath,
            std::wstring(programFiles) + L"\\" + appPath,
            std::wstring(programFilesX86) + L"\\" + appPath,
            std::wstring(localAppData) + L"\\Programs\\" + appPath,
            appPath  // Try as-is last
        };
        
        bool found = false;
        for (const auto& path : searchPaths) {
            DWORD attrs = GetFileAttributesW(path.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                fullPath = path;
                found = true;
                break;
            }
        }
        
        if (!found) {
            // Use ShellExecute which will search PATH
            fullPath = appPath;
        }
    }
    
    // Launch the application
    HINSTANCE result = ShellExecuteW(
        NULL,
        L"open",
        fullPath.c_str(),
        NULL,
        NULL,
        SW_SHOWNORMAL
    );
    
    if ((INT_PTR)result <= 32) {
        Wh_Log(L"Failed to launch app: %s (error code: %d)", appPath.c_str(), (INT_PTR)result);
    } else {
        Wh_Log(L"Successfully launched app: %s", appPath.c_str());
    }
}

// Core function to handle group action with smart focus/launch logic
void HandleGroupAction(int groupIndex) {
    if (groupIndex < 0 || groupIndex >= (int)g_settings.groups.size()) {
        Wh_Log(L"Invalid group index: %d", groupIndex);
        return;
    }
    
    const AppGroup& group = g_settings.groups[groupIndex];
    Wh_Log(L"HandleGroupAction for group: %s", group.name.c_str());
    
    // Step 1: Check if a window from the group is currently foreground
    HWND foregroundWindow = GetForegroundWindow();
    if (foregroundWindow) {
        DWORD foregroundProcessId = 0;
        GetWindowThreadProcessId(foregroundWindow, &foregroundProcessId);
        
        std::vector<RunningApp> runningApps = GetRunningApps(groupIndex);
        for (const auto& app : runningApps) {
            if (app.processId == foregroundProcessId && app.windowHandle == foregroundWindow) {
                Wh_Log(L"Window from group %s is already foreground, no action", group.name.c_str());
                return; // No action needed
            }
        }
    }
    
    // Step 2: Check if any apps in group are running
    std::vector<RunningApp> runningApps = GetRunningApps(groupIndex);
    
    if (!runningApps.empty()) {
        // Find most recently active window and focus it
        HWND targetWindow = GetMostRecentWindow(groupIndex);
        if (targetWindow) {
            Wh_Log(L"Focusing window for group %s", group.name.c_str());
            FocusWindow(targetWindow);
            return;
        }
    }
    
    // Step 3: Launch preferred app (or first in list)
    std::wstring appToLaunch;
    if (!group.preferredApp.empty()) {
        appToLaunch = group.preferredApp;
    } else if (!group.apps.empty()) {
        appToLaunch = group.apps[0];
    } else {
        Wh_Log(L"No apps configured for group %s", group.name.c_str());
        return;
    }
    
    Wh_Log(L"Launching app for group %s: %s", group.name.c_str(), appToLaunch.c_str());
    LaunchApp(appToLaunch);
}

// Show context menu with groups
void ShowGroupMenu(HWND hwnd) {
    Wh_Log(L"ShowGroupMenu called with hwnd=%p", hwnd);
    
    if (g_settings.groups.empty()) {
        Wh_Log(L"Warning: No groups configured!");
        return;
    }
    
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        Wh_Log(L"Failed to create popup menu");
        return;
    }
    
    for (size_t i = 0; i < g_settings.groups.size(); i++) {
        AppendMenuW(menu, MF_STRING, 1000 + (int)i, g_settings.groups[i].name.c_str());
    }
    
    POINT pt;
    GetCursorPos(&pt);
    
    // Required when showing a menu from tray: set our window foreground so the menu appears
    SetForegroundWindow(hwnd);
    int cmd = TrackPopupMenu(
        menu,
        TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_LEFTALIGN,
        pt.x,
        pt.y,
        0,
        hwnd,
        NULL
    );
    
    if (cmd >= 1000) {
        int index = cmd - 1000;
        HandleGroupAction(index);
    }
    
    DestroyMenu(menu);
}

// Window procedure for the dedicated tray icon window (receives tray notifications)
LRESULT CALLBACK TrayIconWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == TRAY_ICON_CALLBACK_MSG) {
        Wh_Log(L"Tray icon callback! wParam=%d, lParam=%d (0x%X)", wParam, lParam, lParam);
        
        if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDOWN ||
            lParam == WM_RBUTTONUP || lParam == WM_RBUTTONDOWN) {
            const wchar_t* buttonType =
                (lParam == WM_LBUTTONUP) ? L"Left Up" :
                (lParam == WM_LBUTTONDOWN) ? L"Left Down" :
                (lParam == WM_RBUTTONUP) ? L"Right Up" : L"Right Down";
            Wh_Log(L"Tray icon click: %s, showing menu...", buttonType);
            ShowGroupMenu(hwnd);
        } else {
            Wh_Log(L"Tray message (not click): lParam=0x%X", lParam);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Create the hidden window used for tray icon callbacks
// Use a normal overlapped window (not message-only); the shell may not deliver
// tray notifications to HWND_MESSAGE windows.
BOOL CreateTrayIconWindow() {
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = TrayIconWindowProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = TRAY_ICON_WINDOW_CLASS;
    if (!RegisterClassExW(&wc)) {
        Wh_Log(L"Failed to register tray window class");
        return FALSE;
    }
    g_trayIconWindow = CreateWindowExW(
        0,
        TRAY_ICON_WINDOW_CLASS,
        L"Taskbar Groups Tray",
        WS_OVERLAPPED,
        0, 0, 1, 1,
        NULL,
        NULL,
        GetModuleHandle(NULL),
        NULL
    );
    if (!g_trayIconWindow) {
        Wh_Log(L"Failed to create tray icon window");
        UnregisterClassW(TRAY_ICON_WINDOW_CLASS, GetModuleHandle(NULL));
        return FALSE;
    }
    ShowWindow(g_trayIconWindow, SW_HIDE);
    Wh_Log(L"Tray icon window created: %p", g_trayIconWindow);
    return TRUE;
}

void DestroyTrayIconWindow() {
    if (g_trayIconWindow) {
        DestroyWindow(g_trayIconWindow);
        g_trayIconWindow = NULL;
        UnregisterClassW(TRAY_ICON_WINDOW_CLASS, GetModuleHandle(NULL));
        Wh_Log(L"Tray icon window destroyed");
    }
}

// System tray icon (uses g_trayIconWindow so notifications are delivered reliably)
void CreateTrayIcon() {
    if (!g_trayIconWindow) {
        Wh_Log(L"Cannot create tray icon: tray window not created");
        return;
    }
    g_notifyIcon.cbSize = sizeof(NOTIFYICONDATAW);
    g_notifyIcon.hWnd = g_trayIconWindow;
    g_notifyIcon.uID = 1;
    g_notifyIcon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_notifyIcon.uCallbackMessage = TRAY_ICON_CALLBACK_MSG;
    g_notifyIcon.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wcscpy_s(g_notifyIcon.szTip, L"Taskbar Groups");

    if (!Shell_NotifyIconW(NIM_ADD, &g_notifyIcon)) {
        Wh_Log(L"Shell_NotifyIconW(NIM_ADD) failed, error=%lu", GetLastError());
    } else {
        Wh_Log(L"System tray icon created");
    }
}

void RemoveTrayIcon() {
    if (g_notifyIcon.uID != 0) {
        Shell_NotifyIconW(NIM_DELETE, &g_notifyIcon);
        Wh_Log(L"System tray icon removed");
        g_notifyIcon.uID = 0;
    }
}

// Dedicated thread: creates tray window + icon and runs a message loop so tray callbacks are received
static DWORD WINAPI TrayThreadProc(LPVOID) {
    Wh_Log(L"Tray thread started");
    if (!CreateTrayIconWindow()) {
        Wh_Log(L"Tray thread: CreateTrayIconWindow failed");
        return 1;
    }
    CreateTrayIcon();
    Wh_Log(L"Tray thread: icon created, running message loop");

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        DispatchMessageW(&msg);
    }

    RemoveTrayIcon();
    DestroyTrayIconWindow();
    Wh_Log(L"Tray thread exiting");
    return 0;
}

// Taskbar subclass procedure to intercept Ctrl+Right-click
LRESULT CALLBACK TaskbarSubclassProc(
    HWND hwnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR uIdSubclass,
    DWORD_PTR dwRefData
) {
    // Log all right-button events for debugging
    if (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) {
        bool ctrlPressed = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        Wh_Log(L"Taskbar right-click detected! Ctrl pressed: %d", ctrlPressed);
    }
    
    if (msg == WM_RBUTTONDOWN) {
        // Check if Ctrl key is pressed
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            Wh_Log(L"Ctrl+Right-click detected! Showing group menu...");
            ShowGroupMenu(hwnd);
            return 0;
        }
    }
    
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// Load settings from Windhawk
void LoadSettings() {
    g_settings.groups.clear();
    
    Wh_Log(L"Loading settings...");
    
    // Parse groups using flat structure: group0Name, group0Apps, etc.
    int groupIndex = 0;
    while (true) {
        // Try to load group name
        std::wstring nameKey = L"group" + std::to_wstring(groupIndex) + L"Name";
        PCWSTR name = Wh_GetStringSetting(nameKey.c_str());
        
        if (!name || wcslen(name) == 0) {
            break; // No more groups
        }
        
        AppGroup group;
        group.name = name;
        
        Wh_Log(L"Loading group %d: %s", groupIndex, name);
        
        // Load apps (semicolon-separated string)
        std::wstring appsKey = L"group" + std::to_wstring(groupIndex) + L"Apps";
        PCWSTR appsStr = Wh_GetStringSetting(appsKey.c_str());
        
        if (appsStr && wcslen(appsStr) > 0) {
            // Parse semicolon-separated apps
            std::wstring appsString = appsStr;
            size_t pos = 0;
            while ((pos = appsString.find(L';')) != std::wstring::npos) {
                std::wstring app = appsString.substr(0, pos);
                if (!app.empty()) {
                    group.apps.push_back(app);
                    Wh_Log(L"  App: %s", app.c_str());
                }
                appsString.erase(0, pos + 1);
            }
            // Add last app
            if (!appsString.empty()) {
                group.apps.push_back(appsString);
                Wh_Log(L"  App: %s", appsString.c_str());
            }
        }
        
        // Load preferredApp
        std::wstring preferredKey = L"group" + std::to_wstring(groupIndex) + L"PreferredApp";
        PCWSTR preferred = Wh_GetStringSetting(preferredKey.c_str());
        if (preferred && wcslen(preferred) > 0) {
            group.preferredApp = preferred;
            Wh_Log(L"  Preferred app: %s", preferred);
        } else if (!group.apps.empty()) {
            group.preferredApp = group.apps[0]; // Default to first app
            Wh_Log(L"  Preferred app (default): %s", group.preferredApp.c_str());
        }
        
        // Icon support can be added later with groupNIcon key
        // For now, leave icon empty
        
        if (!group.apps.empty()) {
            g_settings.groups.push_back(group);
        }
        
        groupIndex++;
    }
    
    Wh_Log(L"Loaded %d groups", (int)g_settings.groups.size());
}

// The mod is being initialized, load settings, hook functions, and do other
// initialization stuff if required.
BOOL Wh_ModInit() {
    Wh_Log(L"Taskbar Groups Mod loaded successfully!");
    
    LoadSettings();
    
    // Find taskbar first so we can create the tray icon on the main UI thread
    // (tray callbacks are delivered to the thread that created the icon's window)
    g_taskbarWindow = FindWindowW(L"Shell_TrayWnd", NULL);
    if (!g_taskbarWindow) {
        Wh_Log(L"Warning: Could not find taskbar window");
        return TRUE;
    }
    Wh_Log(L"Found taskbar window: %p", g_taskbarWindow);

    HWND child = GetWindow(g_taskbarWindow, GW_CHILD);
    int childIndex = 0;
    HWND rebarChild = NULL;
    HWND coreChild = NULL;

    while (child) {
        WCHAR className[256];
        GetClassNameW(child, className, 256);
        Wh_Log(L"Taskbar child %d: %p (class: %s)", childIndex, child, className);

        if (_wcsicmp(className, L"ReBarWindow32") == 0) {
            rebarChild = child;
        } else if (_wcsicmp(className, L"Windows.UI.Core.CoreWindow") == 0 ||
                   _wcsicmp(className, L"Windows.UI.Composition.DesktopWindowContentBridge") == 0) {
            coreChild = child;
        }

        child = GetWindow(child, GW_HWNDNEXT);
        childIndex++;
    }

    SetWindowSubclass(g_taskbarWindow, TaskbarSubclassProc, 1, 0);
    Wh_Log(L"Taskbar window subclassed successfully");

    if (rebarChild) {
        SetWindowSubclass(rebarChild, TaskbarSubclassProc, 2, 0);
        Wh_Log(L"ReBarWindow32 child subclassed: %p", rebarChild);
    }

    if (coreChild) {
        SetWindowSubclass(coreChild, TaskbarSubclassProc, 3, 0);
        Wh_Log(L"CoreWindow/Bridge child subclassed: %p", coreChild);
    }

    // Tray icon lives on a dedicated thread with its own message loop so shell callbacks are received
    g_trayThread = CreateThread(NULL, 0, TrayThreadProc, NULL, 0, &g_trayThreadId);
    if (!g_trayThread) {
        Wh_Log(L"Failed to create tray thread, error=%lu", GetLastError());
    } else {
        Wh_Log(L"Tray thread created: id=%lu", g_trayThreadId);
    }

    return TRUE;
}

// The mod is being unloaded, free all allocated resources.
void Wh_ModUninit() {
    Wh_Log(L"Uninit");

    // Signal tray thread to quit; it removes the icon and destroys the window before exiting
    if (g_trayThread && g_trayThreadId) {
        PostThreadMessageW(g_trayThreadId, WM_QUIT, 0, 0);
        if (WaitForSingleObject(g_trayThread, 5000) != WAIT_OBJECT_0) {
            Wh_Log(L"Tray thread did not exit in time");
        }
        CloseHandle(g_trayThread);
        g_trayThread = NULL;
        g_trayThreadId = 0;
    }

    if (g_taskbarWindow) {
        RemoveWindowSubclass(g_taskbarWindow, TaskbarSubclassProc, 1);

        HWND child = GetWindow(g_taskbarWindow, GW_CHILD);
        while (child) {
            WCHAR className[256];
            GetClassNameW(child, className, 256);

            if (_wcsicmp(className, L"ReBarWindow32") == 0) {
                RemoveWindowSubclass(child, TaskbarSubclassProc, 2);
            } else if (_wcsicmp(className, L"Windows.UI.Core.CoreWindow") == 0 ||
                       _wcsicmp(className, L"Windows.UI.Composition.DesktopWindowContentBridge") == 0) {
                RemoveWindowSubclass(child, TaskbarSubclassProc, 3);
            }

            child = GetWindow(child, GW_HWNDNEXT);
        }
    }
}

// The mod setting were changed, reload them.
void Wh_ModSettingsChanged() {
    Wh_Log(L"SettingsChanged");
    
    LoadSettings();
}
