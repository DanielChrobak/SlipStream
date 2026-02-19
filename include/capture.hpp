#pragma once
#include "common.hpp"

struct FrameData {
    ID3D11Texture2D* tex=nullptr;
    int64_t ts=0;
    uint64_t fence=0;
    int poolIdx=-1;
    bool needsSync=false;
    uint64_t generation=0;
    void Release();
};

class FrameSlot {
    static constexpr int N=4;
    FrameData fr[N];
    CRITICAL_SECTION cs;
    HANDLE evt;
    int head=0, tail=0, cnt=0;
    uint32_t inFlight=0;
    std::atomic<uint64_t> curGen{0};

public:
    FrameSlot();
    ~FrameSlot();

    void SetGeneration(uint64_t g);
    uint64_t GetGeneration() const;

    void Push(ID3D11Texture2D* tex, int64_t ts, uint64_t fence, bool sync, int idx=-1);
    bool Pop(FrameData& out);
    void Wake();
    void MarkReleased(int i);
    bool IsInFlight(int i);
    void Reset();
};

class GPUSync {
    ID3D11Device5* d5=nullptr;
    ID3D11DeviceContext4* c4=nullptr;
    ID3D11Fence* f=nullptr;
    HANDLE evt=nullptr;
    uint64_t val=0;
    bool use=false;

public:
    bool Init(ID3D11Device* d, ID3D11DeviceContext* c);
    ~GPUSync();
    uint64_t Signal(bool& sync);
    bool Wait(uint64_t v, ID3D11DeviceContext* ctx, ID3D11Multithread* mt=nullptr, DWORD ms=16);
    bool Complete(uint64_t v) const;
};

class ScreenCapture {
    static constexpr int POOL=6;

    ID3D11Device* dev=nullptr;
    ID3D11DeviceContext* ctx=nullptr;
    ID3D11Multithread* mt=nullptr;

    WGD::Direct3D11::IDirect3DDevice winrtDev{nullptr};
    WGC::GraphicsCaptureItem item{nullptr};
    WGC::Direct3D11CaptureFramePool pool{nullptr};
    WGC::GraphicsCaptureSession sess{nullptr};

    ID3D11Texture2D* texPool[POOL]{};
    uint64_t texFences[POOL]{};
    int texIdx=0, w=0, h=0, hostFps=60;

    std::atomic<int> targetFps{60}, monIdx{0};
    std::atomic<uint64_t> captureGen{0};

    GPUSync sync;
    FrameSlot* slot;
    std::atomic<bool> running{true}, capturing{false}, started{false};
    std::atomic<int> cbActive{0};
    HMONITOR curMon=nullptr;
    std::recursive_mutex mtx;
    std::function<void(int,int,int)> onResChange;
    bool cursorCapture=false;

    int FindTex();
    void OnFrame(WGC::Direct3D11CaptureFramePool const& s, winrt::Windows::Foundation::IInspectable const&);
    void UpdateInterval();
    void WaitCB(int ms=500);
    void InitMon(HMONITOR mon, bool keepFps=false);

public:
    explicit ScreenCapture(FrameSlot* s);
    ~ScreenCapture();

    void SetResolutionChangeCallback(std::function<void(int,int,int)> cb);
    void StartCapture();
    void PauseCapture();
    bool SwitchMonitor(int i);
    bool SetFPS(int fps);
    int RefreshHostFPS();

    int GetCurrentMonitorIndex() const;
    int GetHostFPS() const;
    int GetCurrentFPS() const;
    bool IsCapturing() const;
    bool WaitReady(uint64_t f);
    ID3D11Device* GetDev() const;
    ID3D11DeviceContext* GetCtx() const;
    ID3D11Multithread* GetMT() const;
    int GetW() const;
    int GetH() const;
    uint64_t GetGeneration() const;

    void SetCursorCapture(bool en);
};
