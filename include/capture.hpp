#pragma once
#include "common.hpp"

struct FrameData {
    ID3D11Texture2D* tex = nullptr;
    int64_t ts = 0; uint64_t fence = 0; int poolIdx = -1; bool needsSync = false;
    void Release() { SafeRelease(tex); poolIdx = -1; needsSync = false; }
};

class FrameSlot {
    static constexpr int N = 4;
    FrameData frames[N];
    CRITICAL_SECTION cs;
    HANDLE evt;
    int head = 0, tail = 0, count = 0;
    uint32_t inFlight = 0;
public:
    FrameSlot() { InitializeCriticalSection(&cs); evt = CreateEventW(nullptr, FALSE, FALSE, nullptr); }
    ~FrameSlot() { DeleteCriticalSection(&cs); CloseHandle(evt); for (int i = 0; i < N; i++) frames[i].Release(); }

    void Push(ID3D11Texture2D* tex, int64_t ts, uint64_t fence, bool sync, int idx = -1) {
        EnterCriticalSection(&cs);
        if (count >= N) {
            if (frames[tail].poolIdx >= 0) inFlight &= ~(1u << frames[tail].poolIdx);
            frames[tail].Release(); tail = (tail + 1) % N; count--;
        }
        tex->AddRef();
        frames[head] = {tex, ts, fence, idx, sync};
        if (idx >= 0) inFlight |= (1u << idx);
        head = (head + 1) % N; count++;
        SetEvent(evt);
        LeaveCriticalSection(&cs);
    }

    bool Pop(FrameData& out) {
        WaitForSingleObject(evt, INFINITE);
        EnterCriticalSection(&cs);
        if (count == 0) { LeaveCriticalSection(&cs); return false; }
        out = frames[tail]; frames[tail] = {};
        tail = (tail + 1) % N; count--;
        if (count > 0) SetEvent(evt);
        LeaveCriticalSection(&cs);
        return true;
    }

    void Wake() { SetEvent(evt); }
    void MarkReleased(int i) { if (i < 0) return; EnterCriticalSection(&cs); inFlight &= ~(1u << i); LeaveCriticalSection(&cs); }
    bool IsInFlight(int i) { if (i < 0) return false; EnterCriticalSection(&cs); bool r = (inFlight & (1u << i)) != 0; LeaveCriticalSection(&cs); return r; }

    void Reset() {
        EnterCriticalSection(&cs);
        for (int i = 0; i < N; i++) frames[i].Release();
        head = tail = count = 0; inFlight = 0; ResetEvent(evt);
        LeaveCriticalSection(&cs);
    }
};

class GPUSync {
    ID3D11Device5* dev5 = nullptr;
    ID3D11DeviceContext4* ctx4 = nullptr;
    ID3D11Fence* fence = nullptr;
    HANDLE evt = nullptr;
    uint64_t val = 0;
    bool use = false;
public:
    bool Init(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
        if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dev5))) &&
            SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&ctx4))) &&
            SUCCEEDED(dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)))) {
            evt = CreateEventW(nullptr, FALSE, FALSE, nullptr); use = true;
        } else SafeRelease(dev5, ctx4, fence);
        return true;
    }
    ~GPUSync() { if (evt) CloseHandle(evt); SafeRelease(fence, ctx4, dev5); }

    uint64_t Signal(bool& sync) {
        sync = true;
        if (use && ctx4 && fence) { ctx4->Signal(fence, ++val); return val; }
        return 0;
    }

    bool Wait(uint64_t v, ID3D11DeviceContext* ctx, ID3D11Multithread* mt = nullptr, DWORD ms = 16) {
        if (use && fence && evt) {
            if (fence->GetCompletedValue() >= v) return true;
            fence->SetEventOnCompletion(v, evt);
            return WaitForSingleObject(evt, ms) == WAIT_OBJECT_0 || fence->GetCompletedValue() >= v;
        }
        if (mt) { MTLock lk(mt); ctx->Flush(); } else ctx->Flush();
        return true;
    }

    bool Complete(uint64_t v) const { return !use || !fence || fence->GetCompletedValue() >= v; }
};

