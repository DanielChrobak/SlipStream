#pragma once
#include "common.hpp"
#include <mmdeviceapi.h>
#include <audioclient.h>
extern "C" {
    #include <opus/opus.h>
}

struct AudioPacket { std::vector<uint8_t> data; int64_t ts=0; int samples=0; };

class AudioCapture {
    IMMDeviceEnumerator* enm=nullptr; IMMDevice* dev=nullptr;
    IAudioClient* cli=nullptr; IAudioCaptureClient* cap=nullptr;
    OpusEncoder* enc=nullptr; WAVEFORMATEX* wf=nullptr;

    static constexpr int RATE=48000, FRAME_MS=10, FRAME_SZ=RATE*FRAME_MS/1000;
    static constexpr size_t MAX_Q=4;

    int sysRate=48000, ch=2;
    std::unique_ptr<LinearResampler<float>> resampler;
    std::vector<int16_t> encBuf;
    std::vector<uint8_t> outBuf;
    std::vector<float> silBuf;

    std::atomic<bool> running{false}, capturing{false}, init{false}, streaming{false};
    std::thread thr;
    std::queue<AudioPacket> q;
    std::mutex qMtx;
    std::condition_variable qCv;
    std::atomic<int> consecutiveErrors{0};
    static constexpr int MAX_CONSECUTIVE_ERRORS=10;

    void Loop();
    void Process(const float* data, UINT32 frames, int64_t ts);

public:
    AudioCapture();
    ~AudioCapture();

    void Start();
    void Stop();
    void SetStreaming(bool s);
    bool PopPacket(AudioPacket& out, int ms=5);
};
