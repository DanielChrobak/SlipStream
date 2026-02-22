#include "capture.hpp"
#include "app_support.hpp"

FrameSlot::FrameSlot() {
    InitializeCriticalSection(&cs);
    evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

FrameSlot::~FrameSlot() {
    DeleteCriticalSection(&cs);
    if (evt) CloseHandle(evt);
    for (auto& frame : fr) frame.Release();
}

void FrameSlot::Push(ID3D11Texture2D* tex, int64_t ts, uint64_t fence, bool sync, int idx) {
    if (!tex) return;
    EnterCriticalSection(&cs);
    uint64_t gen = curGen.load(std::memory_order_acquire);

    if (cnt >= N) {
        if (fr[tail].poolIdx >= 0) inFlight &= ~(1u << fr[tail].poolIdx);
        fr[tail].Release();
        tail = (tail + 1) % N;
        cnt--;
    }

    tex->AddRef();
    fr[head] = {tex, ts, fence, idx, sync, gen};
    if (idx >= 0) inFlight |= (1u << idx);
    head = (head + 1) % N;
    cnt++;
    SetEvent(evt);
    LeaveCriticalSection(&cs);
}

bool FrameSlot::Pop(FrameData& out) {
    if (WaitForSingleObject(evt, INFINITE) != WAIT_OBJECT_0) return false;
    EnterCriticalSection(&cs);
    if (cnt == 0) { LeaveCriticalSection(&cs); return false; }
    out = fr[tail];
    fr[tail] = {};
    tail = (tail + 1) % N;
    cnt--;
    if (cnt > 0) SetEvent(evt);
    LeaveCriticalSection(&cs);
    return true;
}

void FrameSlot::MarkReleased(int i) {
    if (i < 0) return;
    EnterCriticalSection(&cs);
    inFlight &= ~(1u << i);
    LeaveCriticalSection(&cs);
}

bool FrameSlot::IsInFlight(int i) {
    if (i < 0) return false;
    EnterCriticalSection(&cs);
    bool r = (inFlight & (1u << i)) != 0;
    LeaveCriticalSection(&cs);
    return r;
}

void FrameSlot::Reset() {
    EnterCriticalSection(&cs);
    for (auto& frame : fr) frame.Release();
    head = tail = cnt = 0;
    inFlight = 0;
    ResetEvent(evt);
    LeaveCriticalSection(&cs);
}

bool GPUSync::Init(ID3D11Device* d, ID3D11DeviceContext* c) {
    if (FAILED(d->QueryInterface(IID_PPV_ARGS(&d5)))) return true;
    if (FAILED(c->QueryInterface(IID_PPV_ARGS(&c4)))) { SafeRelease(d5); return true; }
    if (FAILED(d5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&f)))) { SafeRelease(d5, c4); return true; }
    evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!evt) { SafeRelease(f, c4, d5); return false; }
    use = true;
    return true;
}

GPUSync::~GPUSync() {
    if (evt) CloseHandle(evt);
    SafeRelease(f, c4, d5);
}

uint64_t GPUSync::Signal(bool& sync) {
    sync = true;
    if (use && c4 && f && SUCCEEDED(c4->Signal(f, ++val))) return val;
    return 0;
}

bool GPUSync::Wait(uint64_t v, ID3D11DeviceContext* ctx, ID3D11Multithread* mt, DWORD ms) {
    if (use && f && evt) {
        if (f->GetCompletedValue() >= v) return true;
        if (FAILED(f->SetEventOnCompletion(v, evt))) return false;
        return WaitForSingleObject(evt, ms) == WAIT_OBJECT_0 || f->GetCompletedValue() >= v;
    }
    if (mt) { MTLock lk(mt); ctx->Flush(); } else ctx->Flush();
    return true;
}

int ScreenCapture::FindTex() {
    for (int i = 0; i < POOL; i++) {
        int idx = (texIdx + i) % POOL;
        if (!slot->IsInFlight(idx) && sync.Complete(texFences[idx])) {
            texIdx = idx + 1;
            return idx;
        }
    }
    for (int i = 0; i < POOL; i++) {
        int idx = (texIdx + i) % POOL;
        if (!slot->IsInFlight(idx)) {
            if (texFences[idx] > 0 && !sync.Complete(texFences[idx])) {
                MTLock lk(mt);
                [[maybe_unused]] const bool waited = sync.Wait(texFences[idx], ctx, mt, 4);
            }
            texIdx = idx + 1;
            return idx;
        }
    }
    return -1;
}

