#include "input.hpp"
#include <array>
#include <cstring>

namespace {
    const std::unordered_map<uint16_t, WORD> JS_VK_MAP = {
        {8,VK_BACK}, {9,VK_TAB}, {13,VK_RETURN}, {16,VK_SHIFT}, {17,VK_CONTROL}, {18,VK_MENU},
        {19,VK_PAUSE}, {20,VK_CAPITAL}, {27,VK_ESCAPE}, {32,VK_SPACE}, {33,VK_PRIOR}, {34,VK_NEXT},
        {35,VK_END}, {36,VK_HOME}, {37,VK_LEFT}, {38,VK_UP}, {39,VK_RIGHT}, {40,VK_DOWN},
        {44,VK_SNAPSHOT}, {45,VK_INSERT}, {46,VK_DELETE}, {91,VK_LWIN}, {92,VK_RWIN},
        {96,VK_NUMPAD0}, {97,VK_NUMPAD1}, {98,VK_NUMPAD2}, {99,VK_NUMPAD3}, {100,VK_NUMPAD4},
        {101,VK_NUMPAD5}, {102,VK_NUMPAD6}, {103,VK_NUMPAD7}, {104,VK_NUMPAD8}, {105,VK_NUMPAD9},
        {106,VK_MULTIPLY}, {107,VK_ADD}, {109,VK_SUBTRACT}, {110,VK_DECIMAL}, {111,VK_DIVIDE},
        {112,VK_F1}, {113,VK_F2}, {114,VK_F3}, {115,VK_F4}, {116,VK_F5}, {117,VK_F6},
        {118,VK_F7}, {119,VK_F8}, {120,VK_F9}, {121,VK_F10}, {122,VK_F11}, {123,VK_F12},
        {144,VK_NUMLOCK}, {145,VK_SCROLL}, {186,VK_OEM_1}, {187,VK_OEM_PLUS}, {188,VK_OEM_COMMA},
        {189,VK_OEM_MINUS}, {190,VK_OEM_PERIOD}, {191,VK_OEM_2}, {192,VK_OEM_3}, {219,VK_OEM_4},
        {220,VK_OEM_5}, {221,VK_OEM_6}, {222,VK_OEM_7}
    };

    HCURSOR GetStdCursor(int i) {
        static const std::array<LPCTSTR, 13> ids = {
            IDC_ARROW, IDC_IBEAM, IDC_HAND, IDC_WAIT, IDC_APPSTARTING, IDC_CROSS, IDC_SIZEALL,
            IDC_SIZEWE, IDC_SIZENS, IDC_SIZENWSE, IDC_SIZENESW, IDC_NO, IDC_HELP
        };
        static HCURSOR cache[13] = {};
        if (!cache[0]) {
            for (size_t j = 0; j < ids.size(); ++j) cache[j] = LoadCursor(nullptr, ids[j]);
        }
        return cache[i];
    }

    template <typename T>
    [[nodiscard]] bool ReadPod(const uint8_t* data, size_t len, T& out) {
        if (len < sizeof(T)) return false;
        std::memcpy(&out, data, sizeof(T));
        return true;
    }

    bool IsExtendedKey(WORD vk) {
        return vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END ||
               vk == VK_PRIOR || vk == VK_NEXT || vk == VK_LEFT || vk == VK_RIGHT ||
               vk == VK_UP || vk == VK_DOWN || vk == VK_LWIN || vk == VK_RWIN ||
               vk == VK_APPS || vk == VK_DIVIDE || vk == VK_NUMLOCK;
    }
}

WORD JsKeyToVK(uint16_t k) {
    if ((k >= 65 && k <= 90) || (k >= 48 && k <= 57)) return static_cast<WORD>(k);
    auto it = JS_VK_MAP.find(k);
    return it != JS_VK_MAP.end() ? it->second : 0;
}

bool InputHandler::CheckLimit(std::atomic<int>& cnt, int max, std::atomic<uint64_t>& dropped) {
    int64_t now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    if (now - rateStart.load() >= 1000) {
        rateStart = now;
        moveCnt = clickCnt = keyCnt = 0;
    }
    if (cnt.fetch_add(1) >= max) { dropped++; return false; }
    return true;
}

