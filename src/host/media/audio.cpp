#include "host/media/audio.hpp"

using namespace std::chrono_literals;

namespace {
void InitCOM() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) throw std::runtime_error("COM initialization failed");
}
void ChkHR(HRESULT h, const char* m) { if (FAILED(h)) throw std::runtime_error(m); }
}

// ═══════════════════════════════════════════════════════════════
// AudioCapture — loopback capture → Opus encode → queue
// ═══════════════════════════════════════════════════════════════

AudioCapture::AudioCapture() {
    InitCOM();

    ChkHR(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                           __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&deviceEnumerator)), "MMDeviceEnumerator");
    ChkHR(deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &audioDevice), "GetDefaultAudioEndpoint");
    ChkHR(audioDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audioClient)), "AudioClient activation");
    ChkHR(audioClient->GetMixFormat(&waveFormat), "GetMixFormat");

    systemSampleRate = waveFormat->nSamplesPerSec;
    channelCount = std::min(static_cast<int>(waveFormat->nChannels), 2);

    ChkHR(audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 30000, 0, waveFormat, nullptr),
           "AudioClient Initialize");
    ChkHR(audioClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&captureClient)), "IAudioCaptureClient");

    int err;
    opusEncoder = opus_encoder_create(kSampleRate, channelCount, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
    if (err != OPUS_OK) throw std::runtime_error("Opus encoder creation failed");

    opus_encoder_ctl(opusEncoder, OPUS_SET_BITRATE(96000));
    opus_encoder_ctl(opusEncoder, OPUS_SET_COMPLEXITY(3));
    opus_encoder_ctl(opusEncoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));

    encodeBuffer.resize(kFrameSamples * channelCount);
    opusBuffer.resize(4000);
    resampler = std::make_unique<SpeexResampler>(systemSampleRate, kSampleRate, channelCount);

    isInitialized = true;
    LOG("Audio: %dHz -> %dHz, %dch", systemSampleRate, kSampleRate, channelCount);
    CoUninitialize();
}

AudioCapture::~AudioCapture() {
    Stop();
    if (opusEncoder) opus_encoder_destroy(opusEncoder);
    if (waveFormat) CoTaskMemFree(waveFormat);
    SafeRelease(captureClient, audioClient, audioDevice, deviceEnumerator);
}

void AudioCapture::Loop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    std::vector<float> silBuf;

    while (running.load(std::memory_order_acquire)) {
        if (!captureActive.load() || !captureClient || !isInitialized.load()) {
            std::this_thread::sleep_for(10ms);
            continue;
        }

        UINT32 packetLength = 0;
        HRESULT packetHr = captureClient->GetNextPacketSize(&packetLength);
        if (FAILED(packetHr)) {
            ERR("AudioCapture: GetNextPacketSize failed: 0x%08X", packetHr);
            std::this_thread::sleep_for(50ms);
            continue;
        }

        while (packetLength > 0 && running.load() && captureActive.load() && captureClient) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;

            HRESULT bufHr = captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(bufHr)) {
                ERR("AudioCapture: GetBuffer failed: 0x%08X", bufHr);
                break;
            }

            if (data && frames > 0) {
                float* src;
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    silBuf.assign(frames * channelCount, 0.0f);
                    src = silBuf.data();
                } else if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
                    WARN("AudioCapture: Data discontinuity detected (frames=%u) - possible audio glitch", frames);
                    src = reinterpret_cast<float*>(data);
                } else {
                    src = reinterpret_cast<float*>(data);
                }
                if (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) {
                    WARN("AudioCapture: Timestamp error flag set (frames=%u)", frames);
                }
                Process(src, frames, GetTimestamp());
            } else if (frames == 0) {
                DBG("AudioCapture: GetBuffer returned 0 frames (flags=0x%X)", flags);
            }

            HRESULT relHr = captureClient->ReleaseBuffer(frames);
            if (FAILED(relHr)) {
                ERR("AudioCapture: ReleaseBuffer failed: 0x%08X (frames=%u)", relHr, frames);
                break;
            }
            HRESULT nextHr = captureClient->GetNextPacketSize(&packetLength);
            if (FAILED(nextHr)) {
                ERR("AudioCapture: GetNextPacketSize failed in loop: 0x%08X", nextHr);
                break;
            }
        }
        std::this_thread::sleep_for(2ms);
    }
    CoUninitialize();
}

