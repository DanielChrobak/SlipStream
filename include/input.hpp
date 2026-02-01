#pragma once

#include "common.hpp"

#pragma pack(push, 1)
struct MouseMoveMsg { uint32_t magic; float x, y; };
struct MouseMoveRelMsg { uint32_t magic; int16_t dx, dy; };
struct MouseBtnMsg { uint32_t magic; uint8_t button, action; };
struct MouseWheelMsg { uint32_t magic; int16_t deltaX, deltaY; float x, y; };
struct KeyMsg { uint32_t magic; uint16_t keyCode, scanCode; uint8_t action; };
#pragma pack(pop)

inline WORD JsKeyToVK(uint16_t k) {
    if ((k >= 65 && k <= 90) || (k >= 48 && k <= 57)) return (WORD)k;
    static const std::unordered_map<uint16_t, WORD> m = {
        {8,VK_BACK},{9,VK_TAB},{13,VK_RETURN},{16,VK_SHIFT},{17,VK_CONTROL},{18,VK_MENU},
        {19,VK_PAUSE},{20,VK_CAPITAL},{27,VK_ESCAPE},{32,VK_SPACE},{33,VK_PRIOR},{34,VK_NEXT},
        {35,VK_END},{36,VK_HOME},{37,VK_LEFT},{38,VK_UP},{39,VK_RIGHT},{40,VK_DOWN},
        {44,VK_SNAPSHOT},{45,VK_INSERT},{46,VK_DELETE},{91,VK_LWIN},{92,VK_RWIN},
        {96,VK_NUMPAD0},{97,VK_NUMPAD1},{98,VK_NUMPAD2},{99,VK_NUMPAD3},{100,VK_NUMPAD4},
        {101,VK_NUMPAD5},{102,VK_NUMPAD6},{103,VK_NUMPAD7},{104,VK_NUMPAD8},{105,VK_NUMPAD9},
        {106,VK_MULTIPLY},{107,VK_ADD},{109,VK_SUBTRACT},{110,VK_DECIMAL},{111,VK_DIVIDE},
        {112,VK_F1},{113,VK_F2},{114,VK_F3},{115,VK_F4},{116,VK_F5},{117,VK_F6},
        {118,VK_F7},{119,VK_F8},{120,VK_F9},{121,VK_F10},{122,VK_F11},{123,VK_F12},
        {144,VK_NUMLOCK},{145,VK_SCROLL},{186,VK_OEM_1},{187,VK_OEM_PLUS},
        {188,VK_OEM_COMMA},{189,VK_OEM_MINUS},{190,VK_OEM_PERIOD},{191,VK_OEM_2},
        {192,VK_OEM_3},{219,VK_OEM_4},{220,VK_OEM_5},{221,VK_OEM_6},{222,VK_OEM_7}
    };
    auto it = m.find(k);
    return it != m.end() ? it->second : 0;
}

class InputHandler {
    std::atomic<int> monX{0}, monY{0}, monW{1920}, monH{1080};
    std::atomic<bool> enabled{false}, ctrlDown{false}, altDown{false};
    std::atomic<int64_t> rateStart{0};
    std::atomic<int> moveCnt{0}, clickCnt{0}, keyCnt{0};
    static constexpr int MAX_MOVES = 500, MAX_CLICKS = 50, MAX_KEYS = 100;

    void ResetWindow() {
        int64_t now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        if (now - rateStart.load() >= 1000) { rateStart = now; moveCnt = clickCnt = keyCnt = 0; }
    }

    bool CheckLimit(std::atomic<int>& cnt, int max) { ResetWindow(); return cnt.fetch_add(1) < max; }

    void ToAbs(float nx, float ny, LONG& ax, LONG& ay) {
        int px = monX + (int)(std::clamp(nx, 0.f, 1.f) * monW);
        int py = monY + (int)(std::clamp(ny, 0.f, 1.f) * monH);
        ax = (LONG)((px - GetSystemMetrics(SM_XVIRTUALSCREEN)) * 65535 / GetSystemMetrics(SM_CXVIRTUALSCREEN));
        ay = (LONG)((py - GetSystemMetrics(SM_YVIRTUALSCREEN)) * 65535 / GetSystemMetrics(SM_CYVIRTUALSCREEN));
    }

    static bool IsExtKey(WORD vk) {
        return vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END ||
               vk == VK_PRIOR || vk == VK_NEXT || vk == VK_LEFT || vk == VK_RIGHT ||
               vk == VK_UP || vk == VK_DOWN || vk == VK_LWIN || vk == VK_RWIN ||
               vk == VK_APPS || vk == VK_DIVIDE || vk == VK_NUMLOCK;
    }

    bool IsBlocked(WORD vk, bool down) {
        if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) ctrlDown = down;
        if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) altDown = down;
        return vk == VK_LWIN || vk == VK_RWIN || (ctrlDown && altDown && vk == VK_DELETE);
    }

