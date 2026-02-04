#pragma once
#include "common.hpp"
#include <mmdeviceapi.h>
#include <audioclient.h>
extern "C" {
    #include <opus/opus.h>
}

struct AudioPacket { std::vector<uint8_t> data; int64_t ts = 0; int samples = 0; };

class AudioCapture {
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* audioClient = nullptr;
    IAudioCaptureClient* captureClient = nullptr;
    OpusEncoder* opusEncoder = nullptr;
    WAVEFORMATEX* waveFormat = nullptr;

    static constexpr int OPUS_RATE = 48000, OPUS_FRAME_MS = 10, FRAME_SAMPLES = OPUS_RATE * OPUS_FRAME_MS / 1000;
    static constexpr size_t MAX_QUEUE_SIZE = 8;

    int sysRate = 48000, channels = 2;
    double ratio = 1.0, accum = 0.0;
    std::vector<float> resBuf, prev;
    std::vector<int16_t> encBuf;
    std::vector<uint8_t> outBuf;

    std::atomic<bool> running{false}, capturing{false};
    std::atomic<bool> initialized{false};
    std::thread thread;
    std::queue<AudioPacket> queue;
    std::mutex qMtx;
    std::condition_variable qCond;

    void Loop() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        while (running.load(std::memory_order_acquire)) {
            if (!capturing.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(5ms);
                continue;
            }

            if (!captureClient || !initialized.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(10ms);
                continue;
            }

            UINT32 pktLen = 0;
            HRESULT hr = captureClient->GetNextPacketSize(&pktLen);
            if (FAILED(hr)) {
                ERR("[AudioCapture] GetNextPacketSize failed: 0x%08X", hr);
                std::this_thread::sleep_for(50ms);
                continue;
            }

            while (pktLen > 0 && running.load(std::memory_order_acquire) &&
                   capturing.load(std::memory_order_acquire) && captureClient) {
                BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
                hr = captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(hr)) {
                    ERR("[AudioCapture] GetBuffer failed: 0x%08X", hr);
                    break;
                }

                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data && frames > 0) {
                    Process(data, frames, GetTimestamp());
                }
                captureClient->ReleaseBuffer(frames);

                if (!captureClient) break;
                hr = captureClient->GetNextPacketSize(&pktLen);
                if (FAILED(hr)) break;
            }

            std::this_thread::sleep_for(2ms);
        }

        CoUninitialize();
    }

    void Resample(const float* in, size_t frames) {
        if (!frames) return;
        if (sysRate == OPUS_RATE) {
            resBuf.insert(resBuf.end(), in, in + frames * channels);
            for (int c = 0; c < channels; c++) prev[c] = in[(frames - 1) * channels + c];
            return;
        }
        while (accum < frames) {
            size_t i0 = (size_t)accum, i1 = i0 + 1;
            double f = accum - i0;
            for (int c = 0; c < channels; c++) {
                float s0 = (i0 == 0 && accum < 1.0) ? prev[c] : in[i0 * channels + c];
                float s1 = (i1 < frames) ? in[i1 * channels + c] : s0;
                resBuf.push_back((float)(s0 + (s1 - s0) * f));
            }
            accum += ratio;
        }
        accum -= frames;
        for (int c = 0; c < channels; c++) prev[c] = in[(frames - 1) * channels + c];
    }

    void Process(BYTE* data, UINT32 frames, int64_t ts) {
        if (!opusEncoder) return;

        Resample((float*)data, frames);

        while (resBuf.size() >= (size_t)(FRAME_SAMPLES * channels)) {
            for (int i = 0; i < FRAME_SAMPLES * channels; i++)
                encBuf[i] = (int16_t)(std::clamp(resBuf[i], -1.0f, 1.0f) * 32767.0f);
            resBuf.erase(resBuf.begin(), resBuf.begin() + FRAME_SAMPLES * channels);

            int bytes = opus_encode(opusEncoder, encBuf.data(), FRAME_SAMPLES, outBuf.data(), (opus_int32)outBuf.size());

            if (bytes > 0) {
                std::lock_guard<std::mutex> lk(qMtx);
                if (queue.size() >= MAX_QUEUE_SIZE) {
                    queue.pop();
                }
                queue.push({{outBuf.begin(), outBuf.begin() + bytes}, ts, FRAME_SAMPLES});
                qCond.notify_one();
            } else if (bytes < 0) {
                ERR("[AudioCapture] Opus encode error: %d (%s)", bytes, opus_strerror(bytes));
            }
        }
    }