void AudioCapture::Process(const float* data, UINT32 frames, int64_t ts) {
    if (!opusEncoder || !resampler || !streaming.load()) {
        if (!streaming.load()) {
            resampler->buf.clear();
            resamplerReadPos = 0;
        }
        return;
    }

    resampler->Process(data, frames);

    const size_t bufferedSamples = resampler->buf.size() > resamplerReadPos
        ? (resampler->buf.size() - resamplerReadPos)
        : 0;
    const size_t maxBuf = kFrameSamples * channelCount * 6;
    if (bufferedSamples > maxBuf) {
        size_t dropSamples = bufferedSamples - kFrameSamples * channelCount * 2;
        WARN("AudioCapture: Resampler buffer overflow, dropping %zu samples (buf=%zu, max=%zu)",
            dropSamples / channelCount, bufferedSamples / channelCount, maxBuf / channelCount);
        resamplerReadPos = std::min(resamplerReadPos + dropSamples, resampler->buf.size());
    }

    const size_t frameSamples = static_cast<size_t>(kFrameSamples * channelCount);
    while (resampler->buf.size() >= resamplerReadPos + frameSamples) {
        const float* src = resampler->buf.data() + resamplerReadPos;
        for (int i = 0; i < kFrameSamples * channelCount; i++)
            encodeBuffer[i] = static_cast<int16_t>(std::clamp(src[i], -1.0f, 1.0f) * 32767.0f);
        resamplerReadPos += frameSamples;

        int bytes = opus_encode(opusEncoder, encodeBuffer.data(), kFrameSamples, opusBuffer.data(), static_cast<opus_int32>(opusBuffer.size()));
        if (bytes < 0) {
            ERR("AudioCapture: Opus encode error: %s (code=%d)", opus_strerror(bytes), bytes);
            continue;
        }
        if (bytes == 0) {
            DBG("AudioCapture: Opus encode returned 0 bytes (DTX silence?)");
            continue;
        }

        std::lock_guard<std::mutex> lk(queueMutex);
        PushBoundedQueue(packetQueue, kMaxQueueSize, AudioPacket{{opusBuffer.begin(), opusBuffer.begin() + bytes}, ts, kFrameSamples});
        queueCv.notify_one();
    }

    if (resamplerReadPos > 0 && (resamplerReadPos >= resampler->buf.size() || resamplerReadPos >= 8192)) {
        if (resamplerReadPos < resampler->buf.size()) {
            resampler->buf.erase(resampler->buf.begin(), resampler->buf.begin() + static_cast<std::ptrdiff_t>(resamplerReadPos));
        } else {
            resampler->buf.clear();
        }
        resamplerReadPos = 0;
    }
}

void AudioCapture::Start() {
    if (running.load() || !isInitialized.load()) {
        WARN("AudioCapture: Start called but running=%d init=%d", running.load() ? 1 : 0, isInitialized.load() ? 1 : 0);
        return;
    }
    running = true;
    captureActive = true;
    resampler->Reset();
    resamplerReadPos = 0;
    HRESULT startHr = audioClient->Start();
    if (FAILED(startHr)) {
        ERR("AudioCapture: IAudioClient::Start failed: 0x%08X", startHr);
        running = false;
        captureActive = false;
        return;
    }
    captureThread = std::thread(&AudioCapture::Loop, this);
    LOG("AudioCapture: Started (rate=%d, ch=%d)", systemSampleRate, channelCount);
}

void AudioCapture::Stop() {
    if (!running.load()) return;
    running = false;
    captureActive = false;
    streaming = false;
    queueCv.notify_all();
    if (captureThread.joinable()) captureThread.join();
    if (audioClient) audioClient->Stop();
    LOG("AudioCapture: Stopped");
}

void AudioCapture::SetStreaming(bool s) {
    bool was = streaming.exchange(s);
    if (s != was) {
        LOG("AudioCapture: Streaming %s -> %s", was ? "on" : "off", s ? "on" : "off");
    }
    if (s && !was) {
        ClearQueue(packetQueue, queueMutex);
        resampler->Reset();
        resamplerReadPos = 0;
    }
}

bool AudioCapture::PopPacket(AudioPacket& out, int ms) {
    std::unique_lock<std::mutex> lk(queueMutex);
    if (!queueCv.wait_for(lk, std::chrono::milliseconds(ms), [this] { return !packetQueue.empty() || !running.load(); }))
        return false;
    if (packetQueue.empty()) return false;
    out = std::move(packetQueue.front());
    packetQueue.pop();
    return true;
}

// ═══════════════════════════════════════════════════════════════
// MicPlayback — Opus decode → resample → WASAPI render
// ═══════════════════════════════════════════════════════════════

