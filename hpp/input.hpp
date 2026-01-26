#pragma once

#include "common.hpp"

// ============================================================================
// Input Message Structures
// ============================================================================

#pragma pack(push, 1)

struct MouseMoveMsg {
    uint32_t magic;
    float x;
    float y;
};

struct MouseBtnMsg {
    uint32_t magic;
    uint8_t button;
    uint8_t action;
};

struct MouseWheelMsg {
    uint32_t magic;
    int16_t deltaX;
    int16_t deltaY;
    float x;
    float y;
};

struct KeyMsg {
    uint32_t magic;
    uint16_t keyCode;
    uint16_t scanCode;
    uint8_t action;
    uint8_t modifiers;
};

#pragma pack(pop)

// ============================================================================
// JavaScript Key to Virtual Key Mapping
// ============================================================================

inline WORD JsKeyToVK(uint16_t k) {
    // Direct mapping for alphanumeric keys
    if ((k >= 65 && k <= 90) || (k >= 48 && k <= 57)) {
        return static_cast<WORD>(k);
    }

    static const std::unordered_map<uint16_t, WORD> m = {
        // Control keys
        {8, VK_BACK},
        {9, VK_TAB},
        {13, VK_RETURN},
        {16, VK_SHIFT},
        {17, VK_CONTROL},
        {18, VK_MENU},
        {19, VK_PAUSE},
        {20, VK_CAPITAL},
        {27, VK_ESCAPE},
        {32, VK_SPACE},

        // Navigation keys
        {33, VK_PRIOR},
        {34, VK_NEXT},
        {35, VK_END},
        {36, VK_HOME},
        {37, VK_LEFT},
        {38, VK_UP},
        {39, VK_RIGHT},
        {40, VK_DOWN},

        // Editing keys
        {44, VK_SNAPSHOT},
        {45, VK_INSERT},
        {46, VK_DELETE},

        // Windows keys
        {91, VK_LWIN},
        {92, VK_RWIN},

        // Numpad
        {96, VK_NUMPAD0},
        {97, VK_NUMPAD1},
        {98, VK_NUMPAD2},
        {99, VK_NUMPAD3},
        {100, VK_NUMPAD4},
        {101, VK_NUMPAD5},
        {102, VK_NUMPAD6},
        {103, VK_NUMPAD7},
        {104, VK_NUMPAD8},
        {105, VK_NUMPAD9},
        {106, VK_MULTIPLY},
        {107, VK_ADD},
        {109, VK_SUBTRACT},
        {110, VK_DECIMAL},
        {111, VK_DIVIDE},

        // Function keys
        {112, VK_F1},
        {113, VK_F2},
        {114, VK_F3},
        {115, VK_F4},
        {116, VK_F5},
        {117, VK_F6},
        {118, VK_F7},
        {119, VK_F8},
        {120, VK_F9},
        {121, VK_F10},
        {122, VK_F11},
        {123, VK_F12},

        // Lock keys
        {144, VK_NUMLOCK},
        {145, VK_SCROLL},

        // Media keys
        {173, VK_VOLUME_MUTE},
        {174, VK_VOLUME_DOWN},
        {175, VK_VOLUME_UP},
        {176, VK_MEDIA_NEXT_TRACK},
        {177, VK_MEDIA_PREV_TRACK},
        {178, VK_MEDIA_STOP},
        {179, VK_MEDIA_PLAY_PAUSE},

        // OEM keys
        {186, VK_OEM_1},      // ;:
        {187, VK_OEM_PLUS},   // =+
        {188, VK_OEM_COMMA},  // ,<
        {189, VK_OEM_MINUS},  // -_
        {190, VK_OEM_PERIOD}, // .>
        {191, VK_OEM_2},      // /?
        {192, VK_OEM_3},      // `~
        {219, VK_OEM_4},      // [{
        {220, VK_OEM_5},      // \|
        {221, VK_OEM_6},      // ]}
        {222, VK_OEM_7}       // '"
    };

    auto it = m.find(k);
    return it != m.end() ? it->second : 0;
}

// ============================================================================
// Input Handler Class
// ============================================================================

class InputHandler {
private:
    // Monitor bounds
    std::atomic<int> monitorX{0};
    std::atomic<int> monitorY{0};
    std::atomic<int> monitorWidth{1920};
    std::atomic<int> monitorHeight{1080};

    // State
    std::atomic<bool> enabled{false};

    // Metrics
    std::atomic<uint64_t> moveCount{0};
    std::atomic<uint64_t> clickCount{0};
    std::atomic<uint64_t> keyCount{0};
    std::atomic<uint64_t> blockedCount{0};
    std::atomic<uint64_t> rateLimitedCount{0};

    // Rate limiting
    std::atomic<int64_t> rateLimitWindowStart{0};
    std::atomic<int> moveThisSecond{0};
    std::atomic<int> clicksThisSecond{0};
    std::atomic<int> keysThisSecond{0};

