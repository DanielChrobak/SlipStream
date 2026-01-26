#pragma once

#include "common.hpp"

struct FrameData {
    ID3D11Texture2D* tex = nullptr;
    int64_t ts = 0;
    uint64_t fence = 0;
    int poolIdx = -1;
    bool needsSync = false;
    void Release() { SafeRelease(tex); poolIdx = -1; needsSync = false; }
};

template<typename T = int64_t>
struct AtomicMinMaxAvg {
    std::atomic<T> minVal{std::numeric_limits<T>::max()}, maxVal{0}, sum{0};
    std::atomic<uint64_t> count{0};

    void Record(T val) {
        if (val <= 0) return;
        count++; sum += val;
        for (T m = minVal.load(); val < m && !minVal.compare_exchange_weak(m, val););
        for (T m = maxVal.load(); val > m && !maxVal.compare_exchange_weak(m, val););
    }

    void Reset() { minVal = std::numeric_limits<T>::max(); maxVal = sum = 0; count = 0; }

    struct Snap { T min, max, avg; uint64_t n; };

    Snap GetAndReset() {
        Snap s{minVal.exchange(std::numeric_limits<T>::max()), maxVal.exchange(0), 0, count.exchange(0)};
        T sm = sum.exchange(0);
        s.avg = s.n > 0 ? sm / static_cast<T>(s.n) : 0;
        if (s.min == std::numeric_limits<T>::max()) s.min = 0;
        return s;
    }
};

struct GPUWaitMetrics {
    AtomicMinMaxAvg<int64_t> waits;
    std::atomic<uint64_t> timeoutCount{0}, alreadyCompleteCount{0};

    void RecordWait(int64_t waitUs, bool timedOut) {
        if (timedOut) timeoutCount++;
        else if (waitUs <= 0) alreadyCompleteCount++;
        else waits.Record(waitUs);
    }

    struct Snapshot { int64_t minUs, maxUs, avgUs; uint64_t count, timeouts, noWaitCount; };

    Snapshot GetAndReset() {
        auto s = waits.GetAndReset();
        return {s.min, s.max, s.avg, s.n, timeoutCount.exchange(0), alreadyCompleteCount.exchange(0)};
    }
};

class FrameSlot {
    static constexpr int SLOT_COUNT = 4;
    FrameData frames[SLOT_COUNT];
    CRITICAL_SECTION cs;
    HANDLE newFrameEvent;
    int head = 0, tail = 0, count = 0;
    uint32_t inFlightMask = 0;
    std::atomic<uint64_t> droppedCount{0};

public:
    FrameSlot() {
        InitializeCriticalSection(&cs);
        newFrameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        for (int i = 0; i < SLOT_COUNT; i++) frames[i] = {};
    }

    ~FrameSlot() {
        DeleteCriticalSection(&cs);
        CloseHandle(newFrameEvent);
        for (int i = 0; i < SLOT_COUNT; i++) frames[i].Release();
    }

    void Push(ID3D11Texture2D* tex, int64_t ts, uint64_t fence, bool needsSync, int poolIdx = -1) {
        EnterCriticalSection(&cs);
        if (count >= SLOT_COUNT) {
            droppedCount++;
            int oldPoolIdx = frames[tail].poolIdx;
            if (oldPoolIdx >= 0) inFlightMask &= ~(1u << oldPoolIdx);
            frames[tail].Release();
            tail = (tail + 1) % SLOT_COUNT;
            count--;
        }
        tex->AddRef();
        frames[head] = {tex, ts, fence, poolIdx, needsSync};
        if (poolIdx >= 0) inFlightMask |= (1u << poolIdx);
        head = (head + 1) % SLOT_COUNT;
        count++;
        SetEvent(newFrameEvent);
        LeaveCriticalSection(&cs);
    }

    bool Pop(FrameData& out) {
        WaitForSingleObject(newFrameEvent, INFINITE);
        EnterCriticalSection(&cs);
        if (count == 0) { LeaveCriticalSection(&cs); return false; }
        out = frames[tail];
        frames[tail].tex = nullptr;
        frames[tail].poolIdx = -1;
        frames[tail].needsSync = false;
        tail = (tail + 1) % SLOT_COUNT;
        count--;
        if (count > 0) SetEvent(newFrameEvent);
        LeaveCriticalSection(&cs);
        return true;
    }

    void Wake() { SetEvent(newFrameEvent); }