void ScreenCapture::OnFrame(WGC::Direct3D11CaptureFramePool const& s, winrt::Windows::Foundation::IInspectable const&) {
    uint64_t gen = captureGen.load(std::memory_order_acquire);
    cbActive.fetch_add(1);
    struct Guard { std::atomic<int>& c; ~Guard() { c.fetch_sub(1); } } g{cbActive};

    WGC::Direct3D11CaptureFrame f{nullptr};
    try { f = s.TryGetNextFrame(); } catch (...) { return; }
    if (!f) return;

    if (!running.load() || !capturing.load()) return;

    std::lock_guard<std::recursive_mutex> lk(mtx);
    if (!running.load() || !capturing.load() || gen != captureGen.load()) return;

    auto csz = f.ContentSize();
    if (csz.Width != w || csz.Height != h) return;

    int64_t ts = GetTimestamp();
    WGD::Direct3D11::IDirect3DSurface surf{nullptr};
    try { surf = f.Surface(); } catch (...) { return; }
    if (!surf) return;

    winrt::com_ptr<ID3D11Texture2D> src;
    try {
        auto acc = surf.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        if (FAILED(acc->GetInterface(IID_PPV_ARGS(src.put())))) return;
    } catch (...) { return; }

    D3D11_TEXTURE2D_DESC srcDesc;
    src->GetDesc(&srcDesc);
    if (srcDesc.Width != static_cast<UINT>(w) || srcDesc.Height != static_cast<UINT>(h)) return;

    int ti = FindTex();
    if (ti < 0 || ti >= POOL || !texPool[ti]) return;

    bool ns = false;
    uint64_t fv = 0;
    {
        MTLock l(mt);
        ctx->CopyResource(texPool[ti], src.get());
        ctx->Flush();
        fv = sync.Signal(ns);
    }
    texFences[ti] = fv;
    slot->Push(texPool[ti], ts, fv, ns, ti);
}

void ScreenCapture::WaitCB(int ms) {
    auto st = steady_clock::now();
    while (cbActive.load() > 0 && duration_cast<milliseconds>(steady_clock::now() - st).count() < ms)
        std::this_thread::sleep_for(1ms);
}

void ScreenCapture::InitMon(HMONITOR mon, bool keepFps) {
    MONITORINFOEXW mi{sizeof(mi)};
    DEVMODEW dm{sizeof(dm)};

    if (GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
        hostFps = dm.dmDisplayFrequency;
    else
        hostFps = 60;
    if (!keepFps) targetFps = hostFps;

    auto interop = winrt::get_activation_factory<WGC::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    if (FAILED(interop->CreateForMonitor(mon, winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
                                          winrt::put_abi(item))))
        throw std::runtime_error("Capture item failed");

    w = item.Size().Width;
    h = item.Size().Height;

    for (auto& t : texPool) SafeRelease(t);
    for (auto& f : texFences) f = 0;

    D3D11_TEXTURE2D_DESC td = {static_cast<UINT>(w), static_cast<UINT>(h), 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0},
                               D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                               0, D3D11_RESOURCE_MISC_SHARED};

    for (int i = 0; i < POOL; i++)
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &texPool[i])))
            throw std::runtime_error("Texture pool failed");

    pool = WGC::Direct3D11CaptureFramePool::CreateFreeThreaded(
        winrtDev, WGD::DirectXPixelFormat::B8G8R8A8UIntNormalized, 4, {w, h});
    pool.FrameArrived({this, &ScreenCapture::OnFrame});
    sess = pool.CreateCaptureSession(item);
    sess.IsCursorCaptureEnabled(cursorCapture);
    try { sess.IsBorderRequired(false); } catch (...) {}
    try { sess.MinUpdateInterval(duration<int64_t, std::ratio<1, 10000000>>(0)); } catch (...) {}

    started = false;
    curMon = mon;
    slot->SetGeneration(captureGen.fetch_add(1) + 1);
}