    static constexpr int MAX_MOVES_PER_SECOND = 500;
    static constexpr int MAX_CLICKS_PER_SECOND = 50;
    static constexpr int MAX_KEYS_PER_SECOND = 100;

    // Modifier key state
    std::atomic<bool> ctrlDown{false};
    std::atomic<bool> altDown{false};

    // ========================================================================
    // Coordinate Conversion
    // ========================================================================

    void ToAbsolute(float nx, float ny, LONG& ax, LONG& ay) {
        int px = monitorX + static_cast<int>(std::clamp(nx, 0.f, 1.f) * monitorWidth);
        int py = monitorY + static_cast<int>(std::clamp(ny, 0.f, 1.f) * monitorHeight);

        ax = static_cast<LONG>(
            (px - GetSystemMetrics(SM_XVIRTUALSCREEN)) * 65535 /
            GetSystemMetrics(SM_CXVIRTUALSCREEN)
        );
        ay = static_cast<LONG>(
            (py - GetSystemMetrics(SM_YVIRTUALSCREEN)) * 65535 /
            GetSystemMetrics(SM_CYVIRTUALSCREEN)
        );
    }

    // ========================================================================
    // Extended Key Check
    // ========================================================================

    static bool IsExtendedKey(WORD vk) {
        return vk == VK_INSERT || vk == VK_DELETE ||
               vk == VK_HOME || vk == VK_END ||
               vk == VK_PRIOR || vk == VK_NEXT ||
               vk == VK_LEFT || vk == VK_RIGHT ||
               vk == VK_UP || vk == VK_DOWN ||
               vk == VK_LWIN || vk == VK_RWIN ||
               vk == VK_APPS || vk == VK_DIVIDE ||
               vk == VK_NUMLOCK;
    }

    // ========================================================================
    // Timing
    // ========================================================================

    int64_t GetTimeMs() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();
    }

    // ========================================================================
    // Rate Limiting
    // ========================================================================

    void ResetRateLimitWindow() {
        int64_t now = GetTimeMs();
        int64_t windowStart = rateLimitWindowStart.load();

        if (now - windowStart >= 1000) {
            rateLimitWindowStart = now;
            moveThisSecond = 0;
            clicksThisSecond = 0;
            keysThisSecond = 0;
        }
    }

    bool CheckRateLimit(std::atomic<int>& counter, int maxPerSecond) {
        ResetRateLimitWindow();

        if (counter.fetch_add(1) >= maxPerSecond) {
            rateLimitedCount++;
            return false;
        }
        return true;
    }

    // ========================================================================
    // Blocked Key Combinations
    // ========================================================================

    bool IsBlockedKeyCombo(WORD vk, bool down) {
        // Track modifier state
        if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) {
            ctrlDown = down;
        }
        if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) {
            altDown = down;
        }

        // Block Windows keys
        if (vk == VK_LWIN || vk == VK_RWIN) {
            blockedCount++;
            return true;
        }

        // Block Ctrl+Alt+Delete
        if (ctrlDown && altDown && vk == VK_DELETE) {
            blockedCount++;
            return true;
        }

        return false;
    }

