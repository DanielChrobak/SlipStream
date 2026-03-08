#pragma once
#include "host/core/common.hpp"

struct FrameData {
    ID3D11Texture2D* tex = nullptr;
    int64_t ts = 0;
    int64_t sourceTs = 0;
    uint64_t fence = 0;
    int poolIdx = -1;
    bool needsSync = false;
    uint64_t generation = 0;
    void Release() { SafeRelease(tex); ts = sourceTs = 0; poolIdx = -1; needsSync = false; generation = 0; }
};

class FrameSlot {
    static constexpr int N = 4;
    FrameData fr[N];
    CRITICAL_SECTION cs;
    HANDLE evt;
    int head = 0, tail = 0, cnt = 0;
    uint32_t inFlight = 0;
    std::atomic<uint64_t> curGen{0};

public:
    FrameSlot();
    ~FrameSlot();
    void SetGeneration(uint64_t g) { curGen.store(g, std::memory_order_release); }
    [[nodiscard]] uint64_t GetGeneration() const { return curGen.load(std::memory_order_acquire); }
    void Push(ID3D11Texture2D* tex, int64_t ts, int64_t sourceTs, uint64_t fence, bool sync, int idx = -1);
    bool Pop(FrameData& out);
    void Wake() { if (evt) SetEvent(evt); }
    void MarkReleased(int i);
    [[nodiscard]] bool IsInFlight(int i);
    void Reset();
};

class ScreenCapture {
    static constexpr int POOL = 6;

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11Multithread* mt = nullptr;

    WGD::Direct3D11::IDirect3DDevice winrtDev{nullptr};
    WGC::GraphicsCaptureItem item{nullptr};
    WGC::Direct3D11CaptureFramePool pool{nullptr};
    WGC::GraphicsCaptureSession sess{nullptr};

    ID3D11Texture2D* texPool[POOL]{};
    uint64_t texFences[POOL]{};
    int texIdx = 0, w = 0, h = 0, hostFps = 60;

    std::atomic<int> targetFps{60}, monIdx{0};
    std::atomic<uint64_t> captureGen{0};

    D3D11FenceSync sync;
    FrameSlot* slot;
    std::atomic<bool> running{true}, capturing{false}, started{false};
    std::atomic<int> cbActive{0};
    HMONITOR curMon = nullptr;
    std::recursive_mutex mtx;
    std::function<void(int, int, int)> onResChange;
    bool cursorCapture = false;

    int FindTex();
    void OnFrame(WGC::Direct3D11CaptureFramePool const& s, winrt::Windows::Foundation::IInspectable const&);
    void WaitCB(int ms = 500);
    void InitMon(HMONITOR mon, bool keepFps = false);

public:
    explicit ScreenCapture(FrameSlot* s);
    ~ScreenCapture();

    void SetResolutionChangeCallback(std::function<void(int, int, int)> cb) { onResChange = cb; }
    void Shutdown();
    void StartCapture();
    void PauseCapture();
    bool SwitchMonitor(int i);
    bool SetFPS(int fps);
    int RefreshHostFPS();

    [[nodiscard]] int GetCurrentMonitorIndex() const { return monIdx.load(); }
    [[nodiscard]] int GetHostFPS() const { return hostFps; }
    [[nodiscard]] int GetCurrentFPS() const { return targetFps.load(); }
    [[nodiscard]] bool IsCapturing() const { return capturing.load(); }
    [[nodiscard]] bool WaitReady(uint64_t f) { return sync.Wait(f, ctx, mt, 16); }
    [[nodiscard]] ID3D11Device* GetDev() const { return dev; }
    [[nodiscard]] ID3D11DeviceContext* GetCtx() const { return ctx; }
    [[nodiscard]] ID3D11Multithread* GetMT() const { return mt; }
    [[nodiscard]] int GetW() const { return w; }
    [[nodiscard]] int GetH() const { return h; }
    [[nodiscard]] uint64_t GetGeneration() const { return captureGen.load(); }
    void SetCursorCapture(bool en);
};
