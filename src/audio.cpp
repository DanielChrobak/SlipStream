#include "audio.hpp"

void AudioCapture::Loop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if(FAILED(hr)) { ERR("AudioCapture: CoInitializeEx failed: 0x%08X", hr); return; }

    while(running.load(std::memory_order_acquire)) {
        if(!capturing.load(std::memory_order_acquire) || !cap || !init.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(10ms); continue;
        }

        UINT32 pktLen=0;
        hr = cap->GetNextPacketSize(&pktLen);
        if(FAILED(hr)) {
            if(consecutiveErrors.fetch_add(1) < MAX_CONSECUTIVE_ERRORS) {
                WARN("AudioCapture: GetNextPacketSize failed: 0x%08X", hr);
            }
            std::this_thread::sleep_for(50ms); continue;
        }
        consecutiveErrors.store(0);

        while(pktLen > 0 && running.load(std::memory_order_acquire) && capturing.load(std::memory_order_acquire) && cap) {
            BYTE* data=nullptr; UINT32 frames=0; DWORD flags=0;
            hr = cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if(FAILED(hr)) {
                if(consecutiveErrors.fetch_add(1) < MAX_CONSECUTIVE_ERRORS) {
                    WARN("AudioCapture: GetBuffer failed: 0x%08X", hr);
                }
                break;
            }
            consecutiveErrors.store(0);

            if(data && frames > 0) {
                if(flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    silBuf.assign(frames*ch, 0.0f);
                    Process(silBuf.data(), frames, GetTimestamp());
                } else {
                    Process((float*)data, frames, GetTimestamp());
                }
                if(flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) DBG("AudioCapture: Data discontinuity detected");
                if(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) DBG("AudioCapture: Timestamp error flag set");
            }

            hr = cap->ReleaseBuffer(frames);
            if(FAILED(hr)) WARN("AudioCapture: ReleaseBuffer failed: 0x%08X", hr);
            if(!cap) break;
            hr = cap->GetNextPacketSize(&pktLen);
            if(FAILED(hr)) { DBG("AudioCapture: GetNextPacketSize failed in loop: 0x%08X", hr); break; }
        }
        std::this_thread::sleep_for(2ms);
    }
    CoUninitialize();
    DBG("AudioCapture: Loop thread exiting");
}

void AudioCapture::Process(const float* data, UINT32 frames, int64_t ts) {
    if(!enc || !resampler) { DBG("AudioCapture: Process called but encoder/resampler not ready"); return; }
    if(!streaming.load(std::memory_order_acquire)) {
        for(int c=0; c<ch; c++) resampler->buf.clear();
        return;
    }

    resampler->Process(data, frames);
    const size_t maxBuf = FRAME_SZ * ch * 6;

    if(resampler->buf.size() > maxBuf) {
        DBG("AudioCapture: Resampler buffer overflow, dropping %zu samples", resampler->buf.size() - FRAME_SZ*ch*2);
        resampler->buf.erase(resampler->buf.begin(), resampler->buf.begin() + (resampler->buf.size() - FRAME_SZ*ch*2));
    }

    while(resampler->buf.size() >= (size_t)(FRAME_SZ*ch)) {
        for(int i=0; i<FRAME_SZ*ch; i++) {
            encBuf[i] = (int16_t)(std::clamp(resampler->buf[i], -1.0f, 1.0f) * 32767.0f);
        }
        resampler->buf.erase(resampler->buf.begin(), resampler->buf.begin() + FRAME_SZ*ch);

        int bytes = opus_encode(enc, encBuf.data(), FRAME_SZ, outBuf.data(), (opus_int32)outBuf.size());
        if(bytes < 0) {
            if(bytes != OPUS_BUFFER_TOO_SMALL) WARN("AudioCapture: Opus encode error: %s", opus_strerror(bytes));
            continue;
        }

        if(bytes > 0) {
            std::lock_guard<std::mutex> lk(qMtx);
            if(q.size() >= MAX_Q) {
                DBG("AudioCapture: Queue full, dropping oldest packet");
                q.pop();
            }
            q.push({{outBuf.begin(), outBuf.begin()+bytes}, ts, FRAME_SZ});
            qCv.notify_one();
        }
    }
}

