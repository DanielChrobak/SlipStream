#pragma once

#include "common.hpp"
#include <mmdeviceapi.h>
#include <audioclient.h>

extern "C" {
#include <opus/opus.h>
}

// ============================================================================
// Audio Packet Structure
// ============================================================================

struct AudioPacket {
    std::vector<uint8_t> data;
    int64_t ts = 0;
    int samples = 0;
};

// ============================================================================
// Audio Capture Class
// ============================================================================

class AudioCapture {
private:
    // COM interfaces
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* audioClient = nullptr;
    IAudioCaptureClient* captureClient = nullptr;

    // Opus encoder
    OpusEncoder* opusEncoder = nullptr;

    // Audio format
    WAVEFORMATEX* waveFormat = nullptr;
    int sampleRate = 48000;
    int channels = 2;
    int frameDurationMs = 20;
    int frameSamples = 0;

    // Threading
    std::atomic<bool> running{false};
    std::atomic<bool> capturing{false};
    std::thread captureThread;

    // Packet queue
    std::queue<AudioPacket> packetQueue;
    std::mutex queueMutex;
    std::condition_variable queueCondition;

    // Buffers
    std::vector<float> resampleBuffer;
    std::vector<int16_t> encodeBuffer;
    std::vector<uint8_t> outputBuffer;

    // ========================================================================
    // Capture Loop
    // ========================================================================

    void CaptureLoop() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        while (running) {
            if (!capturing) {
                std::this_thread::sleep_for(10ms);
                continue;
            }

            UINT32 packetLength = 0;
            if (FAILED(captureClient->GetNextPacketSize(&packetLength))) {
                std::this_thread::sleep_for(100ms);
                continue;
            }

            while (packetLength > 0 && running && capturing) {
                BYTE* data = nullptr;
                UINT32 numFrames = 0;
                DWORD flags = 0;

                if (FAILED(captureClient->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr))) {
                    break;
                }

                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data && numFrames > 0) {
                    ProcessAudio(data, numFrames, GetTimestamp());
                }

                captureClient->ReleaseBuffer(numFrames);

                if (FAILED(captureClient->GetNextPacketSize(&packetLength))) {
                    break;
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(frameDurationMs / 2));
        }

        CoUninitialize();
    }

    // ========================================================================
    // Audio Processing
    // ========================================================================

    void ProcessAudio(BYTE* data, UINT32 numFrames, int64_t timestamp) {
        auto* floatData = reinterpret_cast<float*>(data);
        resampleBuffer.insert(resampleBuffer.end(), floatData, floatData + numFrames * channels);

        size_t consumed = 0;
        while (resampleBuffer.size() - consumed >= static_cast<size_t>(frameSamples * channels)) {
            const float* src = resampleBuffer.data() + consumed;

            // Convert float samples to int16
            for (int i = 0; i < frameSamples * channels; i++) {
                encodeBuffer[i] = static_cast<int16_t>(
                    std::clamp(src[i], -1.0f, 1.0f) * 32767.0f
                );
            }
            consumed += frameSamples * channels;

            // Encode with Opus
            int encodedBytes = opus_encode(
                opusEncoder,
                encodeBuffer.data(),
                frameSamples,
                outputBuffer.data(),
                static_cast<opus_int32>(outputBuffer.size())
            );

            if (encodedBytes > 0) {
                std::lock_guard<std::mutex> lock(queueMutex);
                if (packetQueue.size() < 50) {
                    packetQueue.push({
                        std::vector<uint8_t>(outputBuffer.begin(), outputBuffer.begin() + encodedBytes),
                        timestamp,
                        frameSamples
                    });
                }
                queueCondition.notify_one();
            }
        }

        if (consumed > 0) {
            resampleBuffer.erase(resampleBuffer.begin(), resampleBuffer.begin() + consumed);
        }
    }

