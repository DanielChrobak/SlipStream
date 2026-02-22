#include "mic.hpp"

IMMDevice* MicPlayback::FindDevice(const std::string& name) {
    IMMDeviceCollection* col = nullptr;
    if (FAILED(enm->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) {
        WARN("MicPlayback: EnumAudioEndpoints failed");
        return nullptr;
    }

    UINT cnt = 0;
    col->GetCount(&cnt);
    DBG("MicPlayback: Found %u render devices", cnt);

    for (UINT i = 0; i < cnt; i++) {
        IMMDevice* d = nullptr;
        if (FAILED(col->Item(i, &d))) continue;

        IPropertyStore* ps = nullptr;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT var;
            PropVariantInit(&var);
            if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &var))) {
                char dn[256] = {};
                WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, dn, sizeof(dn), nullptr, nullptr);
                PropVariantClear(&var);
                ps->Release();

                std::string dl = dn, tl = name;
                std::transform(dl.begin(), dl.end(), dl.begin(), ::tolower);
                std::transform(tl.begin(), tl.end(), tl.begin(), ::tolower);
                DBG("MicPlayback: Device %u: %s", i, dn);

                if (dl.find(tl) != std::string::npos) {
                    col->Release();
                    LOG("MicPlayback: Found target device: %s", dn);
                    actualDevName = dn;
                    return d;
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

    std::vector<float> decF(FRAME_SZ);

    while (running.load(std::memory_order_acquire)) {
        if (!streaming.load(std::memory_order_acquire) || !rnd || !init.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(10ms);
            continue;
        }

        std::vector<uint8_t> pkt;
        {
            std::unique_lock<std::mutex> lk(qMtx);
            if (!qCv.wait_for(lk, 5ms, [this] { return !pktQ.empty() || !running.load(std::memory_order_acquire); }))
                continue;
            if (!running.load(std::memory_order_acquire) || pktQ.empty()) continue;
            pkt = std::move(pktQ.front());
            pktQ.pop();
        }

        if (pkt.size() < sizeof(MicPacketHeader)) continue;

        auto* h = reinterpret_cast<MicPacketHeader*>(pkt.data());
        if (h->magic != MSG_MIC_DATA || h->dataLength > pkt.size() - sizeof(MicPacketHeader)) continue;

        packetsReceived++;

        int ds = opus_decode(dec, pkt.data() + sizeof(MicPacketHeader), h->dataLength, decBuf.data(), FRAME_SZ, 0);
        if (ds <= 0) {
            if (++decodeErrors % 100 == 1)
                WARN("MicPlayback: Opus decode error: %s (total: %llu)", opus_strerror(ds), decodeErrors.load());
            continue;
        }

        packetsDecoded++;
        for (int i = 0; i < ds; i++) decF[i] = decBuf[i] / 32768.0f;
        resampler->ProcessMono(decF.data(), ds, ch);

        for (int attempts = 0; !resampler->buf.empty() && running.load(std::memory_order_acquire) && attempts < 50; attempts++) {
            UINT32 bufFr = 0, pad = 0;
            if (FAILED(cli->GetBufferSize(&bufFr)) || FAILED(cli->GetCurrentPadding(&pad))) break;

            UINT32 avail = bufFr - pad;
            if (avail == 0) { std::this_thread::sleep_for(1ms); continue; }

            UINT32 toW = std::min(avail, static_cast<UINT32>(resampler->buf.size() / ch));
            if (toW == 0) break;

            BYTE* buf = nullptr;
            if (FAILED(rnd->GetBuffer(toW, &buf))) break;

            float* out = reinterpret_cast<float*>(buf);
            for (UINT32 i = 0; i < toW * ch; i++) out[i] = resampler->buf[i];

            if (FAILED(rnd->ReleaseBuffer(toW, 0))) break;
            resampler->buf.erase(resampler->buf.begin(), resampler->buf.begin() + toW * ch);
            samplesWritten += toW;
        }

        if (resampler->buf.size() > FRAME_SZ * ch * 10) {
            size_t toDrop = resampler->buf.size() - FRAME_SZ * ch * 4;
            resampler->buf.erase(resampler->buf.begin(), resampler->buf.begin() + toDrop);
            bufferOverruns++;
            DBG("MicPlayback: Buffer overrun, dropped %zu samples", toDrop / ch);
        }
    }
    CoUninitialize();
    DBG("MicPlayback: Loop thread exiting");
}

