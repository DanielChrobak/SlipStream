#include "input.hpp"

WORD JsKeyToVK(uint16_t k) {
    if((k >= 65 && k <= 90) || (k >= 48 && k <= 57)) return (WORD)k;

    static const std::unordered_map<uint16_t, WORD> m = {
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
    auto it = m.find(k);
    return it != m.end() ? it->second : 0;
}

HCURSOR InputHandler::GetStdCursor(int i) {
    static const LPCTSTR ids[] = {
        IDC_ARROW, IDC_IBEAM, IDC_HAND, IDC_WAIT, IDC_APPSTARTING, IDC_CROSS, IDC_SIZEALL,
        IDC_SIZEWE, IDC_SIZENS, IDC_SIZENWSE, IDC_SIZENESW, IDC_NO, IDC_HELP
    };
    static HCURSOR cache[13] = {};
    if(!cache[0]) for(int j=0; j<13; j++) cache[j] = LoadCursor(nullptr, ids[j]);
    return cache[i];
}

void InputHandler::ResetWin() {
    int64_t now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    if(now - rateStart.load() >= 1000) {
        rateStart = now;
        int droppedM = moveCnt.load() - MAX_MV;
        int droppedC = clickCnt.load() - MAX_CL;
        int droppedK = keyCnt.load() - MAX_KY;
        if(droppedM > 0 || droppedC > 0 || droppedK > 0) {
            DBG("InputHandler: Rate limit hit - dropped moves:%d clicks:%d keys:%d", std::max(0, droppedM), std::max(0, droppedC), std::max(0, droppedK));
        }
        moveCnt = clickCnt = keyCnt = 0;
    }
}

bool InputHandler::ChkLim(std::atomic<int>& c, int max, std::atomic<uint64_t>& dropped) {
    ResetWin();
    if(c.fetch_add(1) >= max) { dropped++; return false; }
    return true;
}

void InputHandler::ToAbs(float nx, float ny, LONG& ax, LONG& ay) {
    int px = monX + (int)(std::clamp(nx, 0.f, 1.f) * monW);
    int py = monY + (int)(std::clamp(ny, 0.f, 1.f) * monH);
    ax = (LONG)((px - GetSystemMetrics(SM_XVIRTUALSCREEN)) * 65535 / GetSystemMetrics(SM_CXVIRTUALSCREEN));
    ay = (LONG)((py - GetSystemMetrics(SM_YVIRTUALSCREEN)) * 65535 / GetSystemMetrics(SM_CYVIRTUALSCREEN));
}

bool InputHandler::IsExt(WORD vk) {
    return vk==VK_INSERT || vk==VK_DELETE || vk==VK_HOME || vk==VK_END ||
           vk==VK_PRIOR || vk==VK_NEXT || vk==VK_LEFT || vk==VK_RIGHT ||
           vk==VK_UP || vk==VK_DOWN || vk==VK_LWIN || vk==VK_RWIN ||
           vk==VK_APPS || vk==VK_DIVIDE || vk==VK_NUMLOCK;
}

bool InputHandler::IsBlocked(WORD vk, bool down) {
    if(vk==VK_CONTROL || vk==VK_LCONTROL || vk==VK_RCONTROL) ctrlDown = down;
    if(vk==VK_MENU || vk==VK_LMENU || vk==VK_RMENU) altDown = down;
    bool blocked = (vk==VK_LWIN || vk==VK_RWIN || (ctrlDown && altDown && vk==VK_DELETE));
    if(blocked && down) {
        blockedKeys++;
        DBG("InputHandler: Blocked key VK=0x%02X (Ctrl:%d Alt:%d)", vk, ctrlDown.load(), altDown.load());
    }
    return blocked;
}

bool InputHandler::DoSendInput(INPUT* in, UINT count) {
    UINT sent = SendInput(count, in, sizeof(INPUT));
    if(sent != count) {
        DWORD err = GetLastError();
        if(err != 0) DBG("InputHandler: SendInput failed, sent %u/%u, error: %lu", sent, count, err);
        return false;
    }
    return true;
}

void InputHandler::SetMonitorBounds(int x, int y, int w, int h) {
    monX = x; monY = y; monW = w; monH = h;
    DBG("InputHandler: Monitor bounds set to %d,%d %dx%d", x, y, w, h);
}

void InputHandler::UpdateFromMonitorInfo(const MonitorInfo& info) {
    MONITORINFO mi{sizeof(mi)};
    if(GetMonitorInfo(info.hMon, &mi)) {
        SetMonitorBounds(mi.rcMonitor.left, mi.rcMonitor.top,
                        mi.rcMonitor.right - mi.rcMonitor.left,
                        mi.rcMonitor.bottom - mi.rcMonitor.top);
    } else {
        WARN("InputHandler: GetMonitorInfo failed: %lu", GetLastError());
    }
}

void InputHandler::Enable() { enabled = true; LOG("InputHandler: Enabled"); }

bool InputHandler::GetCurrentCursor(CursorType& out) {
    CURSORINFO ci = {sizeof(ci)};
    if(!GetCursorInfo(&ci)) {
        DBG("InputHandler: GetCursorInfo failed: %lu", GetLastError());
        out = CURSOR_DEFAULT;
        return false;
    }
    if(!(ci.flags & CURSOR_SHOWING)) {
        out = CURSOR_NONE;
        return out != lastCur.exchange(out);
    }
    CursorType nc = CURSOR_CUSTOM;
    for(int i=0; i<13; i++) if(ci.hCursor == GetStdCursor(i)) { nc = (CursorType)i; break; }
    out = nc;
    return nc != lastCur.exchange(nc);
}

void InputHandler::WiggleCenter() {
    if(!enabled) return;
    LONG ax, ay;
    ToAbs(0.5f, 0.5f, ax, ay);
    INPUT in{INPUT_MOUSE};
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    in.mi.dx = ax;
    in.mi.dy = ay;
    DoSendInput(&in, 1);
}

void InputHandler::MouseMove(float nx, float ny) {
    if(!enabled || !ChkLim(moveCnt, MAX_MV, droppedMoves)) return;
    LONG ax, ay;
    ToAbs(nx, ny, ax, ay);
    INPUT in{INPUT_MOUSE};
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    in.mi.dx = ax;
    in.mi.dy = ay;
    if(DoSendInput(&in, 1)) totalMoves++;
}

void InputHandler::MouseMoveRel(int16_t dx, int16_t dy) {
    if(!enabled || !ChkLim(moveCnt, MAX_MV, droppedMoves) || (dx==0 && dy==0)) return;
    INPUT in{INPUT_MOUSE};
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    if(DoSendInput(&in, 1)) totalMoves++;
}

void InputHandler::MouseButton(uint8_t btn, bool down) {
    if(!enabled || btn > 4 || !ChkLim(clickCnt, MAX_CL, droppedClicks)) return;

    static const DWORD fl[5][2] = {
        {MOUSEEVENTF_LEFTUP, MOUSEEVENTF_LEFTDOWN},
        {MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_RIGHTDOWN},
        {MOUSEEVENTF_MIDDLEUP, MOUSEEVENTF_MIDDLEDOWN},
        {MOUSEEVENTF_XUP, MOUSEEVENTF_XDOWN},
        {MOUSEEVENTF_XUP, MOUSEEVENTF_XDOWN}
    };

    INPUT in{INPUT_MOUSE};
    in.mi.dwFlags = fl[btn][down ? 1 : 0];
    if(btn >= 3) in.mi.mouseData = btn==3 ? XBUTTON1 : XBUTTON2;
    if(DoSendInput(&in, 1)) totalClicks++;
}

void InputHandler::MouseWheel(int16_t dx, int16_t dy) {
    if(!enabled || !ChkLim(clickCnt, MAX_CL, droppedClicks)) return;
    if(dy) {
        INPUT in{INPUT_MOUSE};
        in.mi.dwFlags = MOUSEEVENTF_WHEEL;
        in.mi.mouseData = (DWORD)(-dy * WHEEL_DELTA / 100);
        DoSendInput(&in, 1);
    }
    if(dx) {
        INPUT in{INPUT_MOUSE};
        in.mi.dwFlags = MOUSEEVENTF_HWHEEL;
        in.mi.mouseData = (DWORD)(dx * WHEEL_DELTA / 100);
        DoSendInput(&in, 1);
    }
}

void InputHandler::Key(uint16_t jsKey, uint16_t scan, bool down) {
    if(!enabled || !ChkLim(keyCnt, MAX_KY, droppedKeys)) return;
    WORD vk = JsKeyToVK(jsKey);
    if(!vk) { DBG("InputHandler: Unknown JS keycode %u", jsKey); return; }
    if(IsBlocked(vk, down)) return;

    INPUT in{INPUT_KEYBOARD};
    in.ki.wVk = vk;
    in.ki.wScan = scan ? scan : (WORD)MapVirtualKey(vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP) | (IsExt(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
    if(DoSendInput(&in, 1)) totalKeys++;
}

bool InputHandler::HandleMessage(const uint8_t* data, size_t len) {
    if(len < 4) { WARN("InputHandler: Message too short (%zu bytes)", len); return false; }
    uint32_t magic = *(const uint32_t*)data;

    switch(magic) {
        case MSG_MOUSE_MOVE:
            if(len >= sizeof(MouseMoveMsg)) {
                auto* m = (const MouseMoveMsg*)data;
                MouseMove(m->x, m->y);
                return true;
            }
            WARN("InputHandler: MSG_MOUSE_MOVE truncated (%zu < %zu)", len, sizeof(MouseMoveMsg));
            break;

        case MSG_MOUSE_MOVE_REL:
            if(len >= sizeof(MouseMoveRelMsg)) {
                auto* m = (const MouseMoveRelMsg*)data;
                MouseMoveRel(m->dx, m->dy);
                return true;
            }
            WARN("InputHandler: MSG_MOUSE_MOVE_REL truncated");
            break;

        case MSG_MOUSE_BTN:
            if(len >= sizeof(MouseBtnMsg)) {
                auto* m = (const MouseBtnMsg*)data;
                MouseButton(m->button, m->action != 0);
                return true;
            }
            WARN("InputHandler: MSG_MOUSE_BTN truncated");
            break;

        case MSG_MOUSE_WHEEL:
            if(len >= sizeof(MouseWheelMsg)) {
                auto* m = (const MouseWheelMsg*)data;
                MouseWheel(m->deltaX, m->deltaY);
                return true;
            }
            WARN("InputHandler: MSG_MOUSE_WHEEL truncated");
            break;

        case MSG_KEY:
            if(len >= sizeof(KeyMsg)) {
                auto* m = (const KeyMsg*)data;
                Key(m->keyCode, m->scanCode, m->action != 0);
                return true;
            }
            WARN("InputHandler: MSG_KEY truncated");
            break;

        default:
            DBG("InputHandler: Unknown message type 0x%08X", magic);
            break;
    }
    return false;
}

bool InputHandler::SetClipboardText(const std::string& text) {
    if(text.empty()) {
        DBG("InputHandler: SetClipboardText called with empty text");
        return false;
    }
    if(text.size() > 1048576) {
        WARN("InputHandler: Clipboard text too large (%zu bytes)", text.size());
        return false;
    }
    if(!OpenClipboard(nullptr)) {
        WARN("InputHandler: OpenClipboard failed: %lu", GetLastError());
        return false;
    }
    if(!EmptyClipboard()) {
        WARN("InputHandler: EmptyClipboard failed: %lu", GetLastError());
        CloseClipboard();
        return false;
    }

    int wlen = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if(wlen <= 0) {
        WARN("InputHandler: MultiByteToWideChar size query failed: %lu", GetLastError());
        CloseClipboard();
        return false;
    }

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, wlen * sizeof(wchar_t));
    if(!hMem) {
        ERR("InputHandler: GlobalAlloc failed for %d wchars: %lu", wlen, GetLastError());
        CloseClipboard();
        return false;
    }

    wchar_t* pMem = (wchar_t*)GlobalLock(hMem);
    if(!pMem) {
        ERR("InputHandler: GlobalLock failed: %lu", GetLastError());
        GlobalFree(hMem);
        CloseClipboard();
        return false;
    }

    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, pMem, wlen);
    GlobalUnlock(hMem);

    if(!SetClipboardData(CF_UNICODETEXT, hMem)) {
        ERR("InputHandler: SetClipboardData failed: %lu", GetLastError());
        GlobalFree(hMem);
        CloseClipboard();
        return false;
    }

    CloseClipboard();
    DBG("InputHandler: Clipboard set (%zu chars)", text.size());
    return true;
}

std::string InputHandler::GetClipboardText() {
    if(!OpenClipboard(nullptr)) {
        DBG("InputHandler: OpenClipboard failed for read: %lu", GetLastError());
        return "";
    }
    if(!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        DBG("InputHandler: Clipboard doesn't contain text");
        CloseClipboard();
        return "";
    }

    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if(!hData) {
        WARN("InputHandler: GetClipboardData failed: %lu", GetLastError());
        CloseClipboard();
        return "";
    }

    wchar_t* pW = (wchar_t*)GlobalLock(hData);
    if(!pW) {
        WARN("InputHandler: GlobalLock failed for clipboard: %lu", GetLastError());
        CloseClipboard();
        return "";
    }

    int len = WideCharToMultiByte(CP_UTF8, 0, pW, -1, nullptr, 0, nullptr, nullptr);
    if(len <= 0) {
        WARN("InputHandler: WideCharToMultiByte size query failed: %lu", GetLastError());
        GlobalUnlock(hData);
        CloseClipboard();
        return "";
    }

    std::string r(len-1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, pW, -1, r.data(), len, nullptr, nullptr);
    GlobalUnlock(hData);
    CloseClipboard();
    DBG("InputHandler: Clipboard read (%zu chars)", r.size());
    return r;
}

void InputHandler::GetStats(uint64_t& moves, uint64_t& clicks, uint64_t& keys,
                            uint64_t& dMoves, uint64_t& dClicks, uint64_t& dKeys, uint64_t& blocked) {
    moves = totalMoves.load();
    clicks = totalClicks.load();
    keys = totalKeys.load();
    dMoves = droppedMoves.load();
    dClicks = droppedClicks.load();
    dKeys = droppedKeys.load();
    blocked = blockedKeys.load();
}
