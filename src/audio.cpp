#include "audio.hpp"

AudioCapture::AudioCapture() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) throw std::runtime_error("COM initialization failed");

    auto chk = [](HRESULT h, const char* m) { if (FAILED(h)) throw std::runtime_error(m); };

    chk(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                         __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enm)), "MMDeviceEnumerator");
    chk(enm->GetDefaultAudioEndpoint(eRender, eConsole, &dev), "GetDefaultAudioEndpoint");
    chk(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&cli)), "AudioClient activation");
    chk(cli->GetMixFormat(&wf), "GetMixFormat");

    sysRate = wf->nSamplesPerSec;
    ch = std::min(static_cast<int>(wf->nChannels), 2);

    chk(cli->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 30000, 0, wf, nullptr),
        "AudioClient Initialize");
    chk(cli->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&cap)), "IAudioCaptureClient");

    int err;
    enc = opus_encoder_create(RATE, ch, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
    if (err != OPUS_OK) throw std::runtime_error("Opus encoder creation failed");

    opus_encoder_ctl(enc, OPUS_SET_BITRATE(96000));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(3));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));

    encBuf.resize(FRAME_SZ * ch);
    outBuf.resize(4000);
    resampler = std::make_unique<LinearResampler<float>>(sysRate, RATE, ch);

    init = true;
    LOG("Audio: %dHz -> %dHz, %dch", sysRate, RATE, ch);
    CoUninitialize();
}

AudioCapture::~AudioCapture() {
    Stop();
    if (enc) opus_encoder_destroy(enc);
    if (wf) CoTaskMemFree(wf);
    SafeRelease(cap, cli, dev, enm);
}

void AudioCapture::Loop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    std::vector<float> silBuf;

    while (running.load(std::memory_order_acquire)) {
        if (!capturing.load() || !cap || !init.load()) {
            std::this_thread::sleep_for(10ms);
            continue;
        }

        UINT32 pktLen = 0;
        if (FAILED(cap->GetNextPacketSize(&pktLen))) {
            std::this_thread::sleep_for(50ms);
            continue;
        }

        while (pktLen > 0 && running.load() && capturing.load() && cap) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;

            if (FAILED(cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;

            if (data && frames > 0) {
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    silBuf.assign(frames * ch, 0.0f);
                    Process(silBuf.data(), frames, GetTimestamp());
                } else {
                    Process(reinterpret_cast<float*>(data), frames, GetTimestamp());
                }
            }

            cap->ReleaseBuffer(frames);
            if (FAILED(cap->GetNextPacketSize(&pktLen))) break;
        }
        std::this_thread::sleep_for(2ms);
    }
    CoUninitialize();
}

void AudioCapture::Process(const float* data, UINT32 frames, int64_t ts) {
    if (!enc || !resampler || !streaming.load()) {
        if (!streaming.load()) resampler->buf.clear();
        return;
    }

    resampler->Process(data, frames);

    const size_t maxBuf = FRAME_SZ * ch * 6;
    if (resampler->buf.size() > maxBuf)
        resampler->buf.erase(resampler->buf.begin(), resampler->buf.begin() + (resampler->buf.size() - FRAME_SZ * ch * 2));

    while (resampler->buf.size() >= (size_t)(FRAME_SZ * ch)) {
        for (int i = 0; i < FRAME_SZ * ch; i++)
            encBuf[i] = static_cast<int16_t>(std::clamp(resampler->buf[i], -1.0f, 1.0f) * 32767.0f);
        resampler->buf.erase(resampler->buf.begin(), resampler->buf.begin() + FRAME_SZ * ch);

        int bytes = opus_encode(enc, encBuf.data(), FRAME_SZ, outBuf.data(), static_cast<opus_int32>(outBuf.size()));
        if (bytes <= 0) continue;

        std::lock_guard<std::mutex> lk(qMtx);
        if (q.size() >= MAX_Q) q.pop();
        q.push({{outBuf.begin(), outBuf.begin() + bytes}, ts, FRAME_SZ});
        qCv.notify_one();
    }
}

void AudioCapture::Start() {
    if (running.load() || !init.load()) return;
    running = true;
    capturing = true;
    resampler->Reset();
    if (FAILED(cli->Start())) {
        running = false;
        capturing = false;
        return;
    }
    thr = std::thread(&AudioCapture::Loop, this);
    LOG("AudioCapture: Started");
}

void AudioCapture::Stop() {
    if (!running.load()) return;
    running = false;
    capturing = false;
    streaming = false;
    qCv.notify_all();
    if (thr.joinable()) thr.join();
    if (cli) cli->Stop();
    LOG("AudioCapture: Stopped");
}

void AudioCapture::SetStreaming(bool s) {
    bool was = streaming.exchange(s);
    if (s && !was) {
        ClearQueue(q, qMtx);
        resampler->Reset();
    }
}

bool AudioCapture::PopPacket(AudioPacket& out, int ms) {
    std::unique_lock<std::mutex> lk(qMtx);
    if (!qCv.wait_for(lk, std::chrono::milliseconds(ms), [this] { return !q.empty() || !running.load(); }))
        return false;
    if (q.empty()) return false;
    out = std::move(q.front());
    q.pop();
    return true;
}
