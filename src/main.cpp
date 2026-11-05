#include "recovery_state.h"

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "UxTheme.lib")

namespace {
constexpr wchar_t kWindowClass[] = L"Redlight.MainWindow";
constexpr wchar_t kAppName[] = L"Redlight";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kToggleHotkey = 1;
constexpr UINT kRestoreHotkey = 2;
constexpr UINT kToggleCommand = 1001;
constexpr UINT kRestoreCommand = 1002;
constexpr UINT kChangeHotkeyCommand = 1003;
constexpr UINT kStartupCommand = 1004;
constexpr UINT kMinimizeToTrayCommand = 1005;
constexpr UINT kModeCommand = 1006;
constexpr UINT kIntensityControl = 1007;
constexpr UINT kTrayOpen = 2001;
constexpr UINT kTrayToggle = 2002;
constexpr UINT kTrayExit = 2003;
constexpr UINT kMinimumWidth = 560;
constexpr UINT kMinimumHeight = 530;
constexpr UINT kDefaultIntensity = 85;
constexpr UINT kDefaultHotkeyModifiers = MOD_CONTROL | MOD_ALT;
constexpr UINT kDefaultHotkeyKey = 'R';
constexpr UINT kEmergencyModifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
constexpr UINT kEmergencyKey = 'R';
constexpr int kDarkWindowAttribute = 20;

const COLORREF kBackground = RGB(26, 23, 28);
const COLORREF kPanel = RGB(37, 32, 39);
const COLORREF kText = RGB(245, 239, 242);
const COLORREF kMutedText = RGB(174, 164, 171);
const COLORREF kAccent = RGB(208, 49, 66);
const COLORREF kAccentHover = RGB(230, 64, 81);

struct Settings {
    int intensity = kDefaultIntensity;
    int mode = 0;
    bool startWithWindows = false;
    bool minimizeToTray = true;
    UINT hotkeyModifiers = kDefaultHotkeyModifiers;
    UINT hotkeyKey = kDefaultHotkeyKey;
};

struct App {
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    HWND toggleButton = nullptr;
    HWND intensitySlider = nullptr;
    HWND intensityValue = nullptr;
    HWND modeCombo = nullptr;
    HWND startupCheck = nullptr;
    HWND trayCheck = nullptr;
    HWND hotkeyValue = nullptr;
    HWND statusLabel = nullptr;
    HFONT font = nullptr;
    HFONT titleFont = nullptr;
    HBRUSH backgroundBrush = nullptr;
    HBRUSH panelBrush = nullptr;
    NOTIFYICONDATAW tray{};
    Settings settings;
    RecoveryState* recoveryState = nullptr;
    HANDLE recoveryMapping = nullptr;
    HANDLE recoveryProcess = nullptr;
    bool filterEnabled = false;
    bool magInitialized = false;
    bool capturingHotkey = false;
    bool shuttingDown = false;
    bool emergencyHotkeyAvailable = false;
    bool toggleHotkeyAvailable = false;
    MAGCOLOREFFECT originalEffect{};
    float originalMagnification = 1.0f;
    int originalXOffset = 0;
    int originalYOffset = 0;
    std::wstring settingsPath;
};

App* g_app = nullptr;

MAGCOLOREFFECT IdentityEffect() {
    MAGCOLOREFFECT effect{};
    for (int index = 0; index < 5; ++index) effect.transform[index][index] = 1.0f;
    return effect;
}

MAGCOLOREFFECT BuildEffect(int intensityPercent, int mode) {
    const float t = std::clamp(intensityPercent, 0, 100) / 100.0f;
    MAGCOLOREFFECT effect = IdentityEffect();
    if (mode == 0) {
        // Red-only mode: preserve red and suppress green and blue.
        effect.transform[1][1] = 1.0f - t;
        effect.transform[2][2] = 1.0f - t;
    } else {
        // Red-blend mode: fade green/blue while lifting red toward full intensity.
        effect.transform[0][0] = 1.0f - t;
        effect.transform[1][1] = 1.0f - t;
        effect.transform[2][2] = 1.0f - t;
        effect.transform[4][0] = t;
    }
    return effect;
}

bool IsIdentityEffect(const MAGCOLOREFFECT& effect) {
    const MAGCOLOREFFECT identity = IdentityEffect();
    for (int row = 0; row < 5; ++row) {
        for (int column = 0; column < 5; ++column) {
            if (std::fabs(effect.transform[row][column] - identity.transform[row][column]) > 0.0001f)
                return false;
        }
    }
    return true;
}

std::wstring GetModulePath() {
    wchar_t buffer[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(_countof(buffer)));
    return length ? std::wstring(buffer, length) : std::wstring();
}

std::wstring ParentDirectory(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

std::wstring GetSettingsPath() {
    PWSTR appData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &appData)))
        return L"Redlight.ini";

    std::wstring folder(appData);
    CoTaskMemFree(appData);
    folder += L"\\Redlight";
    CreateDirectoryW(folder.c_str(), nullptr);
    return folder + L"\\settings.ini";
}

