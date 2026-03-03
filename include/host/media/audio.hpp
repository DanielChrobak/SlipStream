#pragma once
#include "host/core/common.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
extern "C" {
#include <opus/opus.h>
}

// --- Audio capture (loopback → Opus encode → queue) ---

struct AudioPacket {
    std::vector<uint8_t> data;
    int64_t ts = 0;
    int samples = 0;
};

class AudioCapture {
    static constexpr int kSampleRate = 48000;
    static constexpr int kFrameSamples = kSampleRate / 100;
    static constexpr size_t kMaxQueueSize = 4;

    IMMDeviceEnumerator* deviceEnumerator = nullptr;
    IMMDevice* audioDevice = nullptr;
    IAudioClient* audioClient = nullptr;
    IAudioCaptureClient* captureClient = nullptr;
    OpusEncoder* opusEncoder = nullptr;
    WAVEFORMATEX* waveFormat = nullptr;

    int systemSampleRate = 48000;
    int channelCount = 2;
    std::unique_ptr<SpeexResampler> resampler;
    std::vector<int16_t> encodeBuffer;
    std::vector<uint8_t> opusBuffer;
    size_t resamplerReadPos = 0;

    std::atomic<bool> running{false};
    std::atomic<bool> captureActive{false};
    std::atomic<bool> isInitialized{false};
    std::atomic<bool> streaming{false};
    std::thread captureThread;
    std::queue<AudioPacket> packetQueue;
    std::mutex queueMutex;
    std::condition_variable queueCv;

    void Loop();
    void Process(const float* data, UINT32 frames, int64_t ts);

public:
    AudioCapture();
    ~AudioCapture();
    void Start();
    void Stop();
    void SetStreaming(bool s);
    [[nodiscard]] bool PopPacket(AudioPacket& out, int ms = 5);
};

// --- Mic playback (Opus decode → resample → WASAPI render) ---

class MicPlayback {
    IMMDeviceEnumerator* deviceEnumerator = nullptr;
    IMMDevice* audioDevice = nullptr;
    IAudioClient* audioClient = nullptr;
    IAudioRenderClient* renderClient = nullptr;
    OpusDecoder* opusDecoder = nullptr;
    WAVEFORMATEX* waveFormat = nullptr;

    static constexpr int kSampleRate = 48000;
    static constexpr int kFrameSamples = kSampleRate / 100;
    static constexpr size_t kMaxQueueSize = 20;

    int deviceSampleRate = 48000;
    int channelCount = 1;
    std::unique_ptr<SpeexResampler> resampler;
    std::vector<int16_t> decodeBuffer;
    size_t resamplerReadPos = 0;
    std::atomic<bool> running{false};
    std::atomic<bool> streaming{false};
    std::atomic<bool> init{false};
    std::thread playbackThread;
    std::queue<std::vector<uint8_t>> packetQueue;
    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::string targetDevice, actualDeviceName;
    std::atomic<uint64_t> packetsReceived{0}, packetsDecoded{0}, decodeErrors{0};
    std::atomic<uint64_t> samplesWritten{0}, bufferOverruns{0};

    IMMDevice* FindDevice(const std::string& name);
    void Loop();

public:
    explicit MicPlayback(const std::string& deviceName = "CABLE Input");
    ~MicPlayback();

    void Start();
    void Stop();
    void SetStreaming(bool s);
    void PushPacket(const uint8_t* data, size_t len);
    [[nodiscard]] bool IsInitialized() const { return init.load(std::memory_order_acquire); }
    [[nodiscard]] const std::string& GetDeviceName() const { return actualDeviceName; }
};
