#pragma once
#include "common.hpp"

#pragma pack(push,1)
struct MouseMoveMsg { uint32_t magic; float x, y; };
struct MouseMoveRelMsg { uint32_t magic; int16_t dx, dy; };
struct MouseBtnMsg { uint32_t magic; uint8_t button, action; };
struct MouseWheelMsg { uint32_t magic; int16_t deltaX, deltaY; };
struct KeyMsg { uint32_t magic; uint16_t keyCode, scanCode; uint8_t action; };
#pragma pack(pop)

WORD JsKeyToVK(uint16_t k);

class InputHandler {
    std::atomic<int> monX{0}, monY{0}, monW{1920}, monH{1080};
    std::atomic<bool> enabled{false}, ctrlDown{false}, altDown{false};
    std::atomic<int64_t> rateStart{0};
    std::atomic<int> moveCnt{0}, clickCnt{0}, keyCnt{0};
    static constexpr int MAX_MV=500, MAX_CL=50, MAX_KY=100;
    std::atomic<CursorType> lastCur{CURSOR_DEFAULT};

    std::atomic<uint64_t> totalMoves{0}, totalClicks{0}, totalKeys{0};
    std::atomic<uint64_t> droppedMoves{0}, droppedClicks{0}, droppedKeys{0};
    std::atomic<uint64_t> blockedKeys{0};

    static HCURSOR GetStdCursor(int i);
    void ResetWin();
    bool ChkLim(std::atomic<int>& c, int max, std::atomic<uint64_t>& dropped);
    void ToAbs(float nx, float ny, LONG& ax, LONG& ay);
    static bool IsExt(WORD vk);
    bool IsBlocked(WORD vk, bool down);
    bool DoSendInput(INPUT* in, UINT count);

public:
    void SetMonitorBounds(int x, int y, int w, int h);
    void UpdateFromMonitorInfo(const MonitorInfo& info);
    void Enable();

    bool GetCurrentCursor(CursorType& out);
    void WiggleCenter();

    void MouseMove(float nx, float ny);
    void MouseMoveRel(int16_t dx, int16_t dy);
    void MouseButton(uint8_t btn, bool down);
    void MouseWheel(int16_t dx, int16_t dy);
    void Key(uint16_t jsKey, uint16_t scan, bool down);

    bool HandleMessage(const uint8_t* data, size_t len);
    bool SetClipboardText(const std::string& text);
    std::string GetClipboardText();

    void GetStats(uint64_t& moves, uint64_t& clicks, uint64_t& keys,
                  uint64_t& dMoves, uint64_t& dClicks, uint64_t& dKeys, uint64_t& blocked);
};