int ReadInt(const std::wstring& path, const wchar_t* key, int fallback) {
    return static_cast<int>(GetPrivateProfileIntW(L"Settings", key, fallback, path.c_str()));
}

void WriteInt(const std::wstring& path, const wchar_t* key, int value) {
    wchar_t text[32]{};
    swprintf_s(text, L"%d", value);
    WritePrivateProfileStringW(L"Settings", key, text, path.c_str());
}

void LoadSettings(App& app) {
    app.settingsPath = GetSettingsPath();
    app.settings.intensity = std::clamp(ReadInt(app.settingsPath, L"Intensity", kDefaultIntensity), 0, 100);
    app.settings.mode = std::clamp(ReadInt(app.settingsPath, L"Mode", 0), 0, 1);
    app.settings.startWithWindows = ReadInt(app.settingsPath, L"StartWithWindows", 0) != 0;
    app.settings.minimizeToTray = ReadInt(app.settingsPath, L"MinimizeToTray", 1) != 0;
    app.settings.hotkeyModifiers = static_cast<UINT>(ReadInt(app.settingsPath, L"HotkeyModifiers", kDefaultHotkeyModifiers));
    app.settings.hotkeyKey = static_cast<UINT>(ReadInt(app.settingsPath, L"HotkeyKey", kDefaultHotkeyKey));
}

void SaveSetting(App& app, const wchar_t* key, int value) {
    WriteInt(app.settingsPath, key, value);
}

void SetStatus(App& app, const std::wstring& text) {
    if (app.statusLabel) SetWindowTextW(app.statusLabel, text.c_str());
}

std::wstring KeyName(UINT key) {
    UINT scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC);
    LONG keyBits = static_cast<LONG>(scan << 16);
    if (key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN ||
        key == VK_HOME || key == VK_END || key == VK_PRIOR || key == VK_NEXT ||
        key == VK_INSERT || key == VK_DELETE) keyBits |= 1L << 24;
    wchar_t name[80]{};
    if (GetKeyNameTextW(keyBits, name, static_cast<int>(_countof(name))) > 0) return name;
    return L"Key";
}

std::wstring HotkeyName(UINT modifiers, UINT key) {
    std::wstring text;
    if (modifiers & MOD_CONTROL) text += L"Ctrl+";
    if (modifiers & MOD_ALT) text += L"Alt+";
    if (modifiers & MOD_SHIFT) text += L"Shift+";
    if (modifiers & MOD_WIN) text += L"Win+";
    text += KeyName(key);
    return text;
}

void UpdateHotkeyLabel(App& app) {
    if (app.hotkeyValue) {
        const std::wstring text = HotkeyName(app.settings.hotkeyModifiers, app.settings.hotkeyKey);
        SetWindowTextW(app.hotkeyValue, text.c_str());
    }
}

void UpdateToggleButton(App& app) {
    if (app.toggleButton) SetWindowTextW(app.toggleButton, app.filterEnabled ? L"Disable red filter" : L"Enable red filter");
    if (app.tray.cbSize) {
        wcsncpy_s(app.tray.szTip, app.filterEnabled ? L"Redlight is on" : L"Redlight is off", _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &app.tray);
    }
}