public:
    void SetMonitorBounds(int x, int y, int w, int h) { monX = x; monY = y; monW = w; monH = h; }

    void UpdateFromMonitorInfo(const MonitorInfo& info) {
        MONITORINFO mi{sizeof(mi)};
        if (GetMonitorInfo(info.hMon, &mi))
            SetMonitorBounds(mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top);
    }

    void Enable() { enabled = true; }

    void WiggleCenter() {
        if (!enabled) return;
        LONG ax, ay; ToAbs(0.5f, 0.5f, ax, ay);
        INPUT in{INPUT_MOUSE}; in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        in.mi.dx = ax; in.mi.dy = ay;
        SendInput(1, &in, sizeof(INPUT));
    }

    void MouseMove(float nx, float ny) {
        if (!enabled || !CheckLimit(moveCnt, MAX_MOVES)) return;
        LONG ax, ay; ToAbs(nx, ny, ax, ay);
        INPUT in{INPUT_MOUSE}; in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        in.mi.dx = ax; in.mi.dy = ay;
        SendInput(1, &in, sizeof(INPUT));
    }

    void MouseMoveRel(int16_t dx, int16_t dy) {
        if (!enabled || !CheckLimit(moveCnt, MAX_MOVES) || (dx == 0 && dy == 0)) return;
        INPUT in{INPUT_MOUSE}; in.mi.dwFlags = MOUSEEVENTF_MOVE; in.mi.dx = dx; in.mi.dy = dy;
        SendInput(1, &in, sizeof(INPUT));
    }

    void MouseButton(uint8_t btn, bool down) {
        if (!enabled || btn > 4 || !CheckLimit(clickCnt, MAX_CLICKS)) return;
        static const DWORD flags[5][2] = {
            {MOUSEEVENTF_LEFTUP, MOUSEEVENTF_LEFTDOWN}, {MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_RIGHTDOWN},
            {MOUSEEVENTF_MIDDLEUP, MOUSEEVENTF_MIDDLEDOWN}, {MOUSEEVENTF_XUP, MOUSEEVENTF_XDOWN}, {MOUSEEVENTF_XUP, MOUSEEVENTF_XDOWN}
        };
        INPUT in{INPUT_MOUSE}; in.mi.dwFlags = flags[btn][down ? 1 : 0];
        if (btn >= 3) in.mi.mouseData = btn == 3 ? XBUTTON1 : XBUTTON2;
        SendInput(1, &in, sizeof(INPUT));
    }

    void MouseWheel(int16_t dx, int16_t dy) {
        if (!enabled || !CheckLimit(clickCnt, MAX_CLICKS)) return;
        if (dy) { INPUT in{INPUT_MOUSE}; in.mi.dwFlags = MOUSEEVENTF_WHEEL; in.mi.mouseData = (DWORD)(-dy * WHEEL_DELTA / 100); SendInput(1, &in, sizeof(INPUT)); }
        if (dx) { INPUT in{INPUT_MOUSE}; in.mi.dwFlags = MOUSEEVENTF_HWHEEL; in.mi.mouseData = (DWORD)(dx * WHEEL_DELTA / 100); SendInput(1, &in, sizeof(INPUT)); }
    }

    void Key(uint16_t jsKey, uint16_t scan, bool down) {
        if (!enabled || !CheckLimit(keyCnt, MAX_KEYS)) return;
        WORD vk = JsKeyToVK(jsKey);
        if (!vk || IsBlocked(vk, down)) return;
        INPUT in{INPUT_KEYBOARD};
        in.ki.wVk = vk;
        in.ki.wScan = scan ? scan : (WORD)MapVirtualKey(vk, MAPVK_VK_TO_VSC);
        in.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP) | (IsExtKey(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
        SendInput(1, &in, sizeof(INPUT));
    }

    bool HandleMessage(const uint8_t* data, size_t len) {
        if (len < 4) return false;
        uint32_t magic = *(const uint32_t*)data;
        switch (magic) {
            case MSG_MOUSE_MOVE: if (len >= sizeof(MouseMoveMsg)) { auto* m = (const MouseMoveMsg*)data; MouseMove(m->x, m->y); return true; } break;
            case MSG_MOUSE_MOVE_REL: if (len >= sizeof(MouseMoveRelMsg)) { auto* m = (const MouseMoveRelMsg*)data; MouseMoveRel(m->dx, m->dy); return true; } break;
            case MSG_MOUSE_BTN: if (len >= sizeof(MouseBtnMsg)) { auto* m = (const MouseBtnMsg*)data; MouseButton(m->button, m->action != 0); return true; } break;
            case MSG_MOUSE_WHEEL: if (len >= 8) { auto* m = (const MouseWheelMsg*)data; MouseWheel(m->deltaX, m->deltaY); return true; } break;
            case MSG_KEY: if (len >= sizeof(KeyMsg)) { auto* m = (const KeyMsg*)data; Key(m->keyCode, m->scanCode, m->action != 0); return true; } break;
        }
        return false;
    }
};