IMMDevice* MicPlayback::FindDevice(const std::string& name) {
    IMMDeviceCollection* col = nullptr;
    if (FAILED(deviceEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) return nullptr;
    UINT cnt = 0; col->GetCount(&cnt);
    auto lower = [](std::string s) { std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s; };
    std::string tl = lower(name);
    for (UINT i = 0; i < cnt; i++) {
        IMMDevice* d = nullptr;
        if (FAILED(col->Item(i, &d))) continue;
        IPropertyStore* ps = nullptr;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT var; PropVariantInit(&var);
            if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &var))) {
                char dn[256] = {};
                WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, dn, sizeof(dn), nullptr, nullptr);
                PropVariantClear(&var);
                if (lower(dn).find(tl) != std::string::npos) {
                    ps->Release(); col->Release();
                    actualDeviceName = dn; return d;
                }
            }
            ps->Release();
        }
        d->Release();
    }
    col->Release();
    WARN("MicPlayback: Device '%s' not found", name.c_str());
    return nullptr;
}

void MicPlayback::Loop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        ERR("MicPlayback: CoInitializeEx failed: 0x%08X", hr);
        return;
    }

    std::vector<float> decodedFloat(kFrameSamples);

    while (running.load(std::memory_order_acquire)) {
        if (!streaming.load(std::memory_order_acquire) || !renderClient || !init.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(10ms);
            continue;
        }

        std::vector<uint8_t> pkt;
        {
            std::unique_lock<std::mutex> lk(queueMutex);
            if (!queueCv.wait_for(lk, 5ms, [this] { return !packetQueue.empty() || !running.load(std::memory_order_acquire); }))
                continue;
            if (!running.load(std::memory_order_acquire) || packetQueue.empty()) continue;
            pkt = std::move(packetQueue.front());
            packetQueue.pop();
        }

        if (pkt.size() < sizeof(MicPacketHeader)) continue;

        auto* h = reinterpret_cast<MicPacketHeader*>(pkt.data());
        if (h->magic != MSG_MIC_DATA || h->dataLength > pkt.size() - sizeof(MicPacketHeader)) continue;

        packetsReceived++;

        int ds = opus_decode(opusDecoder, pkt.data() + sizeof(MicPacketHeader), h->dataLength, decodeBuffer.data(), kFrameSamples, 0);
        if (ds <= 0) {
            if (++decodeErrors % 100 == 1)
                WARN("MicPlayback: Opus decode error: %s (total: %llu)", opus_strerror(ds), decodeErrors.load());
            continue;
        }

        packetsDecoded++;
        for (int i = 0; i < ds; i++) decodedFloat[i] = decodeBuffer[i] / 32768.0f;
        resampler->ProcessMono(decodedFloat.data(), ds, channelCount);

        for (int attempts = 0; resampler->buf.size() > resamplerReadPos && running.load(std::memory_order_acquire) && attempts < 50; attempts++) {
            UINT32 bufFr = 0, pad = 0;
            if (FAILED(audioClient->GetBufferSize(&bufFr)) || FAILED(audioClient->GetCurrentPadding(&pad))) break;

            UINT32 avail = bufFr - pad;
            if (avail == 0) { std::this_thread::sleep_for(1ms); continue; }

            const size_t bufferedSamples = resampler->buf.size() - resamplerReadPos;
            UINT32 toW = std::min(avail, static_cast<UINT32>(bufferedSamples / channelCount));
            if (toW == 0) break;

            BYTE* buf = nullptr;
            if (FAILED(renderClient->GetBuffer(toW, &buf))) break;

            float* out = reinterpret_cast<float*>(buf);
            const float* src = resampler->buf.data() + resamplerReadPos;
            std::memcpy(out, src, static_cast<size_t>(toW) * channelCount * sizeof(float));

            if (FAILED(renderClient->ReleaseBuffer(toW, 0))) break;
            resamplerReadPos += static_cast<size_t>(toW) * channelCount;
            samplesWritten += toW;
        }

        const size_t bufferedSamples = resampler->buf.size() > resamplerReadPos
            ? (resampler->buf.size() - resamplerReadPos)
            : 0;
        if (bufferedSamples > static_cast<size_t>(kFrameSamples * channelCount * 10)) {
            size_t toDrop = bufferedSamples - static_cast<size_t>(kFrameSamples * channelCount * 4);
            resamplerReadPos = std::min(resamplerReadPos + toDrop, resampler->buf.size());
            bufferOverruns++;
            DBG("MicPlayback: Buffer overrun, dropped %zu samples", toDrop / channelCount);
        }

        if (resamplerReadPos > 0 && (resamplerReadPos >= resampler->buf.size() || resamplerReadPos >= 8192)) {
            if (resamplerReadPos < resampler->buf.size()) {
                resampler->buf.erase(resampler->buf.begin(), resampler->buf.begin() + static_cast<std::ptrdiff_t>(resamplerReadPos));
            } else {
                resampler->buf.clear();
            }
            resamplerReadPos = 0;
        }
    }
    CoUninitialize();
    DBG("MicPlayback: Loop thread exiting");
}