    void MarkReleased(int idx) {
        if (idx < 0) return;
        EnterCriticalSection(&cs);
        inFlightMask &= ~(1u << idx);
        LeaveCriticalSection(&cs);
    }

    bool IsInFlight(int idx) {
        if (idx < 0) return false;
        EnterCriticalSection(&cs);
        bool r = (inFlightMask & (1u << idx)) != 0;
        LeaveCriticalSection(&cs);
        return r;
    }

    void Reset() {
        EnterCriticalSection(&cs);
        for (int i = 0; i < SLOT_COUNT; i++) frames[i].Release();
        head = tail = count = 0;
        inFlightMask = 0;
        ResetEvent(newFrameEvent);
        LeaveCriticalSection(&cs);
    }

    uint64_t GetDropped() { return droppedCount.exchange(0); }
};

class GPUSync {
    ID3D11Device5* device5 = nullptr;
    ID3D11DeviceContext4* context4 = nullptr;
    ID3D11Fence* fence = nullptr;
    ID3D11Query* query = nullptr;
    HANDLE fenceEvent = nullptr, queryEvent = nullptr;
    uint64_t fenceValue = 0;
    bool useFence = false;

    static inline const int64_t qpcFreq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    int64_t GetUs() const { LARGE_INTEGER n; QueryPerformanceCounter(&n); return (n.QuadPart * 1000000) / qpcFreq; }

public:
    GPUWaitMetrics metrics;

    bool Init(ID3D11Device* device, ID3D11DeviceContext* context) {
        queryEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!queryEvent) return false;

        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device5))) &&
            SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context4))) &&
            SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)))) {
            fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            useFence = true;
            LOG("GPU sync: Fence");
            return true;
        }

        SafeRelease(device5, context4, fence);
        D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
        if (SUCCEEDED(device->CreateQuery(&qd, &query))) { LOG("GPU sync: Query"); return true; }
        return false;
    }

    ~GPUSync() {
        if (fenceEvent) CloseHandle(fenceEvent);
        if (queryEvent) CloseHandle(queryEvent);
        SafeRelease(fence, context4, device5, query);
    }

    uint64_t Signal(ID3D11DeviceContext* context, bool& needsSync) {
        needsSync = true;
        if (useFence && context4 && fence) { context4->Signal(fence, ++fenceValue); return fenceValue; }
        if (query) context->End(query);
        return 0;
    }

    bool IsComplete(uint64_t value, ID3D11DeviceContext* context) {
        if (useFence) return !fence || fence->GetCompletedValue() >= value;
        return !query || context->GetData(query, nullptr, 0, 0) == S_OK;
    }

    bool Wait(uint64_t value, ID3D11DeviceContext* context, ID3D11Multithread* multithread = nullptr, DWORD timeoutMs = 16) {
        int64_t startUs = GetUs();

        if (useFence && fence && fenceEvent) {
            if (fence->GetCompletedValue() >= value) { metrics.RecordWait(0, false); return true; }
            fence->SetEventOnCompletion(value, fenceEvent);
            bool completed = (WaitForSingleObject(fenceEvent, timeoutMs) == WAIT_OBJECT_0) || (fence->GetCompletedValue() >= value);
            metrics.RecordWait(GetUs() - startUs, !completed);
            return completed;
        }

        if (query && queryEvent) {
            { MTLock lock(multithread); context->Flush(); if (context->GetData(query, nullptr, 0, 0) == S_OK) { metrics.RecordWait(0, false); return true; } }
            bool completed = false;
            for (DWORD elapsed = 0; elapsed < timeoutMs && !completed; elapsed++) {
                WaitForSingleObject(queryEvent, 1);
                MTLock lock(multithread);
                completed = context->GetData(query, nullptr, 0, 0) == S_OK;
            }
            if (!completed) { MTLock lock(multithread); completed = context->GetData(query, nullptr, 0, 0) == S_OK; }
            metrics.RecordWait(GetUs() - startUs, !completed);
            return completed;
        }
        metrics.RecordWait(0, false);
        return true;
    }

    bool UsesFence() const { return useFence; }
    GPUWaitMetrics::Snapshot GetWaitMetrics() { return metrics.GetAndReset(); }
    bool IsFenceComplete(uint64_t value) const { return !useFence || !fence || fence->GetCompletedValue() >= value; }
};

