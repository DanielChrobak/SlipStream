#include "capture.hpp"

void FrameData::Release() {
    SafeRelease(tex);
    poolIdx = -1;
    needsSync = false;
    generation = 0;
}

FrameSlot::FrameSlot() {
    InitializeCriticalSection(&cs);
    evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if(!evt) {
        ERR("FrameSlot: CreateEventW failed: %lu", GetLastError());
    }
}

FrameSlot::~FrameSlot() {
    DeleteCriticalSection(&cs);
    if(evt) CloseHandle(evt);
    for(int i = 0; i < N; i++) fr[i].Release();
}

void FrameSlot::SetGeneration(uint64_t g) {
    curGen.store(g, std::memory_order_release);
}

uint64_t FrameSlot::GetGeneration() const {
    return curGen.load(std::memory_order_acquire);
}

void FrameSlot::Push(ID3D11Texture2D* tex, int64_t ts, uint64_t fence, bool sync, int idx) {
    if(!tex) {
        WARN("FrameSlot: Push called with null texture");
        return;
    }
    EnterCriticalSection(&cs);
    uint64_t gen = curGen.load(std::memory_order_acquire);

    if(cnt >= N) {
        DBG("FrameSlot: Queue full, dropping oldest frame (pool idx %d)", fr[tail].poolIdx);
        if(fr[tail].poolIdx >= 0) inFlight &= ~(1u << fr[tail].poolIdx);
        fr[tail].Release();
        tail = (tail + 1) % N;
        cnt--;
    }

    tex->AddRef();
    fr[head] = {tex, ts, fence, idx, sync, gen};
    if(idx >= 0) inFlight |= (1u << idx);
    head = (head + 1) % N;
    cnt++;
    SetEvent(evt);
    LeaveCriticalSection(&cs);
}

bool FrameSlot::Pop(FrameData& out) {
    DWORD waitResult = WaitForSingleObject(evt, INFINITE);
    if(waitResult != WAIT_OBJECT_0) {
        WARN("FrameSlot: WaitForSingleObject returned %lu, error: %lu", waitResult, GetLastError());
        return false;
    }
    EnterCriticalSection(&cs);
    if(cnt == 0) {
        LeaveCriticalSection(&cs);
        return false;
    }
    out = fr[tail];
    fr[tail] = {};
    tail = (tail + 1) % N;
    cnt--;
    if(cnt > 0) SetEvent(evt);
    LeaveCriticalSection(&cs);
    return true;
}

void FrameSlot::Wake() {
    if(evt) SetEvent(evt);
}

void FrameSlot::MarkReleased(int i) {
    if(i < 0) return;
    EnterCriticalSection(&cs);
    inFlight &= ~(1u << i);
    LeaveCriticalSection(&cs);
}

bool FrameSlot::IsInFlight(int i) {
    if(i < 0) return false;
    EnterCriticalSection(&cs);
    bool r = (inFlight & (1u << i)) != 0;
    LeaveCriticalSection(&cs);
    return r;
}

void FrameSlot::Reset() {
    EnterCriticalSection(&cs);
    for(int i = 0; i < N; i++) fr[i].Release();
    head = tail = cnt = 0;
    inFlight = 0;
    ResetEvent(evt);
    LeaveCriticalSection(&cs);
    DBG("FrameSlot: Reset completed");
}

bool GPUSync::Init(ID3D11Device* d, ID3D11DeviceContext* c) {
    HRESULT hr = d->QueryInterface(IID_PPV_ARGS(&d5));
    if(FAILED(hr)) {
        DBG("GPUSync: ID3D11Device5 not available (0x%08X), using flush-based sync", hr);
        return true;
    }
    hr = c->QueryInterface(IID_PPV_ARGS(&c4));
    if(FAILED(hr)) {
        WARN("GPUSync: ID3D11DeviceContext4 not available (0x%08X)", hr);
        SafeRelease(d5);
        return true;
    }
    hr = d5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&f));
    if(FAILED(hr)) {
        WARN("GPUSync: CreateFence failed (0x%08X)", hr);
        SafeRelease(d5, c4);
        return true;
    }
    evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if(!evt) {
        ERR("GPUSync: CreateEventW failed: %lu", GetLastError());
        SafeRelease(f, c4, d5);
        return false;
    }
    use = true;
    LOG("GPUSync: Using D3D11 fence-based synchronization");
    return true;
}