AudioCapture::AudioCapture() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if(FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        ERR("AudioCapture: CoInitializeEx failed: 0x%08X", hr);
        throw std::runtime_error("COM initialization failed");
    }

    auto chk = [](HRESULT hr, const char* m) {
        if(FAILED(hr)) { ERR("AudioCapture: %s failed: 0x%08X", m, hr); throw std::runtime_error(m); }
    };

    chk(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                        __uuidof(IMMDeviceEnumerator), (void**)&enm), "MMDeviceEnumerator");
    chk(enm->GetDefaultAudioEndpoint(eRender, eConsole, &dev), "GetDefaultAudioEndpoint");
    chk(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&cli), "AudioClient activation");
    chk(cli->GetMixFormat(&wf), "GetMixFormat");

    sysRate = wf->nSamplesPerSec;
    ch = std::min((int)wf->nChannels, 2);
    DBG("AudioCapture: System format: %d Hz, %d channels, %d bits", sysRate, wf->nChannels, wf->wBitsPerSample);

    hr = cli->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 30000, 0, wf, nullptr);
    if(FAILED(hr)) {
        ERR("AudioCapture: Initialize failed: 0x%08X (loopback may not be supported)", hr);
        throw std::runtime_error("AudioClient Initialize failed");
    }

    chk(cli->GetService(__uuidof(IAudioCaptureClient), (void**)&cap), "GetService IAudioCaptureClient");

    int err;
    enc = opus_encoder_create(RATE, ch, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
    if(err != OPUS_OK) {
        ERR("AudioCapture: Opus encoder creation failed: %s", opus_strerror(err));
        throw std::runtime_error("Opus encoder creation failed");
    }

    auto setOpus = [this](int request, int value, const char* name) {
        int err = opus_encoder_ctl(enc, request, value);
        if(err != OPUS_OK) WARN("AudioCapture: Failed to set Opus %s: %s", name, opus_strerror(err));
    };
    setOpus(OPUS_SET_BITRATE_REQUEST, 96000, "bitrate");
    setOpus(OPUS_SET_COMPLEXITY_REQUEST, 3, "complexity");
    setOpus(OPUS_SET_SIGNAL_REQUEST, OPUS_SIGNAL_MUSIC, "signal type");
    setOpus(OPUS_SET_PACKET_LOSS_PERC_REQUEST, 0, "packet loss");
    setOpus(OPUS_SET_INBAND_FEC_REQUEST, 0, "inband FEC");
    setOpus(OPUS_SET_DTX_REQUEST, 0, "DTX");

    encBuf.resize(FRAME_SZ * ch);
    outBuf.resize(4000);
    resampler = std::make_unique<LinearResampler<float>>(sysRate, RATE, ch);

    init.store(true, std::memory_order_release);
    LOG("Audio: %dHz -> %dHz, %dch", sysRate, RATE, ch);
    CoUninitialize();
}

AudioCapture::~AudioCapture() {
    Stop();
    init.store(false, std::memory_order_release);
    if(enc) opus_encoder_destroy(enc);
    if(wf) CoTaskMemFree(wf);
    SafeRelease(cap, cli, dev, enm);
    DBG("AudioCapture: Destroyed");
}

void AudioCapture::Start() {
    if(running.load(std::memory_order_acquire) || !init.load(std::memory_order_acquire)) {
        DBG("AudioCapture: Start called but already running or not initialized");
        return;
    }
    running.store(true, std::memory_order_release);
    capturing.store(true, std::memory_order_release);
    consecutiveErrors.store(0);
    resampler->Reset();

    HRESULT hr = cli->Start();
    if(FAILED(hr)) {
        ERR("AudioCapture: IAudioClient::Start failed: 0x%08X", hr);
        running.store(false, std::memory_order_release);
        capturing.store(false, std::memory_order_release);
        return;
    }
    thr = std::thread(&AudioCapture::Loop, this);
    LOG("AudioCapture: Started");
}

void AudioCapture::Stop() {
    if(!running.load(std::memory_order_acquire)) return;
    running.store(false, std::memory_order_release);
    capturing.store(false, std::memory_order_release);
    streaming.store(false, std::memory_order_release);
    qCv.notify_all();
    if(thr.joinable()) thr.join();
    if(cli) {
        HRESULT hr = cli->Stop();
        if(FAILED(hr)) WARN("AudioCapture: IAudioClient::Stop failed: 0x%08X", hr);
    }
    LOG("AudioCapture: Stopped");
}

void AudioCapture::SetStreaming(bool s) {
    bool was = streaming.exchange(s, std::memory_order_acq_rel);
    if(s && !was) {
        ClearQueue(q, qMtx);
        resampler->Reset();
        DBG("AudioCapture: Streaming enabled");
    } else if(!s && was) {
        DBG("AudioCapture: Streaming disabled");
    }
}

bool AudioCapture::PopPacket(AudioPacket& out, int ms) {
    std::unique_lock<std::mutex> lk(qMtx);
    if(!qCv.wait_for(lk, std::chrono::milliseconds(ms), [this] {
        return !q.empty() || !running.load(std::memory_order_acquire);
    })) return false;
    if(q.empty()) return false;
    out = std::move(q.front());
    q.pop();
    return true;
}