struct CaptureMetrics {
    std::atomic<uint64_t> frameCount{0}, capturedCount{0}, skippedCount{0}, missedCount{0};
    AtomicMinMaxAvg<int64_t> intervals;
    std::atomic<int64_t> lastIntervalUs{0};

    void RecordInterval(int64_t us) { lastIntervalUs = us; intervals.Record(us); }
    void Reset() { frameCount = capturedCount = skippedCount = missedCount = lastIntervalUs = 0; intervals.Reset(); }

    struct Snapshot { uint64_t frames, captured, skipped, missed; int64_t lastUs, minUs, maxUs, avgUs; };

    Snapshot GetAndReset() {
        auto i = intervals.GetAndReset();
        return {frameCount.exchange(0), capturedCount.exchange(0), skippedCount.exchange(0),
                missedCount.exchange(0), lastIntervalUs.load(), i.min, i.max, i.avg};
    }
};

struct EncoderThreadMetrics {
    AtomicMinMaxAvg<int64_t> handoffs, encodes;
    std::atomic<uint64_t> deadlineMissCount{0}, stateDropCount{0};
    std::atomic<int64_t> worstLatenessUs{0};

    void RecordHandoff(int64_t us) { handoffs.Record(us); }
    void RecordEncode(int64_t us) { encodes.Record(us); }

    void RecordDeadlineMiss(int64_t latenessUs) {
        deadlineMissCount++;
        for (int64_t w = worstLatenessUs.load(); latenessUs > w && !worstLatenessUs.compare_exchange_weak(w, latenessUs););
    }

    struct Snapshot {
        int64_t handoffMinUs, handoffMaxUs, handoffAvgUs; uint64_t handoffCount;
        int64_t encodeMinUs, encodeMaxUs, encodeAvgUs; uint64_t encodeCount;
        uint64_t deadlineMisses, stateDrops; int64_t worstLatenessUs;
    };

    Snapshot GetAndReset() {
        auto h = handoffs.GetAndReset();
        auto e = encodes.GetAndReset();
        return {h.min, h.max, h.avg, h.n, e.min, e.max, e.avg, e.n,
                deadlineMissCount.exchange(0), stateDropCount.exchange(0), worstLatenessUs.exchange(0)};
    }
};

class ScreenCapture {
    static constexpr int TEX_POOL_SIZE = 6, WGC_FRAME_POOL_SIZE = 4;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Multithread* multithread = nullptr;

    WGD::Direct3D11::IDirect3DDevice winrtDevice{nullptr};
    WGC::GraphicsCaptureItem captureItem{nullptr};
    WGC::Direct3D11CaptureFramePool framePool{nullptr};
    WGC::GraphicsCaptureSession captureSession{nullptr};

    ID3D11Texture2D* texturePool[TEX_POOL_SIZE] = {};
    uint64_t textureFences[TEX_POOL_SIZE] = {};
    int textureIndex = 0;

    int width = 0, height = 0, hostFps = 60;
    std::atomic<int> targetFps{60}, currentMonitorIdx{0};

    GPUSync gpuSync;
    FrameSlot* frameSlot;

    std::atomic<bool> running{true}, capturing{false}, sessionStarted{false};
    bool supportsMinInterval = false;

    HMONITOR currentMonitor = nullptr;
    std::mutex captureMutex;
    std::function<void(int, int, int)> onResolutionChange;

    std::atomic<uint64_t> textureConflicts{0};
    int64_t lastCaptureTimestamp = 0, minFrameIntervalUs = 0;
    CaptureMetrics metrics;

    int FindAvailableTexture() {
        for (int i = 0; i < TEX_POOL_SIZE; i++) {
            int idx = (textureIndex + i) % TEX_POOL_SIZE;
            if (!frameSlot->IsInFlight(idx) && gpuSync.IsFenceComplete(textureFences[idx])) {
                textureIndex = idx + 1;
                return idx;
            }
        }
        for (int i = 0; i < TEX_POOL_SIZE; i++) {
            int idx = (textureIndex + i) % TEX_POOL_SIZE;
            if (!frameSlot->IsInFlight(idx)) {
                if (textureFences[idx] > 0 && !gpuSync.IsFenceComplete(textureFences[idx])) {
                    MTLock lock(multithread);
                    gpuSync.Wait(textureFences[idx], context, multithread, 4);
                }
                textureIndex = idx + 1;
                return idx;
            }
        }
        textureConflicts++;
        return -1;
    }