bool StartRecoveryHelper(App& app) {
    wchar_t mappingName[96]{};
    MakeRecoveryMappingName(GetCurrentProcessId(), mappingName, _countof(mappingName));

    app.recoveryMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, sizeof(RecoveryState), mappingName);
    if (!app.recoveryMapping) return false;

    app.recoveryState = static_cast<RecoveryState*>(MapViewOfFile(app.recoveryMapping,
        FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(RecoveryState)));
    if (!app.recoveryState) return false;
    ZeroMemory(app.recoveryState, sizeof(RecoveryState));
    app.recoveryState->signature = kRecoverySignature;

    const std::wstring helperPath = ParentDirectory(GetModulePath()) + L"\\RedlightRecovery.exe";
    const std::wstring commandLine = L"\"" + helperPath + L"\" " + std::to_wstring(GetCurrentProcessId());
    std::wstring mutableCommandLine = commandLine;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(helperPath.c_str(), mutableCommandLine.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, ParentDirectory(helperPath).c_str(), &startup, &process)) return false;

    CloseHandle(process.hThread);
    app.recoveryProcess = process.hProcess;
    return true;
}

bool RestoreSavedColors(App& app, bool keepMagInitialized) {
    if (!app.magInitialized) {
        return !app.recoveryState ||
            InterlockedCompareExchange(&app.recoveryState->filterActive, 0, 0) == 0;
    }
    const BOOL effectRestored = MagSetFullscreenColorEffect(&app.originalEffect);
    const BOOL transformRestored = MagSetFullscreenTransform(app.originalMagnification,
        app.originalXOffset, app.originalYOffset);
    if (effectRestored && transformRestored) {
        if (app.recoveryState) InterlockedExchange(&app.recoveryState->filterActive, 0);
        app.filterEnabled = false;
    }
    if (!keepMagInitialized) {
        MagUninitialize();
        app.magInitialized = false;
    }
    return effectRestored && transformRestored;
}

bool DisableFilter(App& app, bool showError) {
    if (!app.filterEnabled && !app.magInitialized) return true;
    const bool restored = RestoreSavedColors(app, false);
    if (app.recoveryState && restored) InterlockedExchange(&app.recoveryState->filterActive, 0);
    UpdateToggleButton(app);
    if (restored) {
        SetStatus(app, L"Original desktop colors restored.");
    } else {
        SetStatus(app, L"Windows could not confirm color restoration. Use Ctrl+Alt+Shift+R again.");
        if (showError) MessageBoxW(app.window,
            L"Windows did not confirm that the original display colors were restored. Try the emergency shortcut again.",
            kAppName, MB_OK | MB_ICONWARNING);
    }
    return restored;
}

bool ApplyCurrentEffect(App& app) {
    if (!app.filterEnabled) return true;
    MAGCOLOREFFECT effect = BuildEffect(app.settings.intensity, app.settings.mode);
    if (MagSetFullscreenColorEffect(&effect)) return true;
    SetStatus(app, L"Windows rejected the color update. Restoring original colors.");
    DisableFilter(app, true);
    return false;
}