void InputHandler::ToAbsolute(float nx, float ny, LONG& ax, LONG& ay) {
    int px = monX + static_cast<int>(std::clamp(nx, 0.f, 1.f) * monW);
    int py = monY + static_cast<int>(std::clamp(ny, 0.f, 1.f) * monH);
    ax = static_cast<LONG>((px - GetSystemMetrics(SM_XVIRTUALSCREEN)) * 65535 / GetSystemMetrics(SM_CXVIRTUALSCREEN));
    ay = static_cast<LONG>((py - GetSystemMetrics(SM_YVIRTUALSCREEN)) * 65535 / GetSystemMetrics(SM_CYVIRTUALSCREEN));
}

bool InputHandler::IsBlocked(WORD vk, bool down) {
    if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) ctrlDown = down;
    if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) altDown = down;
    bool blocked = (vk == VK_LWIN || vk == VK_RWIN || (ctrlDown && altDown && vk == VK_DELETE));
    if (blocked && down) blockedKeys++;
    return blocked;
}

void InputHandler::SetMonitorBounds(int x, int y, int w, int h) {
    monX = x; monY = y; monW = w; monH = h;
    DBG("InputHandler: Monitor bounds %d,%d %dx%d", x, y, w, h);
}

void InputHandler::UpdateFromMonitorInfo(const MonitorInfo& info) {
    MONITORINFO mi{sizeof(mi)};
    if (GetMonitorInfo(info.hMon, &mi)) {
        SetMonitorBounds(mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top);
    }
}

bool InputHandler::GetCurrentCursor(CursorType& out) {
    CURSORINFO ci = {sizeof(ci)};
    if (!GetCursorInfo(&ci)) { out = CURSOR_DEFAULT; return false; }
    if (!(ci.flags & CURSOR_SHOWING)) { out = CURSOR_NONE; return out != lastCur.exchange(out); }
    CursorType nc = CURSOR_CUSTOM;
    for (int i = 0; i < 13; i++) if (ci.hCursor == GetStdCursor(i)) { nc = static_cast<CursorType>(i); break; }
    out = nc;
    return nc != lastCur.exchange(nc);
}

void InputHandler::WiggleCenter() {
    if (!enabled) return;
    LONG ax, ay;
    ToAbsolute(0.5f, 0.5f, ax, ay);
    INPUT in{INPUT_MOUSE};
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    in.mi.dx = ax; in.mi.dy = ay;
    SendInput(1, &in, sizeof(INPUT));
}

void InputHandler::MouseMove(float nx, float ny) {
    if (!enabled || !CheckLimit(moveCnt, MAX_MV, droppedMoves)) return;
    LONG ax, ay;
    ToAbsolute(nx, ny, ax, ay);
    INPUT in{INPUT_MOUSE};
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    in.mi.dx = ax; in.mi.dy = ay;
    if (SendInput(1, &in, sizeof(INPUT))) totalMoves++;
}

void InputHandler::MouseMoveRel(int16_t dx, int16_t dy) {
    if (!enabled || !CheckLimit(moveCnt, MAX_MV, droppedMoves) || (dx == 0 && dy == 0)) return;
    INPUT in{INPUT_MOUSE};
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    in.mi.dx = dx; in.mi.dy = dy;
    if (SendInput(1, &in, sizeof(INPUT))) totalMoves++;
}

void InputHandler::MouseButton(uint8_t btn, bool down) {
    if (!enabled || btn > 4 || !CheckLimit(clickCnt, MAX_CL, droppedClicks)) return;
    static const DWORD flags[5][2] = {
        {MOUSEEVENTF_LEFTUP, MOUSEEVENTF_LEFTDOWN}, {MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_RIGHTDOWN},
        {MOUSEEVENTF_MIDDLEUP, MOUSEEVENTF_MIDDLEDOWN}, {MOUSEEVENTF_XUP, MOUSEEVENTF_XDOWN},
        {MOUSEEVENTF_XUP, MOUSEEVENTF_XDOWN}
    };
    INPUT in{INPUT_MOUSE};
    in.mi.dwFlags = flags[btn][down ? 1 : 0];
    if (btn >= 3) in.mi.mouseData = btn == 3 ? XBUTTON1 : XBUTTON2;
    if (SendInput(1, &in, sizeof(INPUT))) totalClicks++;
}