public:
    // ========================================================================
    // Constructor
    // ========================================================================

    AudioCapture() {
        LOG("Initializing audio capture...");
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        auto check = [](HRESULT hr, const char* msg) {
            if (FAILED(hr)) {
                throw std::runtime_error(msg);
            }
        };

        // Create device enumerator
        check(
            CoCreateInstance(
                __uuidof(MMDeviceEnumerator),
                nullptr,
                CLSCTX_ALL,
                __uuidof(IMMDeviceEnumerator),
                reinterpret_cast<void**>(&enumerator)
            ),
            "Failed to create device enumerator"
        );

        // Get default audio endpoint
        check(
            enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device),
            "Failed to get default audio endpoint"
        );

        // Activate audio client
        check(
            device->Activate(
                __uuidof(IAudioClient),
                CLSCTX_ALL,
                nullptr,
                reinterpret_cast<void**>(&audioClient)
            ),
            "Failed to activate audio client"
        );

        // Get mix format
        check(audioClient->GetMixFormat(&waveFormat), "Failed to get mix format");

        sampleRate = waveFormat->nSamplesPerSec;
        channels = std::min(static_cast<int>(waveFormat->nChannels), 2);
        frameSamples = sampleRate * frameDurationMs / 1000;

        // Initialize audio client in loopback mode
        check(
            audioClient->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_LOOPBACK,
                200000,
                0,
                waveFormat,
                nullptr
            ),
            "Failed to initialize audio client"
        );

        // Get capture client
        check(
            audioClient->GetService(
                __uuidof(IAudioCaptureClient),
                reinterpret_cast<void**>(&captureClient)
            ),
            "Failed to get capture client"
        );

        // Create Opus encoder
        int opusError;
        opusEncoder = opus_encoder_create(48000, channels, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &opusError);
        if (opusError != OPUS_OK) {
            throw std::runtime_error("Failed to create Opus encoder");
        }

        // Configure Opus encoder
        opus_encoder_ctl(opusEncoder, OPUS_SET_BITRATE(128000));
        opus_encoder_ctl(opusEncoder, OPUS_SET_COMPLEXITY(5));

        // Allocate buffers
        encodeBuffer.resize(frameSamples * channels);
        outputBuffer.resize(4000);
        resampleBuffer.reserve(frameSamples * channels * 8);

        LOG("Audio initialized (%dHz, %d ch)", sampleRate, channels);
        CoUninitialize();
    }

    // ========================================================================
    // Destructor
    // ========================================================================

    ~AudioCapture() {
        Stop();

        if (opusEncoder) {
            opus_encoder_destroy(opusEncoder);
        }

        if (waveFormat) {
            CoTaskMemFree(waveFormat);
        }

        SafeRelease(captureClient, audioClient, device, enumerator);
    }

    // ========================================================================
    // Control Methods
    // ========================================================================

    void Start() {
        if (running) {
            return;
        }

        running = capturing = true;

        if (FAILED(audioClient->Start())) {
            running = capturing = false;
            return;
        }

        captureThread = std::thread(&AudioCapture::CaptureLoop, this);
        LOG("Audio capture started");
    }

    void Stop() {
        if (!running) {
            return;
        }

        running = capturing = false;
        queueCondition.notify_all();

        if (captureThread.joinable()) {
            captureThread.join();
        }

        if (audioClient) {
            audioClient->Stop();
        }
    }

    // ========================================================================
    // Packet Retrieval
    // ========================================================================

    bool PopPacket(AudioPacket& out, int timeoutMs = 10) {
        std::unique_lock<std::mutex> lock(queueMutex);

        if (!queueCondition.wait_for(
            lock,
            std::chrono::milliseconds(timeoutMs),
            [this] { return !packetQueue.empty() || !running; }
        )) {
            return false;
        }

        if (packetQueue.empty()) {
            return false;
        }

        out = std::move(packetQueue.front());
        packetQueue.pop();
        return true;
    }

    // ========================================================================
    // Getters
    // ========================================================================

    int GetSampleRate() const { return 48000; }
    int GetChannels() const { return channels; }
};
