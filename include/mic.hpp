#pragma once
#include "common.hpp"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
extern "C" {
    #include <opus/opus.h>
}

class MicPlayback {
    IMMDeviceEnumerator* enm=nullptr;
    IMMDevice* dev=nullptr;
    IAudioClient* cli=nullptr;
    IAudioRenderClient* rnd=nullptr;
    OpusDecoder* dec=nullptr;
    WAVEFORMATEX* wf=nullptr;

    static constexpr int RATE=48000, FRAME_MS=10, FRAME_SZ=RATE*FRAME_MS/1000;
    static constexpr size_t MAX_Q=20;

    int devRate=48000, ch=1;
    std::unique_ptr<LinearResampler<float>> resampler;
    std::vector<int16_t> decBuf;

    std::atomic<bool> running{false}, streaming{false}, init{false};
    std::thread thr;
    std::queue<std::vector<uint8_t>> pktQ;
    std::mutex qMtx;
    std::condition_variable qCv;
    std::string targetDev;
    std::string actualDevName;

    std::atomic<uint64_t> packetsReceived{0}, packetsDecoded{0}, decodeErrors{0};
    std::atomic<uint64_t> samplesWritten{0}, bufferOverruns{0};

    IMMDevice* FindDev(const std::string& name) {
        IMMDeviceCollection* col=nullptr;
        HRESULT hr = enm->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col);
        if(FAILED(hr)) {
            WARN("MicPlayback: EnumAudioEndpoints failed: 0x%08X", hr);
            return nullptr;
        }

        UINT cnt=0;
        col->GetCount(&cnt);
        DBG("MicPlayback: Found %u render devices", cnt);

        for(UINT i=0; i<cnt; i++) {
            IMMDevice* d=nullptr;
            if(FAILED(col->Item(i, &d))) continue;
            IPropertyStore* ps=nullptr;

            if(SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
                PROPVARIANT var;
                PropVariantInit(&var);
                if(SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &var))) {
                    char dn[256]={};
                    WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, dn, sizeof(dn), nullptr, nullptr);
                    PropVariantClear(&var);
                    ps->Release();

                    std::string dl=dn, tl=name;
                    std::transform(dl.begin(), dl.end(), dl.begin(), ::tolower);
                    std::transform(tl.begin(), tl.end(), tl.begin(), ::tolower);
                    DBG("MicPlayback: Device %u: %s", i, dn);

                    if(dl.find(tl) != std::string::npos) {
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
        WARN("MicPlayback: Device containing '%s' not found among %u devices", name.c_str(), cnt);
        return nullptr;
    }

    void Loop() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if(FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
            ERR("MicPlayback: CoInitializeEx failed: 0x%08X", hr);
            return;
        }

        std::vector<float> decF(FRAME_SZ);