public:
    AudioCapture() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto chk = [](HRESULT hr, const char* m) { if (FAILED(hr)) throw std::runtime_error(m); };
        chk(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator), "Enum");
        chk(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device), "Endpoint");
        chk(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient), "Client");
        chk(audioClient->GetMixFormat(&waveFormat), "Format");
        sysRate = waveFormat->nSamplesPerSec;
        channels = std::min((int)waveFormat->nChannels, 2);
        ratio = (double)sysRate / OPUS_RATE;

        REFERENCE_TIME bufferDuration = 30000;
        chk(audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, bufferDuration, 0, waveFormat, nullptr), "Init");
        chk(audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&captureClient), "Capture");

        int err; opusEncoder = opus_encoder_create(OPUS_RATE, channels, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
        if (err != OPUS_OK) throw std::runtime_error("Opus");

        opus_encoder_ctl(opusEncoder, OPUS_SET_BITRATE(96000));
        opus_encoder_ctl(opusEncoder, OPUS_SET_COMPLEXITY(3));
        opus_encoder_ctl(opusEncoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
        opus_encoder_ctl(opusEncoder, OPUS_SET_PACKET_LOSS_PERC(0));
        opus_encoder_ctl(opusEncoder, OPUS_SET_INBAND_FEC(0));
        opus_encoder_ctl(opusEncoder, OPUS_SET_DTX(0));

        encBuf.resize(FRAME_SAMPLES * channels);
        outBuf.resize(4000);
        resBuf.reserve(FRAME_SAMPLES * channels * 8);
        prev.resize(channels, 0.0f);
        initialized.store(true, std::memory_order_release);

        LOG("Audio: %dHz -> %dHz, %dch, ratio=%.4f, frameSize=%d", sysRate, OPUS_RATE, channels, ratio, FRAME_SAMPLES);
        CoUninitialize();
    }

    ~AudioCapture() {
        Stop();
        initialized.store(false, std::memory_order_release);
        if (opusEncoder) opus_encoder_destroy(opusEncoder);
        if (waveFormat) CoTaskMemFree(waveFormat);
        SafeRelease(captureClient, audioClient, device, enumerator);
    }

    void Start() {
        if (running.load(std::memory_order_acquire)) return;
        if (!initialized.load(std::memory_order_acquire)) return;

        running.store(true, std::memory_order_release);
        capturing.store(true, std::memory_order_release);
        accum = 0.0; resBuf.clear();
        std::fill(prev.begin(), prev.end(), 0.0f);

        HRESULT hr = audioClient->Start();
        if (FAILED(hr)) {
            ERR("[AudioCapture] audioClient->Start() failed: 0x%08X", hr);
            running.store(false, std::memory_order_release);
            capturing.store(false, std::memory_order_release);
            return;
        }
        thread = std::thread(&AudioCapture::Loop, this);
    }

    void Stop() {
        if (!running.load(std::memory_order_acquire)) return;

        running.store(false, std::memory_order_release);
        capturing.store(false, std::memory_order_release);
        qCond.notify_all();

        if (thread.joinable()) thread.join();
        if (audioClient) audioClient->Stop();
    }

    bool PopPacket(AudioPacket& out, int ms = 5) {
        std::unique_lock<std::mutex> lk(qMtx);
        if (!qCond.wait_for(lk, std::chrono::milliseconds(ms), [this] {
            return !queue.empty() || !running.load(std::memory_order_acquire);
        })) return false;
        if (queue.empty()) return false;
        out = std::move(queue.front()); queue.pop();
        return true;
    }
};
