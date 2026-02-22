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

    static constexpr int RATE=48000, FRAME_SZ=RATE/100;
    static constexpr size_t MAX_Q=20;

    int devRate=48000, ch=1;
    std::unique_ptr<LinearResampler<float>> resampler;
    std::vector<int16_t> decBuf;
    std::atomic<bool> running{false}, streaming{false}, init{false};
    std::thread thr;
    std::queue<std::vector<uint8_t>> pktQ;
    std::mutex qMtx;
    std::condition_variable qCv;
    std::string targetDev, actualDevName;
    std::atomic<uint64_t> packetsReceived{0}, packetsDecoded{0}, decodeErrors{0};
    std::atomic<uint64_t> samplesWritten{0}, bufferOverruns{0};

    IMMDevice* FindDevice(const std::string& name);
    void Loop();

public:
    explicit MicPlayback(const std::string& deviceName="CABLE Input");
    ~MicPlayback();

    void Start();
    void Stop();
    void SetStreaming(bool s);
    void PushPacket(const uint8_t* data, size_t len);
    [[nodiscard]] bool IsInitialized() const { return init.load(std::memory_order_acquire); }
    [[nodiscard]] const std::string& GetDeviceName() const { return actualDevName; }
};