class ScreenCapture {
    static constexpr int POOL = 6;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11Multithread* mt = nullptr;
    WGD::Direct3D11::IDirect3DDevice winrtDev{nullptr};
    WGC::GraphicsCaptureItem item{nullptr};
    WGC::Direct3D11CaptureFramePool pool{nullptr};
    WGC::GraphicsCaptureSession session{nullptr};
    ID3D11Texture2D* texPool[POOL] = {};
    uint64_t texFences[POOL] = {};
    int texIdx = 0, w = 0, h = 0, hostFps = 60;
    std::atomic<int> targetFps{60}, monIdx{0};
    GPUSync sync;
    FrameSlot* slot;
    std::atomic<bool> running{true}, capturing{false}, started{false};
    HMONITOR curMon = nullptr;
    std::mutex mtx;
    std::function<void(int, int, int)> onResChange;
    int64_t lastTs = 0, minInterval = 0;

    int FindTex() {
        for (int i = 0; i < POOL; i++) { int idx = (texIdx + i) % POOL; if (!slot->IsInFlight(idx) && sync.Complete(texFences[idx])) { texIdx = idx + 1; return idx; } }
        for (int i = 0; i < POOL; i++) { int idx = (texIdx + i) % POOL; if (!slot->IsInFlight(idx)) { if (texFences[idx] > 0 && !sync.Complete(texFences[idx])) { MTLock lk(mt); sync.Wait(texFences[idx], ctx, mt, 4); } texIdx = idx + 1; return idx; } }
        return -1;
    }

    void OnFrame(WGC::Direct3D11CaptureFramePool const& s, winrt::Windows::Foundation::IInspectable const&) {
        auto f = s.TryGetNextFrame();
        if (!f || !running || !capturing) return;
        int64_t ts = GetTimestamp();
        if (minInterval > 0 && lastTs > 0 && (ts - lastTs) < minInterval) return;
        lastTs = ts;
        auto surf = f.Surface(); if (!surf) return;
        winrt::com_ptr<ID3D11Texture2D> src;
        auto acc = surf.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        if (FAILED(acc->GetInterface(IID_PPV_ARGS(src.put()))) || !src) return;
        int ti = FindTex(); if (ti < 0 || !texPool[ti]) return;
        bool ns = false; uint64_t fv = 0;
        { MTLock lk(mt); ctx->CopyResource(texPool[ti], src.get()); ctx->Flush(); fv = sync.Signal(ns); }
        texFences[ti] = fv;
        slot->Push(texPool[ti], ts, fv, ns, ti);
    }

    void InitMon(HMONITOR mon, bool keepFps = false) {
        MONITORINFOEXW mi{sizeof(mi)}; DEVMODEW dm{.dmSize = sizeof(dm)};
        if (GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) hostFps = dm.dmDisplayFrequency;
        if (!keepFps) targetFps = hostFps;
        UpdateInterval();
        auto interop = winrt::get_activation_factory<WGC::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        if (FAILED(interop->CreateForMonitor(mon, winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(), winrt::put_abi(item))))
            throw std::runtime_error("Capture item failed");
        w = item.Size().Width; h = item.Size().Height;
        for (auto& t : texPool) SafeRelease(t);
        for (auto& f : texFences) f = 0;
        D3D11_TEXTURE2D_DESC td = {(UINT)w, (UINT)h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, D3D11_RESOURCE_MISC_SHARED};
        for (int i = 0; i < POOL; i++) if (FAILED(dev->CreateTexture2D(&td, nullptr, &texPool[i]))) throw std::runtime_error("Texture pool failed");
        pool = WGC::Direct3D11CaptureFramePool::CreateFreeThreaded(winrtDev, WGD::DirectXPixelFormat::B8G8R8A8UIntNormalized, 4, {w, h});
        pool.FrameArrived({this, &ScreenCapture::OnFrame});
        session = pool.CreateCaptureSession(item);
        session.IsCursorCaptureEnabled(true);
        try { session.IsBorderRequired(false); } catch (...) {}
        try { session.MinUpdateInterval(duration<int64_t, std::ratio<1, 10000000>>(0)); } catch (...) {}
        started = false; curMon = mon;
    }