void InputHandler::MouseWheel(int16_t dx, int16_t dy) {
    if (!enabled || !CheckLimit(clickCnt, MAX_CL, droppedClicks)) return;
    INPUT in{INPUT_MOUSE};
    if (dy) { in.mi.dwFlags = MOUSEEVENTF_WHEEL; in.mi.mouseData = static_cast<DWORD>(-dy * WHEEL_DELTA / 100); SendInput(1, &in, sizeof(INPUT)); }
    if (dx) { in.mi.dwFlags = MOUSEEVENTF_HWHEEL; in.mi.mouseData = static_cast<DWORD>(dx * WHEEL_DELTA / 100); SendInput(1, &in, sizeof(INPUT)); }
}

void InputHandler::Key(uint16_t jsKey, uint16_t scan, bool down) {
    if (!enabled || !CheckLimit(keyCnt, MAX_KY, droppedKeys)) return;
    WORD vk = JsKeyToVK(jsKey);
    if (!vk) return;
    if (IsBlocked(vk, down)) return;
    INPUT in{INPUT_KEYBOARD};
    in.ki.wVk = vk;
    in.ki.wScan = scan ? scan : static_cast<WORD>(MapVirtualKey(vk, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP) | (IsExtendedKey(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
    if (SendInput(1, &in, sizeof(INPUT))) totalKeys++;
}

bool InputHandler::HandleMessage(const uint8_t* data, size_t len) {
    if (len < 4) return false;
    uint32_t magic = 0;
    if (!ReadPod(data, len, magic)) return false;

    switch (magic) {
        case MSG_MOUSE_MOVE:
            if (MouseMoveMsg m{}; ReadPod(data, len, m)) { MouseMove(m.x, m.y); return true; }
            break;
        case MSG_MOUSE_MOVE_REL:
            if (MouseMoveRelMsg m{}; ReadPod(data, len, m)) { MouseMoveRel(m.dx, m.dy); return true; }
            break;
        case MSG_MOUSE_BTN:
            if (MouseBtnMsg m{}; ReadPod(data, len, m)) { MouseButton(m.button, m.action != 0); return true; }
            break;
        case MSG_MOUSE_WHEEL:
            if (MouseWheelMsg m{}; ReadPod(data, len, m)) { MouseWheel(m.deltaX, m.deltaY); return true; }
            break;
        case MSG_KEY:
            if (KeyMsg m{}; ReadPod(data, len, m)) { Key(m.keyCode, m.scanCode, m.action != 0); return true; }
            break;
    }
    return false;
}

bool InputHandler::SetClipboardText(const std::string& text) {
    if (text.empty() || text.size() > 1048576) return false;
    if (!OpenClipboard(nullptr)) return false;
    bool ok = false;
    if (EmptyClipboard()) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
        if (wlen > 0) {
            if (HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, wlen * sizeof(wchar_t))) {
                if (wchar_t* p = static_cast<wchar_t*>(GlobalLock(hMem))) {
                    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, p, wlen);
                    GlobalUnlock(hMem);
                    ok = SetClipboardData(CF_UNICODETEXT, hMem) != nullptr;
                }
                if (!ok) GlobalFree(hMem);
            }
        }
    }
    CloseClipboard();
    return ok;
}

std::string InputHandler::GetClipboardText() {
    if (!OpenClipboard(nullptr) || !IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        CloseClipboard();
        return "";
    }
    std::string result;
    if (HANDLE hData = GetClipboardData(CF_UNICODETEXT)) {
        if (wchar_t* pW = static_cast<wchar_t*>(GlobalLock(hData))) {
            int len = WideCharToMultiByte(CP_UTF8, 0, pW, -1, nullptr, 0, nullptr, nullptr);
            if (len > 0) {
                result.resize(len - 1);
                WideCharToMultiByte(CP_UTF8, 0, pW, -1, result.data(), len, nullptr, nullptr);
            }
            GlobalUnlock(hData);
        }
    }
    CloseClipboard();
    return result;
}

void InputHandler::GetStats(uint64_t& moves, uint64_t& clicks, uint64_t& keys,
                            uint64_t& dMoves, uint64_t& dClicks, uint64_t& dKeys, uint64_t& blocked) {
    moves = totalMoves; clicks = totalClicks; keys = totalKeys;
    dMoves = droppedMoves; dClicks = droppedClicks; dKeys = droppedKeys; blocked = blockedKeys;
}