        while(running.load(std::memory_order_acquire)) {
            if(!streaming.load(std::memory_order_acquire) || !rnd ||
               !init.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(10ms);
                continue;
            }

            std::vector<uint8_t> pkt;
            {
                std::unique_lock<std::mutex> lk(qMtx);
                if(!qCv.wait_for(lk, 5ms, [this] {
                    return !pktQ.empty() || !running.load(std::memory_order_acquire);
                })) continue;
                if(!running.load(std::memory_order_acquire)) break;
                if(pktQ.empty()) continue;
                pkt = std::move(pktQ.front());
                pktQ.pop();
            }

            if(pkt.size() < sizeof(MicPacketHeader)) {
                WARN("MicPlayback: Packet too small (%zu bytes)", pkt.size());
                continue;
            }

            auto* h = (MicPacketHeader*)pkt.data();
            if(h->magic != MSG_MIC_DATA) {
                DBG("MicPlayback: Wrong magic 0x%08X", h->magic);
                continue;
            }
            if(h->dataLength > pkt.size() - sizeof(MicPacketHeader)) {
                WARN("MicPlayback: Invalid dataLength %u (packet size %zu)", h->dataLength, pkt.size());
                continue;
            }

            packetsReceived++;

            int ds = opus_decode(dec, pkt.data() + sizeof(MicPacketHeader), h->dataLength,
                                 decBuf.data(), FRAME_SZ, 0);
            if(ds <= 0) {
                decodeErrors++;
                if(decodeErrors.load() % 100 == 1) {
                    WARN("MicPlayback: Opus decode error: %s (total errors: %llu)",
                         opus_strerror(ds), decodeErrors.load());
                }
                continue;
            }

            packetsDecoded++;
            for(int i=0; i<ds; i++) decF[i] = decBuf[i] / 32768.0f;
            resampler->ProcessMono(decF.data(), ds, ch);

            int writeAttempts = 0;
            while(!resampler->buf.empty() && running.load(std::memory_order_acquire) && writeAttempts < 50) {
                writeAttempts++;
                UINT32 bufFr=0, pad=0;

                hr = cli->GetBufferSize(&bufFr);
                if(FAILED(hr)) { WARN("MicPlayback: GetBufferSize failed: 0x%08X", hr); break; }

                hr = cli->GetCurrentPadding(&pad);
                if(FAILED(hr)) { WARN("MicPlayback: GetCurrentPadding failed: 0x%08X", hr); break; }

                UINT32 avail = bufFr - pad;
                if(avail == 0) { std::this_thread::sleep_for(1ms); continue; }

                UINT32 toW = std::min(avail, (UINT32)(resampler->buf.size() / ch));
                if(toW == 0) break;

                BYTE* buf=nullptr;
                hr = rnd->GetBuffer(toW, &buf);
                if(FAILED(hr)) { WARN("MicPlayback: GetBuffer failed: 0x%08X", hr); break; }

                float* out = (float*)buf;
                for(UINT32 i=0; i<toW*ch; i++) out[i] = resampler->buf[i];

                hr = rnd->ReleaseBuffer(toW, 0);
                if(FAILED(hr)) { WARN("MicPlayback: ReleaseBuffer failed: 0x%08X", hr); break; }

                resampler->buf.erase(resampler->buf.begin(), resampler->buf.begin() + toW*ch);
                samplesWritten += toW;
            }

            if(resampler->buf.size() > FRAME_SZ * ch * 10) {
                size_t toDrop = resampler->buf.size() - FRAME_SZ*ch*4;
                resampler->buf.erase(resampler->buf.begin(), resampler->buf.begin() + toDrop);
                bufferOverruns++;
                DBG("MicPlayback: Buffer overrun, dropped %zu samples (total overruns: %llu)",
                    toDrop/ch, bufferOverruns.load());
            }
        }
        CoUninitialize();
        DBG("MicPlayback: Loop thread exiting");
    }