MicPlayback::MicPlayback(const std::string& deviceName) : targetDevice(deviceName) {
    InitCOM();

    ChkHR(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                           __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&deviceEnumerator)), "MMDeviceEnumerator");

    audioDevice = FindDevice(targetDevice);
    if (!audioDevice) {
        LOG("MicPlayback: '%s' not found, using default", targetDevice.c_str());
        ChkHR(deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &audioDevice), "GetDefaultAudioEndpoint");
        actualDeviceName = "(default output)";
    }

    ChkHR(audioDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audioClient)), "IAudioClient");
    ChkHR(audioClient->GetMixFormat(&waveFormat), "GetMixFormat");

    deviceSampleRate = waveFormat->nSamplesPerSec;
    channelCount = waveFormat->nChannels;
    DBG("MicPlayback: Device format: %dHz, %dch", deviceSampleRate, channelCount);

    HRESULT hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                 300000, 0, waveFormat, nullptr);
    if (FAILED(hr)) {
        hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 300000, 0, waveFormat, nullptr);
        if (FAILED(hr)) throw std::runtime_error("AudioClient Initialize failed");
        WARN("MicPlayback: Initialized without auto-conversion");
    }

    ChkHR(audioClient->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&renderClient)), "IAudioRenderClient");

    int err;
    opusDecoder = opus_decoder_create(kSampleRate, 1, &err);
    if (err != OPUS_OK) throw std::runtime_error("Opus decoder creation failed");

    decodeBuffer.resize(kFrameSamples);
    resampler = std::make_unique<SpeexResampler>(kSampleRate, deviceSampleRate, 1);
    init.store(true, std::memory_order_release);
    LOG("MicPlayback: %dHz -> %dHz, %dch, device: %s", kSampleRate, deviceSampleRate, channelCount, actualDeviceName.c_str());
    CoUninitialize();
}

MicPlayback::~MicPlayback() {
    Stop();
    init.store(false, std::memory_order_release);
    LOG("MicPlayback: Stats - recv:%llu dec:%llu err:%llu written:%llu overruns:%llu",
        packetsReceived.load(), packetsDecoded.load(), decodeErrors.load(),
        samplesWritten.load(), bufferOverruns.load());
    if (opusDecoder) opus_decoder_destroy(opusDecoder);
    if (waveFormat) CoTaskMemFree(waveFormat);
    SafeRelease(renderClient, audioClient, audioDevice, deviceEnumerator);
}

void MicPlayback::Start() {
    if (running.load(std::memory_order_acquire) || !init.load(std::memory_order_acquire)) return;
    running.store(true, std::memory_order_release);
    resampler->Reset();
    resamplerReadPos = 0;
    if (FAILED(audioClient->Start())) { ERR("MicPlayback: IAudioClient::Start failed"); running.store(false, std::memory_order_release); return; }
    playbackThread = std::thread(&MicPlayback::Loop, this);
    LOG("MicPlayback: Started");
}

void MicPlayback::Stop() {
    if (!running.load(std::memory_order_acquire)) return;
    running.store(false, std::memory_order_release);
    streaming.store(false, std::memory_order_release);
    queueCv.notify_all();
    if (playbackThread.joinable()) playbackThread.join();
    if (audioClient) audioClient->Stop();
    ClearQueue(packetQueue, queueMutex);
    LOG("MicPlayback: Stopped");
}

void MicPlayback::SetStreaming(bool s) {
    bool was = streaming.exchange(s, std::memory_order_acq_rel);
    if (s && !was) {
        ClearQueue(packetQueue, queueMutex);
        resampler->Reset();
        resamplerReadPos = 0;
    }
}

void MicPlayback::PushPacket(const uint8_t* data, size_t len) {
    if (!streaming.load(std::memory_order_acquire) || len < sizeof(MicPacketHeader)) {
        if (len < sizeof(MicPacketHeader) && len > 0) {
            WARN("MicPlayback: PushPacket too small (%zu < %zu)", len, sizeof(MicPacketHeader));
        }
        return;
    }
    std::lock_guard<std::mutex> lk(queueMutex);
    if (packetQueue.size() >= kMaxQueueSize) {
        DBG("MicPlayback: Queue full, oldest packet will be dropped (queue=%zu)", packetQueue.size());
    }
    PushBoundedQueue(packetQueue, kMaxQueueSize, std::vector<uint8_t>(data, data + len));
    queueCv.notify_one();
}