    void UpdateInterval() { int f = targetFps.load(); minInterval = f > 0 ? 800000 / f : 0; }

public:
    explicit ScreenCapture(FrameSlot* s) : slot(s) {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        D3D_FEATURE_LEVEL lvls[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0}, act;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, lvls, _countof(lvls), D3D11_SDK_VERSION, &dev, &act, &ctx)))
            throw std::runtime_error("D3D11 device failed");
        if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&mt)))) mt->SetMultithreadProtected(TRUE);
        if (!sync.Init(dev, ctx)) throw std::runtime_error("GPU sync failed");
        winrt::com_ptr<IDXGIDevice> dxgi; winrt::com_ptr<::IInspectable> insp;
        if (FAILED(dev->QueryInterface(IID_PPV_ARGS(dxgi.put()))) || FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), insp.put())))
            throw std::runtime_error("WinRT device failed");
        winrtDev = insp.as<WGD::Direct3D11::IDirect3DDevice>();
        RefreshMonitorList();
        InitMon(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY));
        LOG("Capture: %dx%d @ %dHz", w, h, hostFps);
    }

    ~ScreenCapture() {
        running = capturing = false;
        try { if (session) session.Close(); } catch (...) {}
        try { if (pool) pool.Close(); } catch (...) {}
        for (auto& t : texPool) SafeRelease(t);
        SafeRelease(mt, ctx, dev);
        winrt::uninit_apartment();
    }

    void SetResolutionChangeCallback(std::function<void(int, int, int)> cb) { onResChange = cb; }

    void StartCapture() {
        std::lock_guard<std::mutex> lk(mtx);
        if (capturing) return;
        slot->Reset(); texIdx = 0; lastTs = 0;
        for (auto& f : texFences) f = 0;
        if (!started.exchange(true)) session.StartCapture();
        capturing = true;
    }

    void PauseCapture() { capturing = false; }

    bool SwitchMonitor(int i) {
        std::lock_guard<std::mutex> lk(mtx);
        std::lock_guard<std::mutex> ml(g_monitorsMutex);
        if (i < 0 || i >= (int)g_monitors.size()) return false;
        if (monIdx == i && curMon == g_monitors[i].hMon) return true;
        bool was = capturing.load();
        capturing = false;
        try { if (session) session.Close(); } catch (...) {}
        try { if (pool) pool.Close(); } catch (...) {}
        session = nullptr; pool = nullptr; item = nullptr;
        slot->Reset(); texIdx = 0;
        try {
            InitMon(g_monitors[i].hMon, true);
            monIdx = i;
            if (onResChange) onResChange(w, h, targetFps.load());
            if (was) { session.StartCapture(); started = true; capturing = true; }
            return true;
        } catch (...) { return false; }
    }

    bool SetFPS(int fps) { if (fps < 1 || fps > 240) return false; int old = targetFps.exchange(fps); if (old != fps) UpdateInterval(); return true; }

    int RefreshHostFPS() {
        if (curMon) { MONITORINFOEXW mi{sizeof(mi)}; DEVMODEW dm{.dmSize = sizeof(dm)}; if (GetMonitorInfoW(curMon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) hostFps = dm.dmDisplayFrequency; }
        return hostFps;
    }

    int GetCurrentMonitorIndex() const { return monIdx; }
    int GetHostFPS() const { return hostFps; }
    int GetCurrentFPS() const { return targetFps; }
    bool IsCapturing() const { return capturing; }
    bool WaitReady(uint64_t f) { return sync.Wait(f, ctx, mt, 16); }
    ID3D11Device* GetDev() const { return dev; }
    ID3D11DeviceContext* GetCtx() const { return ctx; }
    ID3D11Multithread* GetMT() const { return mt; }
    int GetW() const { return w; }
    int GetH() const { return h; }
};