public:
    MicPlayback(const std::string& dn="CABLE Input") : targetDev(dn) {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if(FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
            ERR("MicPlayback: CoInitializeEx failed: 0x%08X", hr);
            throw std::runtime_error("COM initialization failed");
        }

        auto chk = [](HRESULT hr, const char* m) {
            if(FAILED(hr)) { ERR("MicPlayback: %s failed: 0x%08X", m, hr); throw std::runtime_error(m); }
        };

        chk(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                            __uuidof(IMMDeviceEnumerator), (void**)&enm), "MMDeviceEnumerator");

        dev = FindDev(targetDev);
        if(!dev) {
            LOG("MicPlayback: '%s' not found, using default output device", targetDev.c_str());
            hr = enm->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
            if(FAILED(hr)) {
                ERR("MicPlayback: GetDefaultAudioEndpoint failed: 0x%08X", hr);
                throw std::runtime_error("No audio output device available");
            }
            actualDevName = "(default output)";
        }

        chk(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&cli),
            "IAudioClient activation");
        chk(cli->GetMixFormat(&wf), "GetMixFormat");

        devRate = wf->nSamplesPerSec;
        ch = wf->nChannels;
        DBG("MicPlayback: Device format: %d Hz, %d channels, %d bits, format tag: 0x%04X",
            devRate, ch, wf->wBitsPerSample, wf->wFormatTag);

        hr = cli->Initialize(AUDCLNT_SHAREMODE_SHARED,
                             AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                             300000, 0, wf, nullptr);
        if(FAILED(hr)) {
            ERR("MicPlayback: Initialize failed: 0x%08X", hr);
            hr = cli->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 300000, 0, wf, nullptr);
            if(FAILED(hr)) { throw std::runtime_error("AudioClient Initialize failed"); }
            WARN("MicPlayback: Initialized without auto-conversion");
        }

        chk(cli->GetService(__uuidof(IAudioRenderClient), (void**)&rnd), "IAudioRenderClient");

        int err;
        dec = opus_decoder_create(RATE, 1, &err);
        if(err != OPUS_OK) {
            ERR("MicPlayback: Opus decoder creation failed: %s", opus_strerror(err));
            throw std::runtime_error("Opus decoder creation failed");
        }

        decBuf.resize(FRAME_SZ);
        resampler = std::make_unique<LinearResampler<float>>(RATE, devRate, 1);

        init.store(true, std::memory_order_release);
        LOG("MicPlayback: %dHz -> %dHz, %dch, device: %s", RATE, devRate, ch, actualDevName.c_str());
        CoUninitialize();
    }

    ~MicPlayback() {
        Stop();
        init.store(false, std::memory_order_release);
        LOG("MicPlayback: Stats - received:%llu decoded:%llu errors:%llu written:%llu overruns:%llu",
            packetsReceived.load(), packetsDecoded.load(), decodeErrors.load(),
            samplesWritten.load(), bufferOverruns.load());
        if(dec) opus_decoder_destroy(dec);
        if(wf) CoTaskMemFree(wf);
        SafeRelease(rnd, cli, dev, enm);
        DBG("MicPlayback: Destroyed");
    }

    void Start() {
        if(running.load(std::memory_order_acquire) || !init.load(std::memory_order_acquire)) {
            DBG("MicPlayback: Start called but already running or not initialized");
            return;
        }
        running.store(true, std::memory_order_release);
        resampler->Reset();
        HRESULT hr = cli->Start();
        if(FAILED(hr)) {
            ERR("MicPlayback: IAudioClient::Start failed: 0x%08X", hr);
            running.store(false, std::memory_order_release);
            return;
        }
        thr = std::thread(&MicPlayback::Loop, this);
        LOG("MicPlayback: Started");
    }

    void Stop() {
        if(!running.load(std::memory_order_acquire)) return;
        running.store(false, std::memory_order_release);
        streaming.store(false, std::memory_order_release);
        qCv.notify_all();
        if(thr.joinable()) thr.join();
        if(cli) {
            HRESULT hr = cli->Stop();
            if(FAILED(hr)) { WARN("MicPlayback: IAudioClient::Stop failed: 0x%08X", hr); }
        }
        ClearQueue(pktQ, qMtx);
        LOG("MicPlayback: Stopped");
    }

    void SetStreaming(bool s) {
        bool was = streaming.exchange(s, std::memory_order_acq_rel);
        if(s && !was) {
            ClearQueue(pktQ, qMtx);
            resampler->Reset();
            DBG("MicPlayback: Streaming enabled");
        } else if(!s && was) {
            DBG("MicPlayback: Streaming disabled");
        }
    }

    void PushPacket(const uint8_t* data, size_t len) {
        if(!streaming.load(std::memory_order_acquire)) return;
        if(len < sizeof(MicPacketHeader)) {
            WARN("MicPlayback: PushPacket called with invalid length %zu", len);
            return;
        }
        std::lock_guard<std::mutex> lk(qMtx);
        if(pktQ.size() >= MAX_Q) {
            DBG("MicPlayback: Queue full, dropping oldest packet");
            pktQ.pop();
        }
        pktQ.emplace(data, data + len);
        qCv.notify_one();
    }

    bool IsInitialized() const { return init.load(std::memory_order_acquire); }
    const std::string& GetDeviceName() const { return actualDevName; }
};
