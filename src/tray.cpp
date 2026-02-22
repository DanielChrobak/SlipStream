#include "common.hpp"
#include "app_support.hpp"
#include "tray.hpp"
#include <shellapi.h>

namespace {
constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT TRAY_ICON_ID = 1001;
constexpr UINT IDM_TRAY_OPEN = 2001;
constexpr UINT IDM_TRAY_EXIT = 2002;

HWND g_consoleWnd = nullptr;
HWND g_trayWnd = nullptr;

void RestoreFromTray() {
    if(!g_consoleWnd) return;
    ShowWindow(g_consoleWnd, SW_SHOW);
    ShowWindow(g_consoleWnd, SW_RESTORE);
    SetForegroundWindow(g_consoleWnd);
}

void DisableConsoleCloseButton() {
    if(!g_consoleWnd) return;
    HMENU sysMenu = GetSystemMenu(g_consoleWnd, FALSE);
    if(!sysMenu) return;
    EnableMenuItem(sysMenu, SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);
    DrawMenuBar(g_consoleWnd);
}

void RequestFullExit() {
    g_exitRequested.store(true, std::memory_order_release);
    g_running.store(false, std::memory_order_release);
}

void ShowTrayMenu(HWND wnd) {
    HMENU menu = CreatePopupMenu();
    if(!menu) return;

    AppendMenuW(menu, MF_STRING, IDM_TRAY_OPEN, L"Open SlipStream");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"Exit SlipStream");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(wnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                   pt.x, pt.y, 0, wnd, nullptr);
    PostMessageW(wnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

LRESULT CALLBACK TrayWindowProc(HWND wnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch(msg) {
        case WM_COMMAND:
            switch(LOWORD(wParam)) {
                case IDM_TRAY_OPEN:
                    RestoreFromTray();
                    return 0;
                case IDM_TRAY_EXIT:
                    RequestFullExit();
                    return 0;
            }
            break;
        case WM_TRAYICON:
            {
                UINT evt = LOWORD(lParam);
                UINT idLegacy = LOWORD(wParam);
                UINT idV4 = HIWORD(lParam);
                if(idLegacy == TRAY_ICON_ID || idV4 == TRAY_ICON_ID) {
                if(evt == WM_RBUTTONUP || evt == WM_CONTEXTMENU) {
                    ShowTrayMenu(wnd);
                    return 0;
                }
                }
            }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(wnd, msg, wParam, lParam);
}

}

bool InitAppTray() {
    g_consoleWnd = GetConsoleWindow();

    const wchar_t* clsName = L"SlipStreamTrayWindowClass";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TrayWindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = clsName;

    if(!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        ERR("Tray: RegisterClassExW failed: %lu", GetLastError());
        return false;
    }

    g_trayWnd = CreateWindowExW(0, clsName, L"SlipStreamTray", WS_OVERLAPPED,
                                0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if(!g_trayWnd) {
        ERR("Tray: CreateWindowExW failed: %lu", GetLastError());
        return false;
    }

    HICON icon = reinterpret_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    if(!icon) icon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_trayWnd;
    nid.uID = TRAY_ICON_ID;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = icon;
    wcscpy_s(nid.szTip, L"SlipStream");

    if(!Shell_NotifyIconW(NIM_ADD, &nid)) {
        ERR("Tray: Shell_NotifyIconW(NIM_ADD) failed");
        DestroyWindow(g_trayWnd);
        g_trayWnd = nullptr;
        return false;
    }

    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    DisableConsoleCloseButton();
    HideAppToTray();
    return true;
}

void PumpAppTrayMessages() {
    MSG msg;
    while(PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if(g_consoleWnd && IsWindow(g_consoleWnd) && IsWindowVisible(g_consoleWnd) && IsIconic(g_consoleWnd)) {
        HideAppToTray();
    }
}

void HideAppToTray() {
    if(g_consoleWnd) ShowWindow(g_consoleWnd, SW_HIDE);
}

void CleanupAppTray() {
    if(g_trayWnd) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = g_trayWnd;
        nid.uID = TRAY_ICON_ID;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        DestroyWindow(g_trayWnd);
        g_trayWnd = nullptr;
    }
}