ScreenCapture::ScreenCapture(FrameSlot* s) : slot(s) {
    try { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
    catch (const winrt::hresult_error& e) { if (e.code() != RPC_E_CHANGED_MODE) throw; }

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    D3D_FEATURE_LEVEL lvls[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
                                D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL act;

    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, lvls, _countof(lvls),
                                  D3D11_SDK_VERSION, &dev, &act, &ctx)))
        throw std::runtime_error("D3D11 device failed");

    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&mt)))) mt->SetMultithreadProtected(TRUE);
    if (!sync.Init(dev, ctx)) throw std::runtime_error("GPU sync failed");

    winrt::com_ptr<IDXGIDevice> dxgi;
    winrt::com_ptr<::IInspectable> insp;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(dxgi.put()))) ||
        FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), insp.put())))
        throw std::runtime_error("WinRT device failed");
    winrtDev = insp.as<WGD::Direct3D11::IDirect3DDevice>();

    RefreshMonitorList();
    cursorCapture = false;
    InitMon(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY));
    LOG("Capture: %dx%d @ %dHz", w, h, hostFps);
}

ScreenCapture::~ScreenCapture() {
    running = false;
    capturing = false;
    captureGen.fetch_add(1);

    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        try { if (sess) sess.Close(); } catch (...) {}
        try { if (pool) pool.Close(); } catch (...) {}
        sess = nullptr;
        pool = nullptr;
        item = nullptr;
    }

    WaitCB();

    for (auto& t : texPool) SafeRelease(t);
    SafeRelease(mt, ctx, dev);
    try { winrt::uninit_apartment(); } catch (...) {}
}

void ScreenCapture::StartCapture() {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    if (capturing) return;
    slot->Reset();
    texIdx = 0;
    for (auto& f : texFences) f = 0;

    if (!started.exchange(true)) {
        try { sess.StartCapture(); } catch (...) { started = false; return; }
    }
    capturing = true;
    LOG("ScreenCapture: Capture started");
}

void ScreenCapture::PauseCapture() {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    if (!capturing.exchange(false)) return;
    slot->Reset();
    LOG("ScreenCapture: Capture paused");
}

bool ScreenCapture::SwitchMonitor(int i) {
    HMONITOR nextMon = nullptr;
    {
        std::lock_guard<std::mutex> ml(g_monitorsMutex);
        if (i < 0 || i >= (int)g_monitors.size()) return false;
        if (monIdx == i && curMon == g_monitors[i].hMon) return true;
        nextMon = g_monitors[i].hMon;
    }

    bool was = false;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        was = capturing.load();
        capturing = false;
        slot->SetGeneration(captureGen.fetch_add(1) + 1);
        slot->Wake();

        try { if (sess) sess.Close(); } catch (...) {}
        try { if (pool) pool.Close(); } catch (...) {}
        sess = nullptr;
        pool = nullptr;
        item = nullptr;
    }

    WaitCB();
    std::this_thread::sleep_for(5ms);

    try {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        slot->Wake();
        slot->Reset();
        texIdx = 0;

        InitMon(nextMon, true);
        monIdx = i;
        if (onResChange) onResChange(w, h, targetFps.load());
        if (was) {
            sess.StartCapture();
            started = true;
            capturing = true;
        }
        LOG("ScreenCapture: Monitor switch complete");
        return true;
    } catch (const std::exception& e) {
        ERR("ScreenCapture: Monitor switch failed: %s", e.what());
        return false;
    }
}

bool ScreenCapture::SetFPS(int fps) {
    if (fps < 1 || fps > 240) return false;
    targetFps = fps;
    return true;
}

int ScreenCapture::RefreshHostFPS() {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    if (curMon) {
        MONITORINFOEXW mi{sizeof(mi)};
        DEVMODEW dm{sizeof(dm)};
        if (GetMonitorInfoW(curMon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
            hostFps = dm.dmDisplayFrequency;
    }
    return hostFps;
}

void ScreenCapture::SetCursorCapture(bool en) {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    cursorCapture = en;
    if (sess) try { sess.IsCursorCaptureEnabled(en); } catch (...) {}
}