GPUSync::~GPUSync() {
    if(evt) CloseHandle(evt);
    SafeRelease(f, c4, d5);
}

uint64_t GPUSync::Signal(bool& sync) {
    sync = true;
    if(use && c4 && f) {
        HRESULT hr = c4->Signal(f, ++val);
        if(FAILED(hr)) {
            WARN("GPUSync: Signal failed (0x%08X)", hr);
            return 0;
        }
        return val;
    }
    return 0;
}

bool GPUSync::Wait(uint64_t v, ID3D11DeviceContext* ctx, ID3D11Multithread* mt, DWORD ms) {
    if(use && f && evt) {
        if(f->GetCompletedValue() >= v) return true;
        HRESULT hr = f->SetEventOnCompletion(v, evt);
        if(FAILED(hr)) {
            WARN("GPUSync: SetEventOnCompletion failed (0x%08X)", hr);
            return false;
        }
        DWORD result = WaitForSingleObject(evt, ms);
        if(result == WAIT_TIMEOUT) {
            DBG("GPUSync: Wait timed out for fence value %llu", v);
        }
        return result == WAIT_OBJECT_0 || f->GetCompletedValue() >= v;
    }
    if(mt) {
        MTLock lk(mt);
        ctx->Flush();
    } else {
        ctx->Flush();
    }
    return true;
}

bool GPUSync::Complete(uint64_t v) const {
    return !use || !f || f->GetCompletedValue() >= v;
}

int ScreenCapture::FindTex() {
    for(int i = 0; i < POOL; i++) {
        int idx = (texIdx + i) % POOL;
        if(!slot->IsInFlight(idx) && sync.Complete(texFences[idx])) {
            texIdx = idx + 1;
            return idx;
        }
    }
    for(int i = 0; i < POOL; i++) {
        int idx = (texIdx + i) % POOL;
        if(!slot->IsInFlight(idx)) {
            if(texFences[idx] > 0 && !sync.Complete(texFences[idx])) {
                DBG("ScreenCapture: Waiting for texture pool slot %d (fence %llu)", idx, texFences[idx]);
                MTLock lk(mt);
                sync.Wait(texFences[idx], ctx, mt, 4);
            }
            texIdx = idx + 1;
            return idx;
        }
    }
    WARN("ScreenCapture: No available texture in pool");
    return -1;
}