public:
    // ========================================================================
    // Configuration
    // ========================================================================

    void SetMonitorBounds(int x, int y, int w, int h) {
        monitorX = x;
        monitorY = y;
        monitorWidth = w;
        monitorHeight = h;
    }

    void UpdateFromMonitorInfo(const MonitorInfo& info) {
        MONITORINFO mi{sizeof(mi)};
        if (GetMonitorInfo(info.hMon, &mi)) {
            SetMonitorBounds(
                mi.rcMonitor.left,
                mi.rcMonitor.top,
                mi.rcMonitor.right - mi.rcMonitor.left,
                mi.rcMonitor.bottom - mi.rcMonitor.top
            );
        }
    }

    void Enable() {
        enabled = true;
        LOG("Input enabled");
    }

    void Disable() {
        enabled = false;
    }

    // ========================================================================
    // Mouse Input
    // ========================================================================

    void WiggleCenter() {
        if (!enabled) return;

        LONG ax, ay;
        ToAbsolute(0.5f, 0.5f, ax, ay);

        INPUT inputs[3] = {};
        DWORD flags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;

        for (int i = 0; i < 3; i++) {
            inputs[i].type = INPUT_MOUSE;
            inputs[i].mi.dwFlags = flags;
            inputs[i].mi.dx = ax + (i == 1 ? 1 : 0);
            inputs[i].mi.dy = ay;
        }

        SendInput(3, inputs, sizeof(INPUT));
    }

    void MouseMove(float nx, float ny) {
        if (!enabled) return;
        if (!CheckRateLimit(moveThisSecond, MAX_MOVES_PER_SECOND)) return;

        LONG ax, ay;
        ToAbsolute(nx, ny, ax, ay);

        INPUT in{INPUT_MOUSE};
        in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        in.mi.dx = ax;
        in.mi.dy = ay;

        SendInput(1, &in, sizeof(INPUT));
        moveCount++;
    }

    void MouseButton(uint8_t button, bool down) {
        if (!enabled || button > 4) return;
        if (!CheckRateLimit(clicksThisSecond, MAX_CLICKS_PER_SECOND)) return;

        static const DWORD flags[5][2] = {
            {MOUSEEVENTF_LEFTUP, MOUSEEVENTF_LEFTDOWN},
            {MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_RIGHTDOWN},
            {MOUSEEVENTF_MIDDLEUP, MOUSEEVENTF_MIDDLEDOWN},
            {MOUSEEVENTF_XUP, MOUSEEVENTF_XDOWN},
            {MOUSEEVENTF_XUP, MOUSEEVENTF_XDOWN}
        };

        INPUT in{INPUT_MOUSE};
        in.mi.dwFlags = flags[button][down ? 1 : 0];

        if (button >= 3) {
            in.mi.mouseData = button == 3 ? XBUTTON1 : XBUTTON2;
        }

        SendInput(1, &in, sizeof(INPUT));
        clickCount++;
    }

    void MouseWheel(int16_t dx, int16_t dy) {
        if (!enabled) return;
        if (!CheckRateLimit(clicksThisSecond, MAX_CLICKS_PER_SECOND)) return;

        if (dy) {
            INPUT in{INPUT_MOUSE};
            in.mi.dwFlags = MOUSEEVENTF_WHEEL;
            in.mi.mouseData = static_cast<DWORD>(-dy * WHEEL_DELTA / 100);
            SendInput(1, &in, sizeof(INPUT));
        }

        if (dx) {
            INPUT in{INPUT_MOUSE};
            in.mi.dwFlags = MOUSEEVENTF_HWHEEL;
            in.mi.mouseData = static_cast<DWORD>(dx * WHEEL_DELTA / 100);
            SendInput(1, &in, sizeof(INPUT));
        }
    }

    // ========================================================================
    // Keyboard Input
    // ========================================================================

    void Key(uint16_t jsKey, uint16_t scanCode, bool down) {
        if (!enabled) return;
        if (!CheckRateLimit(keysThisSecond, MAX_KEYS_PER_SECOND)) return;

        WORD vk = JsKeyToVK(jsKey);
        if (!vk) return;

        if (IsBlockedKeyCombo(vk, down)) return;

        INPUT in{INPUT_KEYBOARD};
        in.ki.wVk = vk;
        in.ki.wScan = scanCode ? scanCode : static_cast<WORD>(MapVirtualKey(vk, MAPVK_VK_TO_VSC));
        in.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP) |
                        (IsExtendedKey(vk) ? KEYEVENTF_EXTENDEDKEY : 0);

        SendInput(1, &in, sizeof(INPUT));
        keyCount++;
    }

    // ========================================================================
    // Message Handler
    // ========================================================================

    bool HandleMessage(const uint8_t* data, size_t len) {
        if (len < 4) return false;

        uint32_t magic = *reinterpret_cast<const uint32_t*>(data);

        switch (magic) {
            case MSG_MOUSE_MOVE:
                if (len >= sizeof(MouseMoveMsg)) {
                    auto* m = reinterpret_cast<const MouseMoveMsg*>(data);
                    MouseMove(m->x, m->y);
                    return true;
                }
                break;

            case MSG_MOUSE_BTN:
                if (len >= sizeof(MouseBtnMsg)) {
                    auto* m = reinterpret_cast<const MouseBtnMsg*>(data);
                    MouseButton(m->button, m->action != 0);
                    return true;
                }
                break;

            case MSG_MOUSE_WHEEL:
                if (len >= 8) {
                    auto* m = reinterpret_cast<const MouseWheelMsg*>(data);
                    MouseWheel(m->deltaX, m->deltaY);
                    return true;
                }
                break;

            case MSG_KEY:
                if (len >= sizeof(KeyMsg)) {
                    auto* m = reinterpret_cast<const KeyMsg*>(data);
                    Key(m->keyCode, m->scanCode, m->action != 0);
                    return true;
                }
                break;
        }

        return false;
    }

    // ========================================================================
    // Statistics
    // ========================================================================

    struct Stats {
        uint64_t moves;
        uint64_t clicks;
        uint64_t keys;
        uint64_t blocked;
        uint64_t rateLimited;
    };

    Stats GetStats() {
        return {
            moveCount.exchange(0),
            clickCount.exchange(0),
            keyCount.exchange(0),
            blockedCount.exchange(0),
            rateLimitedCount.exchange(0)
        };
    }
};