    void OnFrameArrived(WGC::Direct3D11CaptureFramePool const& sender, winrt::Windows::Foundation::IInspectable const&) {
        auto frame = sender.TryGetNextFrame();
        if (!frame || !running || !capturing) return;

        int64_t timestamp = GetTimestamp();
        metrics.frameCount++;

        if (minFrameIntervalUs > 0 && lastCaptureTimestamp > 0 && (timestamp - lastCaptureTimestamp) < minFrameIntervalUs) {
            metrics.skippedCount++;
            return;
        }
        if (lastCaptureTimestamp > 0) metrics.RecordInterval(timestamp - lastCaptureTimestamp);
        lastCaptureTimestamp = timestamp;

        auto surface = frame.Surface();
        if (!surface) { metrics.missedCount++; return; }

        winrt::com_ptr<ID3D11Texture2D> sourceTexture;
        auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        if (FAILED(access->GetInterface(IID_PPV_ARGS(sourceTexture.put()))) || !sourceTexture) { metrics.missedCount++; return; }

        int texIdx = FindAvailableTexture();
        if (texIdx < 0 || !texturePool[texIdx]) { metrics.missedCount++; return; }

        bool needsSync = false;
        uint64_t fenceValue = 0;
        {
            MTLock lock(multithread);
            context->CopyResource(texturePool[texIdx], sourceTexture.get());
            context->Flush();
            fenceValue = gpuSync.Signal(context, needsSync);
        }

        textureFences[texIdx] = fenceValue;
        frameSlot->Push(texturePool[texIdx], timestamp, fenceValue, needsSync, texIdx);
        metrics.capturedCount++;
    }