void ScreenCapture::OnFrame(WGC::Direct3D11CaptureFramePool const& s, winrt::Windows::Foundation::IInspectable const&) {
    if(!running.load(std::memory_order_acquire) || !capturing.load(std::memory_order_acquire)) return;

    uint64_t gen = captureGen.load(std::memory_order_acquire);
    cbActive.fetch_add(1, std::memory_order_acq_rel);
    struct Guard { std::atomic<int>& c; ~Guard() { c.fetch_sub(1, std::memory_order_acq_rel); } } g{cbActive};

    WGC::Direct3D11CaptureFrame f{nullptr};
    try { f = s.TryGetNextFrame(); }
    catch(const winrt::hresult_error& e) {
        WARN("ScreenCapture: TryGetNextFrame failed: 0x%08X %ls", e.code().value, e.message().c_str());
        return;
    }
    if(!f) return;

    std::lock_guard<std::recursive_mutex> lk(mtx);
    if(!running.load(std::memory_order_acquire) || !capturing.load(std::memory_order_acquire)) return;
    if(gen != captureGen.load(std::memory_order_acquire)) {
        DBG("ScreenCapture: Frame generation mismatch, discarding");
        return;
    }

    auto csz = f.ContentSize();
    if(csz.Width != w || csz.Height != h) {
        LOG("ScreenCapture: Resolution changed from %dx%d to %dx%d", w, h, csz.Width, csz.Height);
        return;
    }

    int64_t ts = GetTimestamp();
    WGD::Direct3D11::IDirect3DSurface surf{nullptr};
    try { surf = f.Surface(); }
    catch(const winrt::hresult_error& e) {
        WARN("ScreenCapture: Surface() failed: 0x%08X", e.code().value);
        return;
    }
    if(!surf) return;

    winrt::com_ptr<ID3D11Texture2D> src;
    try {
        auto acc = surf.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        HRESULT hr = acc->GetInterface(IID_PPV_ARGS(src.put()));
        if(FAILED(hr)) {
            WARN("ScreenCapture: GetInterface for texture failed: 0x%08X", hr);
            return;
        }
    } catch(const winrt::hresult_error& e) {
        WARN("ScreenCapture: Failed to get D3D11 texture: 0x%08X", e.code().value);
        return;
    }
    if(!src) return;

    D3D11_TEXTURE2D_DESC srcDesc;
    src->GetDesc(&srcDesc);
    if(srcDesc.Width != (UINT)w || srcDesc.Height != (UINT)h) {
        DBG("ScreenCapture: Texture size mismatch: got %ux%u, expected %dx%d",
            srcDesc.Width, srcDesc.Height, w, h);
        return;
    }

    int ti = FindTex();
    if(ti < 0 || ti >= POOL || !texPool[ti]) {
        WARN("ScreenCapture: Invalid texture pool index %d", ti);
        return;
    }

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

void ScreenCapture::UpdateInterval() {
    if(!sess) return;
    try {
        sess.MinUpdateInterval(duration<int64_t, std::ratio<1, 10000000>>(0));
    } catch(const winrt::hresult_error& e) {
        DBG("ScreenCapture: MinUpdateInterval not supported: 0x%08X", e.code().value);
    }
}

void ScreenCapture::WaitCB(int ms) {
    auto st = steady_clock::now();
    while(cbActive.load(std::memory_order_acquire) > 0) {
        if(duration_cast<milliseconds>(steady_clock::now() - st).count() > ms) {
            WARN("ScreenCapture: Timeout waiting for callbacks to complete");
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
}

void ScreenCapture::InitMon(HMONITOR mon, bool keepFps) {
    MONITORINFOEXW mi{sizeof(mi)};
    DEVMODEW dm{.dmSize = sizeof(dm)};

    if(GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
        hostFps = dm.dmDisplayFrequency;
    } else {
        WARN("ScreenCapture: Failed to get monitor info, using default 60Hz");
        hostFps = 60;
    }
    if(!keepFps) targetFps = hostFps;

    auto interop = winrt::get_activation_factory<WGC::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    HRESULT hr = interop->CreateForMonitor(mon,
        winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
        winrt::put_abi(item));
    if(FAILED(hr)) {
        ERR("ScreenCapture: CreateForMonitor failed: 0x%08X", hr);
        throw std::runtime_error("Capture item failed");
    }

    w = item.Size().Width;
    h = item.Size().Height;
    LOG("ScreenCapture: Monitor size: %dx%d @ %dHz", w, h, hostFps);

    for(auto& t : texPool) SafeRelease(t);
    for(auto& f : texFences) f = 0;

    D3D11_TEXTURE2D_DESC td = {
        (UINT)w, (UINT)h, 1, 1,
        DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0},
        D3D11_USAGE_DEFAULT,
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
        0, D3D11_RESOURCE_MISC_SHARED
    };

    for(int i = 0; i < POOL; i++) {
        HRESULT texHr = dev->CreateTexture2D(&td, nullptr, &texPool[i]);
        if(FAILED(texHr)) {
            ERR("ScreenCapture: CreateTexture2D failed for pool slot %d: 0x%08X", i, texHr);
            throw std::runtime_error("Texture pool failed");
        }
    }

    try {
        pool = WGC::Direct3D11CaptureFramePool::CreateFreeThreaded(
            winrtDev, WGD::DirectXPixelFormat::B8G8R8A8UIntNormalized, 4, {w, h});
        pool.FrameArrived({this, &ScreenCapture::OnFrame});
        sess = pool.CreateCaptureSession(item);
        sess.IsCursorCaptureEnabled(cursorCapture);
    } catch(const winrt::hresult_error& e) {
        ERR("ScreenCapture: Failed to create capture session: 0x%08X %ls",
            e.code().value, e.message().c_str());
        throw std::runtime_error("Capture session failed");
    }

    try { sess.IsBorderRequired(false); }
    catch(const winrt::hresult_error&) { DBG("ScreenCapture: IsBorderRequired not supported (Windows 10)"); }

    UpdateInterval();
    started = false;
    curMon = mon;
    uint64_t newGen = captureGen.fetch_add(1, std::memory_order_acq_rel) + 1;
    slot->SetGeneration(newGen);
}

ScreenCapture::ScreenCapture(FrameSlot* s) : slot(s) {
    try { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
    catch(const winrt::hresult_error& e) {
        if(e.code() != RPC_E_CHANGED_MODE) {
            ERR("ScreenCapture: winrt::init_apartment failed: 0x%08X", e.code().value);
            throw;
        }
    }

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    D3D_FEATURE_LEVEL lvls[] = {
        D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0
    };
    D3D_FEATURE_LEVEL act;

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   flags, lvls, _countof(lvls), D3D11_SDK_VERSION,
                                   &dev, &act, &ctx);
    if(FAILED(hr)) {
        ERR("ScreenCapture: D3D11CreateDevice failed: 0x%08X", hr);
        throw std::runtime_error("D3D11 device failed");
    }
    LOG("ScreenCapture: D3D11 device created, feature level: 0x%04X", act);

    hr = dev->QueryInterface(IID_PPV_ARGS(&mt));
    if(SUCCEEDED(hr)) {
        mt->SetMultithreadProtected(TRUE);
    } else {
        WARN("ScreenCapture: ID3D11Multithread not available: 0x%08X", hr);
    }

    if(!sync.Init(dev, ctx)) {
        ERR("ScreenCapture: GPU sync initialization failed");
        throw std::runtime_error("GPU sync failed");
    }

    winrt::com_ptr<IDXGIDevice> dxgi;
    winrt::com_ptr<::IInspectable> insp;
    hr = dev->QueryInterface(IID_PPV_ARGS(dxgi.put()));
    if(FAILED(hr)) {
        ERR("ScreenCapture: QueryInterface for IDXGIDevice failed: 0x%08X", hr);
        throw std::runtime_error("DXGI device failed");
    }

    hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), insp.put());
    if(FAILED(hr)) {
        ERR("ScreenCapture: CreateDirect3D11DeviceFromDXGIDevice failed: 0x%08X", hr);
        throw std::runtime_error("WinRT device failed");
    }
    winrtDev = insp.as<WGD::Direct3D11::IDirect3DDevice>();

    RefreshMonitorList();
    cursorCapture = false;
    InitMon(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY));
    LOG("Capture: %dx%d @ %dHz", w, h, hostFps);
}