bool EnableFilter(App& app) {
    if (app.filterEnabled) return true;
    if (!app.recoveryProcess || WaitForSingleObject(app.recoveryProcess, 0) != WAIT_TIMEOUT) {
        SetStatus(app, L"Recovery helper is unavailable. Restart Redlight before enabling the filter.");
        MessageBoxW(app.window,
            L"The recovery helper could not be started. Keep RedlightRecovery.exe beside Redlight.exe and restart the app before enabling the filter.",
            kAppName, MB_OK | MB_ICONERROR);
        return false;
    }
    if (!MagInitialize()) {
        SetStatus(app, L"Windows Magnification API could not be initialized.");
        MessageBoxW(app.window,
            L"Windows could not initialize its full-screen magnifier. The desktop was not changed.",
            kAppName, MB_OK | MB_ICONERROR);
        return false;
    }
    app.magInitialized = true;

    if (!MagGetFullscreenColorEffect(&app.originalEffect) ||
        !MagGetFullscreenTransform(&app.originalMagnification, &app.originalXOffset, &app.originalYOffset)) {
        MagUninitialize();
        app.magInitialized = false;
        SetStatus(app, L"Windows could not read the current desktop color state.");
        MessageBoxW(app.window, L"Windows could not save the current display settings. The filter was not enabled.",
            kAppName, MB_OK | MB_ICONERROR);
        return false;
    }

    if (!IsIdentityEffect(app.originalEffect) ||
        std::fabs(app.originalMagnification - 1.0f) > 0.0001f ||
        app.originalXOffset != 0 || app.originalYOffset != 0) {
        MagUninitialize();
        app.magInitialized = false;
        SetStatus(app, L"Another full-screen magnification effect is already active.");
        MessageBoxW(app.window,
            L"Another app is already using the Windows full-screen Magnification API. Turn off Windows Magnifier or that app first, then try again.",
            kAppName, MB_OK | MB_ICONWARNING);
        return false;
    }

    app.recoveryState->originalEffect = app.originalEffect;
    app.recoveryState->originalMagnification = app.originalMagnification;
    app.recoveryState->originalXOffset = app.originalXOffset;
    app.recoveryState->originalYOffset = app.originalYOffset;
    InterlockedExchange(&app.recoveryState->filterActive, 1);

    MAGCOLOREFFECT effect = BuildEffect(app.settings.intensity, app.settings.mode);
    if (!MagSetFullscreenTransform(1.0f, 0, 0) || !MagSetFullscreenColorEffect(&effect)) {
        RestoreSavedColors(app, false);
        SetStatus(app, L"Windows could not apply the full-screen color filter.");
        MessageBoxW(app.window,
            L"Windows could not apply the full-screen color filter. Redlight attempted to restore the original colors.",
            kAppName, MB_OK | MB_ICONERROR);
        UpdateToggleButton(app);
        return false;
    }

    app.filterEnabled = true;
    UpdateToggleButton(app);
    SetStatus(app, L"Red tint is active across the Windows desktop.");
    return true;
}

void ToggleFilter(App& app) {
    if (app.filterEnabled) DisableFilter(app, false);
    else EnableFilter(app);
}

bool SetStartupRegistration(bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0,
        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;

    bool success = false;
    if (enabled) {
        const std::wstring command = L"\"" + GetModulePath() + L"\" --startup";
        success = RegSetValueExW(key, L"Redlight", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    } else {
        const LONG result = RegDeleteValueW(key, L"Redlight");
        success = result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
    }
    RegCloseKey(key);
    return success;
}

void SaveIntensity(App& app, int value) {
    app.settings.intensity = std::clamp(value, 0, 100);
    SaveSetting(app, L"Intensity", app.settings.intensity);
    wchar_t text[32]{};
    swprintf_s(text, L"%d%%", app.settings.intensity);
    SetWindowTextW(app.intensityValue, text);
    if (app.filterEnabled) ApplyCurrentEffect(app);
}

void SaveMode(App& app, int value) {
    app.settings.mode = std::clamp(value, 0, 1);
    SaveSetting(app, L"Mode", app.settings.mode);
    if (app.filterEnabled) ApplyCurrentEffect(app);
}

bool RegisterUserHotkey(App& app, UINT modifiers, UINT key) {
    if (!key || key == VK_CONTROL || key == VK_MENU || key == VK_SHIFT || key == VK_LWIN || key == VK_RWIN)
        return false;
    if (modifiers == 0) return false;
    if (modifiers == kEmergencyModifiers && key == kEmergencyKey) return false;

    if (app.toggleHotkeyAvailable) UnregisterHotKey(app.window, kToggleHotkey);
    app.toggleHotkeyAvailable = false;
    if (!RegisterHotKey(app.window, kToggleHotkey, modifiers | MOD_NOREPEAT, key)) {
        if (RegisterHotKey(app.window, kToggleHotkey,
            app.settings.hotkeyModifiers | MOD_NOREPEAT, app.settings.hotkeyKey))
            app.toggleHotkeyAvailable = true;
        return false;
    }

    app.settings.hotkeyModifiers = modifiers;
    app.settings.hotkeyKey = key;
    app.toggleHotkeyAvailable = true;
    SaveSetting(app, L"HotkeyModifiers", static_cast<int>(modifiers));
    SaveSetting(app, L"HotkeyKey", static_cast<int>(key));
    UpdateHotkeyLabel(app);
    return true;
}

void BeginHotkeyCapture(App& app) {
    app.capturingHotkey = true;
    if (app.toggleHotkeyAvailable) {
        UnregisterHotKey(app.window, kToggleHotkey);
        app.toggleHotkeyAvailable = false;
    }
    SetStatus(app, L"Press a shortcut with Ctrl, Alt, Shift, or Windows. Esc cancels.");
    SetFocus(app.window);
}

void EndHotkeyCapture(App& app) {
    app.capturingHotkey = false;
    if (!app.toggleHotkeyAvailable) {
        app.toggleHotkeyAvailable = RegisterHotKey(app.window, kToggleHotkey,
            app.settings.hotkeyModifiers | MOD_NOREPEAT, app.settings.hotkeyKey) != FALSE;
    }
}

void ShowSettings(App& app) {
    ShowWindow(app.window, SW_SHOW);
    ShowWindow(app.window, SW_RESTORE);
    SetForegroundWindow(app.window);
}

void ShowTrayMenu(App& app) {
    POINT point{};
    GetCursorPos(&point);
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kTrayToggle, app.filterEnabled ? L"Disable Red Filter" : L"Enable Red Filter");
    AppendMenuW(menu, MF_STRING, kTrayOpen, L"Open Settings");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayExit, L"Exit");
    SetForegroundWindow(app.window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, point.x, point.y, 0, app.window, nullptr);
    DestroyMenu(menu);
    PostMessageW(app.window, WM_NULL, 0, 0);
}