MicPlayback::MicPlayback(const std::string& dn) : targetDev(dn) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
        throw std::runtime_error("COM initialization failed");

    auto chk = [](HRESULT hr, const char* m) {
        if (FAILED(hr)) { ERR("MicPlayback: %s failed: 0x%08X", m, hr); throw std::runtime_error(m); }
    };

    chk(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                         __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enm)), "MMDeviceEnumerator");

    dev = FindDevice(targetDev);
    if (!dev) {
        LOG("MicPlayback: '%s' not found, using default", targetDev.c_str());
        chk(enm->GetDefaultAudioEndpoint(eRender, eConsole, &dev), "GetDefaultAudioEndpoint");
        actualDevName = "(default output)";
    }

    chk(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&cli)), "IAudioClient");
    chk(cli->GetMixFormat(&wf), "GetMixFormat");

    devRate = wf->nSamplesPerSec;
    ch = wf->nChannels;
    DBG("MicPlayback: Device format: %dHz, %dch", devRate, ch);

    hr = cli->Initialize(AUDCLNT_SHAREMODE_SHARED,
                         AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                         300000, 0, wf, nullptr);
    if (FAILED(hr)) {
        hr = cli->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 300000, 0, wf, nullptr);
        if (FAILED(hr)) throw std::runtime_error("AudioClient Initialize failed");
        WARN("MicPlayback: Initialized without auto-conversion");
    }

    chk(cli->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&rnd)), "IAudioRenderClient");

    int err;
    dec = opus_decoder_create(RATE, 1, &err);
    if (err != OPUS_OK) throw std::runtime_error("Opus decoder creation failed");

    decBuf.resize(FRAME_SZ);
    resampler = std::make_unique<LinearResampler<float>>(RATE, devRate, 1);
    init.store(true, std::memory_order_release);
    LOG("MicPlayback: %dHz -> %dHz, %dch, device: %s", RATE, devRate, ch, actualDevName.c_str());
    CoUninitialize();
}

MicPlayback::~MicPlayback() {
    Stop();
    init.store(false, std::memory_order_release);
    LOG("MicPlayback: Stats - recv:%llu dec:%llu err:%llu written:%llu overruns:%llu",
        packetsReceived.load(), packetsDecoded.load(), decodeErrors.load(),
        samplesWritten.load(), bufferOverruns.load());
    if (dec) opus_decoder_destroy(dec);
    if (wf) CoTaskMemFree(wf);
    SafeRelease(rnd, cli, dev, enm);
}

void MicPlayback::Start() {
    if (running.load(std::memory_order_acquire) || !init.load(std::memory_order_acquire)) return;
    running.store(true, std::memory_order_release);
    resampler->Reset();
    if (FAILED(cli->Start())) {
        ERR("MicPlayback: IAudioClient::Start failed");
        running.store(false, std::memory_order_release);
        return;
    }
    thr = std::thread(&MicPlayback::Loop, this);
    LOG("MicPlayback: Started");
}

void MicPlayback::Stop() {
    if (!running.load(std::memory_order_acquire)) return;
    running.store(false, std::memory_order_release);
    streaming.store(false, std::memory_order_release);
    qCv.notify_all();
    if (thr.joinable()) thr.join();
    if (cli) cli->Stop();
    ClearQueue(pktQ, qMtx);
    LOG("MicPlayback: Stopped");
}

void MicPlayback::SetStreaming(bool s) {
    bool was = streaming.exchange(s, std::memory_order_acq_rel);
    if (s && !was) {
        ClearQueue(pktQ, qMtx);
        resampler->Reset();
        DBG("MicPlayback: Streaming enabled");
    } else if (!s && was) {
        DBG("MicPlayback: Streaming disabled");
    }
}

void MicPlayback::PushPacket(const uint8_t* data, size_t len) {
    if (!streaming.load(std::memory_order_acquire) || len < sizeof(MicPacketHeader)) return;
    std::lock_guard<std::mutex> lk(qMtx);
    if (pktQ.size() >= MAX_Q) pktQ.pop();
    pktQ.emplace(data, data + len);
    qCv.notify_one();
}