ScreenCapture::~ScreenCapture() {
    running.store(false, std::memory_order_release);
    capturing.store(false, std::memory_order_release);
    captureGen.fetch_add(1, std::memory_order_release);

    std::lock_guard<std::recursive_mutex> lk(mtx);
    try { if(sess) sess.Close(); }
    catch(const winrt::hresult_error& e) {
        DBG("ScreenCapture: sess.Close() failed: 0x%08X", e.code().value);
    }
    try { if(pool) pool.Close(); }
    catch(const winrt::hresult_error& e) {
        DBG("ScreenCapture: pool.Close() failed: 0x%08X", e.code().value);
    }
    sess = nullptr;
    pool = nullptr;
    WaitCB();

    for(auto& t : texPool) SafeRelease(t);
    SafeRelease(mt, ctx, dev);
    try { winrt::uninit_apartment(); } catch(...) {}
    LOG("ScreenCapture: Destroyed");
}

void ScreenCapture::SetResolutionChangeCallback(std::function<void(int,int,int)> cb) {
    onResChange = cb;
}

void ScreenCapture::StartCapture() {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    if(capturing) return;
    slot->Reset();
    texIdx = 0;
    for(auto& f : texFences) f = 0;

    if(!started.exchange(true)) {
        try { sess.StartCapture(); }
        catch(const winrt::hresult_error& e) {
            ERR("ScreenCapture: StartCapture failed: 0x%08X %ls",
                e.code().value, e.message().c_str());
            started = false;
            return;
        }
    }
    capturing.store(true, std::memory_order_release);
    LOG("ScreenCapture: Capture started");
}

void ScreenCapture::PauseCapture() {
    capturing.store(false, std::memory_order_release);
    DBG("ScreenCapture: Capture paused");
}