HWND CreateControl(App& app, const wchar_t* className, const wchar_t* text,
    DWORD style, int x, int y, int width, int height, UINT id) {
    HWND control = CreateWindowExW(0, className, text, WS_CHILD | WS_VISIBLE | style,
        x, y, width, height, app.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        app.instance, nullptr);
    if (control && app.font) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(app.font), TRUE);
    if (control) SetWindowTheme(control, L"DarkMode_Explorer", nullptr);
    return control;
}

void CreateSettingsUi(App& app) {
    app.font = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    app.titleFont = CreateFontW(-30, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    app.backgroundBrush = CreateSolidBrush(kBackground);
    app.panelBrush = CreateSolidBrush(kPanel);

    HWND title = CreateControl(app, L"STATIC", kAppName, SS_LEFT, 28, 22, 440, 42, 0);
    SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(app.titleFont), TRUE);
    CreateControl(app, L"STATIC", L"A system-wide redlight filter for your desktop", SS_LEFT, 30, 67, 480, 28, 0);

    app.toggleButton = CreateControl(app, L"BUTTON", L"Enable red filter", BS_OWNERDRAW | WS_TABSTOP,
        30, 112, 500, 48, kToggleCommand);

    CreateControl(app, L"STATIC", L"Intensity", SS_LEFT, 30, 182, 100, 28, 0);
    app.intensityValue = CreateControl(app, L"STATIC", L"85%", SS_RIGHT, 450, 182, 78, 28, 0);
    wchar_t savedIntensityText[32]{};
    swprintf_s(savedIntensityText, L"%d%%", app.settings.intensity);
    SetWindowTextW(app.intensityValue, savedIntensityText);
    app.intensitySlider = CreateControl(app, TRACKBAR_CLASSW, L"", TBS_AUTOTICKS | TBS_NOTICKS | WS_TABSTOP,
        28, 208, 504, 36, kIntensityControl);
    SendMessageW(app.intensitySlider, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
    SendMessageW(app.intensitySlider, TBM_SETPOS, TRUE, app.settings.intensity);

    CreateControl(app, L"STATIC", L"Color mode", SS_LEFT, 30, 266, 170, 28, 0);
    app.modeCombo = CreateControl(app, L"COMBOBOX", L"",
        CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 210, 258, 320, 180, kModeCommand);
    SendMessageW(app.modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Red-only filter"));
    SendMessageW(app.modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Red tint blend"));
    SendMessageW(app.modeCombo, CB_SETCURSEL, app.settings.mode, 0);

    app.startupCheck = CreateControl(app, L"BUTTON", L"Start with Windows", BS_AUTOCHECKBOX | WS_TABSTOP,
        30, 320, 235, 32, kStartupCommand);
    SendMessageW(app.startupCheck, BM_SETCHECK, app.settings.startWithWindows ? BST_CHECKED : BST_UNCHECKED, 0);
    app.trayCheck = CreateControl(app, L"BUTTON", L"Minimize to system tray", BS_AUTOCHECKBOX | WS_TABSTOP,
        275, 320, 255, 32, kMinimizeToTrayCommand);
    SendMessageW(app.trayCheck, BM_SETCHECK, app.settings.minimizeToTray ? BST_CHECKED : BST_UNCHECKED, 0);

    CreateControl(app, L"STATIC", L"Toggle shortcut", SS_LEFT, 30, 370, 150, 28, 0);
    app.hotkeyValue = CreateControl(app, L"STATIC", L"", SS_LEFT, 185, 370, 160, 28, 0);
    UpdateHotkeyLabel(app);
    CreateControl(app, L"BUTTON", L"Change shortcut...", BS_OWNERDRAW | WS_TABSTOP,
        350, 362, 180, 36, kChangeHotkeyCommand);

    app.statusLabel = CreateControl(app, L"STATIC", L"Filter is off. Emergency restore: Ctrl+Alt+Shift+R.",
        SS_LEFT, 30, 421, 500, 44, 0);
    CreateControl(app, L"BUTTON", L"Restore original colors", BS_OWNERDRAW | WS_TABSTOP,
        30, 474, 500, 40, kRestoreCommand);
    UpdateToggleButton(app);
}

bool AddTrayIcon(App& app) {
    app.tray.cbSize = sizeof(app.tray);
    app.tray.hWnd = app.window;
    app.tray.uID = 1;
    app.tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    app.tray.uCallbackMessage = kTrayMessage;
    app.tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcsncpy_s(app.tray.szTip, L"Redlight is off", _TRUNCATE);
    if (!Shell_NotifyIconW(NIM_ADD, &app.tray)) return false;
    app.tray.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &app.tray);
    return true;
}

bool RegisterHotkeys(App& app) {
    app.emergencyHotkeyAvailable = RegisterHotKey(app.window, kRestoreHotkey,
        kEmergencyModifiers | MOD_NOREPEAT, kEmergencyKey) != FALSE;
    app.toggleHotkeyAvailable = RegisterHotKey(app.window, kToggleHotkey,
        app.settings.hotkeyModifiers | MOD_NOREPEAT, app.settings.hotkeyKey) != FALSE;
    if (!app.emergencyHotkeyAvailable) {
        SetStatus(app, L"Emergency restore shortcut is busy. Use Restore original colors or the tray menu.");
    } else if (!app.toggleHotkeyAvailable) {
        SetStatus(app, L"Toggle shortcut is busy. Change it in Settings.");
    }
    return app.emergencyHotkeyAvailable;
}

void ExitApp(App& app) {
    if (app.shuttingDown) return;
    app.shuttingDown = true;
    if (app.filterEnabled || app.magInitialized) DisableFilter(app, true);
    DestroyWindow(app.window);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = static_cast<App*>(create->lpCreateParams);
        app->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window, message, wParam, lParam);

    switch (message) {
    case WM_CREATE: {
        CreateSettingsUi(*app);
        if (!StartRecoveryHelper(*app)) {
            SetStatus(*app, L"Recovery helper did not start. Keep RedlightRecovery.exe beside this app.");
        }
        RegisterHotkeys(*app);
        AddTrayIcon(*app);
        return 0;
    }
    case WM_HOTKEY:
        if (wParam == kRestoreHotkey) {
            if (app->filterEnabled) DisableFilter(*app, false);
        } else if (wParam == kToggleHotkey && !app->capturingHotkey) {
            ToggleFilter(*app);
        }
        return 0;
    case WM_KEYDOWN:
        if (app->capturingHotkey) {
            if (wParam == VK_ESCAPE) {
                EndHotkeyCapture(*app);
                SetStatus(*app, L"Shortcut change canceled.");
                return 0;
            }
            if (wParam == VK_CONTROL || wParam == VK_MENU || wParam == VK_SHIFT ||
                wParam == VK_LWIN || wParam == VK_RWIN) return 0;
            UINT modifiers = 0;
            if (GetAsyncKeyState(VK_CONTROL) & 0x8000) modifiers |= MOD_CONTROL;
            if (GetAsyncKeyState(VK_MENU) & 0x8000) modifiers |= MOD_ALT;
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000) modifiers |= MOD_SHIFT;
            if ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) modifiers |= MOD_WIN;
            if (RegisterUserHotkey(*app, modifiers, static_cast<UINT>(wParam))) {
                SetStatus(*app, L"Toggle shortcut saved.");
            } else {
                SetStatus(*app, L"That shortcut is unavailable or needs a modifier. Try another.");
            }
            EndHotkeyCapture(*app);
            return 0;
        }
        break;
    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lParam) == app->intensitySlider)
            SaveIntensity(*app, static_cast<int>(SendMessageW(app->intensitySlider, TBM_GETPOS, 0, 0)));
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kToggleCommand:
            ToggleFilter(*app);
            return 0;
        case kRestoreCommand:
            DisableFilter(*app, true);
            return 0;
        case kChangeHotkeyCommand:
            if (HIWORD(wParam) == BN_CLICKED) BeginHotkeyCapture(*app);
            return 0;
        case kStartupCommand:
            if (HIWORD(wParam) == BN_CLICKED) {
                const bool enabled = SendMessageW(app->startupCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
                if (!SetStartupRegistration(enabled)) {
                    SendMessageW(app->startupCheck, BM_SETCHECK, enabled ? BST_UNCHECKED : BST_CHECKED, 0);
                    SetStatus(*app, L"Could not update the Windows startup setting.");
                } else {
                    app->settings.startWithWindows = enabled;
                    SaveSetting(*app, L"StartWithWindows", enabled ? 1 : 0);
                    SetStatus(*app, enabled ? L"Redlight will start when you sign in." : L"Windows startup disabled.");
                }
            }
            return 0;
        case kMinimizeToTrayCommand:
            if (HIWORD(wParam) == BN_CLICKED) {
                app->settings.minimizeToTray = SendMessageW(app->trayCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
                SaveSetting(*app, L"MinimizeToTray", app->settings.minimizeToTray ? 1 : 0);
            }
            return 0;
        case kModeCommand:
            if (HIWORD(wParam) == CBN_SELCHANGE)
                SaveMode(*app, static_cast<int>(SendMessageW(app->modeCombo, CB_GETCURSEL, 0, 0)));
            return 0;
        case kTrayOpen:
            ShowSettings(*app);
            return 0;
        case kTrayToggle:
            ToggleFilter(*app);
            return 0;
        case kTrayExit:
            ExitApp(*app);
            return 0;
        }
        break;
    case WM_DRAWITEM: {
        auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (!draw || draw->CtlType != ODT_BUTTON) break;
        const bool primary = draw->CtlID == kToggleCommand;
        const bool pressed = (draw->itemState & ODS_SELECTED) != 0;
        HBRUSH brush = CreateSolidBrush(primary ? (pressed ? kAccentHover : kAccent) : (pressed ? RGB(68, 55, 64) : kPanel));
        FillRect(draw->hDC, &draw->rcItem, brush);
        DeleteObject(brush);
        SetBkMode(draw->hDC, TRANSPARENT);
        SetTextColor(draw->hDC, kText);
        wchar_t text[128]{};
        GetWindowTextW(draw->hwndItem, text, static_cast<int>(_countof(text)));
        DrawTextW(draw->hDC, text, -1, &draw->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (draw->itemState & ODS_FOCUS) DrawFocusRect(draw->hDC, &draw->rcItem);
        return TRUE;
    }
    case WM_CTLCOLORSTATIC:
        SetBkColor(reinterpret_cast<HDC>(wParam), kBackground);
        SetTextColor(reinterpret_cast<HDC>(wParam), GetDlgCtrlID(reinterpret_cast<HWND>(lParam)) == 0 ? kMutedText : kText);
        return reinterpret_cast<LRESULT>(app->backgroundBrush);
    case WM_CTLCOLORBTN:
        SetBkColor(reinterpret_cast<HDC>(wParam), kBackground);
        SetTextColor(reinterpret_cast<HDC>(wParam), kText);
        return reinterpret_cast<LRESULT>(app->backgroundBrush);
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetBkColor(reinterpret_cast<HDC>(wParam), kPanel);
        SetTextColor(reinterpret_cast<HDC>(wParam), kText);
        return reinterpret_cast<LRESULT>(app->panelBrush);
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED && app->settings.minimizeToTray) ShowWindow(window, SW_HIDE);
        return 0;
    case WM_DISPLAYCHANGE:
        if (app->filterEnabled) {
            MagSetFullscreenTransform(1.0f, 0, 0);
            ApplyCurrentEffect(*app);
        }
        return 0;
    case WM_POWERBROADCAST:
        if (wParam == PBT_APMRESUMEAUTOMATIC && app->filterEnabled) {
            MagSetFullscreenTransform(1.0f, 0, 0);
            ApplyCurrentEffect(*app);
        }
        return TRUE;
    case kTrayMessage:
        if (lParam == WM_LBUTTONUP || lParam == NIN_SELECT || lParam == NIN_KEYSELECT) ShowSettings(*app);
        else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) ShowTrayMenu(*app);
        return 0;
    case WM_CLOSE:
        if (app->settings.minimizeToTray && !app->shuttingDown) {
            ShowWindow(window, SW_HIDE);
            return 0;
        }
        ExitApp(*app);
        return 0;
    case WM_QUERYENDSESSION:
        DisableFilter(*app, false);
        return TRUE;
    case WM_DESTROY:
        if (app->filterEnabled || app->magInitialized) DisableFilter(*app, false);
        UnregisterHotKey(window, kToggleHotkey);
        UnregisterHotKey(window, kRestoreHotkey);
        if (app->tray.cbSize) Shell_NotifyIconW(NIM_DELETE, &app->tray);
        if (app->font) DeleteObject(app->font);
        if (app->titleFont) DeleteObject(app->titleFont);
        if (app->backgroundBrush) DeleteObject(app->backgroundBrush);
        if (app->panelBrush) DeleteObject(app->panelBrush);
        if (app->recoveryState) {
            UnmapViewOfFile(app->recoveryState);
            app->recoveryState = nullptr;
        }
        if (app->recoveryMapping) CloseHandle(app->recoveryMapping);
        if (app->recoveryProcess) {
            WaitForSingleObject(app->recoveryProcess, 1200);
            CloseHandle(app->recoveryProcess);
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int showCommand) {
    HANDLE singleInstance = CreateMutexW(nullptr, TRUE, L"Local\\Redlight.SingleInstance");
    if (!singleInstance || GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(kWindowClass, kAppName);
        if (existing) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        if (singleInstance) CloseHandle(singleInstance);
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX commonControls{ sizeof(commonControls), ICC_BAR_CLASSES };
    InitCommonControlsEx(&commonControls);

    App app{};
    app.instance = instance;
    LoadSettings(app);

    WNDCLASSEXW windowClass{ sizeof(windowClass) };
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.hbrBackground = CreateSolidBrush(kBackground);
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass)) {
        CloseHandle(singleInstance);
        return 1;
    }

    RECT bounds{ 0, 0, kMinimumWidth, kMinimumHeight };
    AdjustWindowRect(&bounds, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    HWND window = CreateWindowExW(WS_EX_APPWINDOW, kWindowClass, kAppName,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top,
        nullptr, nullptr, instance, &app);
    if (!window) {
        DeleteObject(windowClass.hbrBackground);
        UnregisterClassW(kWindowClass, instance);
        CloseHandle(singleInstance);
        return 2;
    }

    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(window, kDarkWindowAttribute, &darkMode, sizeof(darkMode));
    g_app = &app;

    const bool startInTray = wcsstr(commandLine, L"--startup") != nullptr && app.settings.minimizeToTray;
    if (startInTray) ShowWindow(window, SW_HIDE);
    else ShowWindow(window, showCommand);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    DeleteObject(windowClass.hbrBackground);
    UnregisterClassW(kWindowClass, instance);
    CloseHandle(singleInstance);
    g_app = nullptr;
    return static_cast<int>(message.wParam);
}