    void InitializeMonitor(HMONITOR monitor) {
        MONITORINFOEXW mi{sizeof(mi)};
        DEVMODEW dm{.dmSize = sizeof(dm)};
        if (GetMonitorInfoW(monitor, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
            hostFps = dm.dmDisplayFrequency;

        targetFps = hostFps;
        UpdateFrameInterval();

        auto interop = winrt::get_activation_factory<WGC::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        if (FAILED(interop->CreateForMonitor(monitor, winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(), winrt::put_abi(captureItem))))
            throw std::runtime_error("Failed to create capture item for monitor");

        width = captureItem.Size().Width;
        height = captureItem.Size().Height;

        for (auto& tex : texturePool) SafeRelease(tex);
        for (auto& fence : textureFences) fence = 0;

        D3D11_TEXTURE2D_DESC td = {static_cast<UINT>(width), static_cast<UINT>(height), 1, 1,
            DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, D3D11_RESOURCE_MISC_SHARED};

        for (int i = 0; i < TEX_POOL_SIZE; i++)
            if (FAILED(device->CreateTexture2D(&td, nullptr, &texturePool[i])))
                throw std::runtime_error("Failed to create texture pool");

        framePool = WGC::Direct3D11CaptureFramePool::CreateFreeThreaded(winrtDevice,
            WGD::DirectXPixelFormat::B8G8R8A8UIntNormalized, WGC_FRAME_POOL_SIZE, {width, height});
        framePool.FrameArrived({this, &ScreenCapture::OnFrameArrived});

        captureSession = framePool.CreateCaptureSession(captureItem);
        captureSession.IsCursorCaptureEnabled(true);
        try { captureSession.IsBorderRequired(false); } catch (...) {}
        if (supportsMinInterval) try { captureSession.MinUpdateInterval(duration<int64_t, std::ratio<1, 10000000>>(0)); } catch (...) {}

        sessionStarted = false;
        currentMonitor = monitor;
    }

    void UpdateFrameInterval() {
        int fps = targetFps.load();
        minFrameIntervalUs = fps > 0 ? (800000 / fps) : 0;
    }

public:
    explicit ScreenCapture(FrameSlot* slot) : frameSlot(slot) {
        LOG("Initializing screen capture...");
        winrt::init_apartment(winrt::apartment_type::multi_threaded);

        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL actual;

        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, _countof(levels),
            D3D11_SDK_VERSION, &device, &actual, &context)))
            throw std::runtime_error("Failed to create D3D11 device");

        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&multithread)))) multithread->SetMultithreadProtected(TRUE);
        if (!gpuSync.Init(device, context)) throw std::runtime_error("Failed to initialize GPU synchronization");

        winrt::com_ptr<IDXGIDevice> dxgi;
        winrt::com_ptr<::IInspectable> insp;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(dxgi.put()))) ||
            FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), insp.put())))
            throw std::runtime_error("Failed to create WinRT device");

        winrtDevice = insp.as<WGD::Direct3D11::IDirect3DDevice>();
        RefreshMonitorList();

        supportsMinInterval = winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
            L"Windows.Graphics.Capture.GraphicsCaptureSession", L"MinUpdateInterval");

        InitializeMonitor(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY));
        LOG("Capture initialized: %dx%d @ %dHz (sync: %s)", width, height, hostFps, gpuSync.UsesFence() ? "fence" : "query");
    }

    ~ScreenCapture() {
        running = capturing = false;
        try { if (captureSession) captureSession.Close(); } catch (...) {}
        try { if (framePool) framePool.Close(); } catch (...) {}
        for (auto& tex : texturePool) SafeRelease(tex);
        SafeRelease(multithread, context, device);
        winrt::uninit_apartment();
    }

    void SetResolutionChangeCallback(std::function<void(int, int, int)> cb) { onResolutionChange = cb; }

    void StartCapture() {
        std::lock_guard<std::mutex> lock(captureMutex);
        if (capturing) return;
        frameSlot->Reset();
        textureIndex = 0;
        lastCaptureTimestamp = 0;
        metrics.Reset();
        for (auto& fence : textureFences) fence = 0;
        if (!sessionStarted.exchange(true)) captureSession.StartCapture();
        capturing = true;
        LOG("Capture started at target %dHz", targetFps.load());
    }

    void PauseCapture() { if (capturing) capturing = false; }

    bool SwitchMonitor(int index) {
        std::lock_guard<std::mutex> lock(captureMutex);
        std::lock_guard<std::mutex> ml(g_monitorsMutex);

        if (index < 0 || index >= static_cast<int>(g_monitors.size())) return false;
        if (currentMonitorIdx == index && currentMonitor == g_monitors[index].hMon) return true;

        bool wasCapturing = capturing.load();
        capturing = false;

        try { if (captureSession) captureSession.Close(); } catch (...) {}
        try { if (framePool) framePool.Close(); } catch (...) {}

        captureSession = nullptr;
        framePool = nullptr;
        captureItem = nullptr;
        frameSlot->Reset();
        textureIndex = 0;

        try {
            InitializeMonitor(g_monitors[index].hMon);
            currentMonitorIdx = index;
            LOG("Switched to monitor %d: %dx%d @ %dHz", index, width, height, hostFps);
            if (onResolutionChange) onResolutionChange(width, height, hostFps);
            if (wasCapturing) { captureSession.StartCapture(); sessionStarted = true; capturing = true; }
            return true;
        } catch (const std::exception& e) { ERR("Monitor switch failed: %s", e.what()); return false; }
    }

    bool SetFPS(int fps) {
        if (fps < 1 || fps > 240) return false;
        int oldFps = targetFps.exchange(fps);
        if (oldFps != fps) UpdateFrameInterval();
        return true;
    }

    int RefreshHostFPS() {
        if (currentMonitor) {
            MONITORINFOEXW mi{sizeof(mi)};
            DEVMODEW dm{.dmSize = sizeof(dm)};
            if (GetMonitorInfoW(currentMonitor, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
                hostFps = dm.dmDisplayFrequency;
        }
        return hostFps;
    }

    int GetCurrentMonitorIndex() const { return currentMonitorIdx; }
    int GetHostFPS() const { return hostFps; }
    int GetCurrentFPS() const { return targetFps; }
    bool IsCapturing() const { return capturing; }

    bool IsReady(uint64_t fence) {
        if (gpuSync.UsesFence()) return gpuSync.IsFenceComplete(fence);
        MTLock lock(multithread);
        return gpuSync.IsComplete(fence, context);
    }

    bool WaitReady(uint64_t fence) { return gpuSync.Wait(fence, context, multithread, 16); }
    uint64_t GetTexConflicts() { return textureConflicts.exchange(0); }
    ID3D11Device* GetDev() const { return device; }
    ID3D11DeviceContext* GetCtx() const { return context; }
    ID3D11Multithread* GetMT() const { return multithread; }
    int GetW() const { return width; }
    int GetH() const { return height; }
    CaptureMetrics::Snapshot GetMetrics() { return metrics.GetAndReset(); }
    GPUWaitMetrics::Snapshot GetGPUWaitMetrics() { return gpuSync.GetWaitMetrics(); }
};