bool ScreenCapture::SwitchMonitor(int i) {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    std::lock_guard<std::mutex> ml(g_monitorsMutex);

    if(i < 0 || i >= (int)g_monitors.size()) {
        WARN("ScreenCapture: Invalid monitor index %d (have %zu monitors)", i, g_monitors.size());
        return false;
    }
    if(monIdx == i && curMon == g_monitors[i].hMon) return true;

    LOG("ScreenCapture: Switching to monitor %d (%s)", i, g_monitors[i].name.c_str());
    bool was = capturing.load(std::memory_order_acquire);
    capturing.store(false, std::memory_order_release);

    uint64_t newGen = captureGen.fetch_add(1, std::memory_order_acq_rel) + 1;
    slot->SetGeneration(newGen);
    slot->Wake();

    try { if(sess) sess.Close(); }
    catch(const winrt::hresult_error& e) {
        DBG("ScreenCapture: sess.Close() failed: 0x%08X", e.code().value);
    }
    try { if(pool) pool.Close(); }
    catch(const winrt::hresult_error& e) {
        DBG("ScreenCapture: pool.Close() failed: 0x%08X", e.code().value);
    }

    sess = nullptr;
    pool = nullptr;
    item = nullptr;
    WaitCB();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    slot->Wake();
    slot->Reset();
    texIdx = 0;

    try {
        InitMon(g_monitors[i].hMon, true);
        monIdx = i;
        if(onResChange) onResChange(w, h, targetFps.load());
        if(was) {
            sess.StartCapture();
            started = true;
            capturing.store(true, std::memory_order_release);
        }
        LOG("ScreenCapture: Monitor switch complete");
        return true;
    } catch(const std::exception& e) {
        ERR("ScreenCapture: Monitor switch failed: %s", e.what());
        return false;
    }
}

bool ScreenCapture::SetFPS(int fps) {
    if(fps < 1 || fps > 240) {
        WARN("ScreenCapture: Invalid FPS %d (must be 1-240)", fps);
        return false;
    }
    int old = targetFps.exchange(fps);
    if(old != fps) {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        UpdateInterval();
        DBG("ScreenCapture: FPS changed from %d to %d", old, fps);
    }
    return true;
}

int ScreenCapture::RefreshHostFPS() {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    if(curMon) {
        MONITORINFOEXW mi{sizeof(mi)};
        DEVMODEW dm{.dmSize = sizeof(dm)};
        if(GetMonitorInfoW(curMon, &mi) &&
           EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
            int newFps = dm.dmDisplayFrequency;
            if(newFps != hostFps) {
                LOG("ScreenCapture: Host refresh rate changed from %d to %d Hz", hostFps, newFps);
                hostFps = newFps;
            }
        }
    }
    return hostFps;
}

int ScreenCapture::GetCurrentMonitorIndex() const {
    return monIdx.load(std::memory_order_acquire);
}

int ScreenCapture::GetHostFPS() const {
    return hostFps;
}

int ScreenCapture::GetCurrentFPS() const {
    return targetFps.load(std::memory_order_acquire);
}

bool ScreenCapture::IsCapturing() const {
    return capturing.load(std::memory_order_acquire);
}

bool ScreenCapture::WaitReady(uint64_t f) {
    return sync.Wait(f, ctx, mt, 16);
}

ID3D11Device* ScreenCapture::GetDev() const {
    return dev;
}

ID3D11DeviceContext* ScreenCapture::GetCtx() const {
    return ctx;
}

ID3D11Multithread* ScreenCapture::GetMT() const {
    return mt;
}

int ScreenCapture::GetW() const {
    return w;
}

int ScreenCapture::GetH() const {
    return h;
}

uint64_t ScreenCapture::GetGeneration() const {
    return captureGen.load(std::memory_order_acquire);
}

void ScreenCapture::SetCursorCapture(bool en) {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    cursorCapture = en;
    if(sess) {
        try {
            sess.IsCursorCaptureEnabled(cursorCapture);
            DBG("ScreenCapture: Cursor capture %s", en ? "enabled" : "disabled");
        } catch(const winrt::hresult_error& e) {
            WARN("ScreenCapture: IsCursorCaptureEnabled failed: 0x%08X", e.code().value);
        }
    }
}
