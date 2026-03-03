#pragma once

#include <speex/speex_resampler.h>

#include <stdexcept>
#include <vector>

class SpeexResampler {
    SpeexResamplerState* st = nullptr;
    int srcRate_, dstRate_, channels_;
public:
    std::vector<float> buf;

    SpeexResampler(int src, int dst, int ch, int quality = 4)
        : srcRate_(src), dstRate_(dst), channels_(ch) {
        int err;
        st = speex_resampler_init(ch, src, dst, quality, &err);
        if (!st) throw std::runtime_error("Speex resampler init failed");
        buf.reserve(480 * ch * 8);
    }

    ~SpeexResampler() { if (st) speex_resampler_destroy(st); }
    SpeexResampler(const SpeexResampler&) = delete;
    SpeexResampler& operator=(const SpeexResampler&) = delete;

    void Reset() {
        if (st) speex_resampler_reset_mem(st);
        buf.clear();
    }

    void Process(const float* in, size_t frames) {
        if (!frames || !st) return;
        spx_uint32_t inLen = static_cast<spx_uint32_t>(frames);
        spx_uint32_t outLen = static_cast<spx_uint32_t>(frames * dstRate_ / srcRate_ + 64);
        size_t pos = buf.size();
        buf.resize(pos + outLen * channels_);
        speex_resampler_process_interleaved_float(st, in, &inLen, buf.data() + pos, &outLen);
        buf.resize(pos + outLen * channels_);
    }

    void ProcessMono(const float* in, size_t frames, int outCh) {
        if (!frames || !st) return;
        spx_uint32_t inLen = static_cast<spx_uint32_t>(frames);
        spx_uint32_t outLen = static_cast<spx_uint32_t>(frames * dstRate_ / srcRate_ + 64);
        size_t oldSz = buf.size();
        buf.resize(oldSz + outLen);
        speex_resampler_process_float(st, 0, in, &inLen, buf.data() + oldSz, &outLen);
        buf.resize(oldSz + outLen);
        if (outCh > 1) {
            size_t monoCount = outLen;
            buf.resize(oldSz + monoCount * outCh);
            for (size_t i = monoCount; i-- > 0;)
                for (int c = outCh - 1; c >= 0; c--)
                    buf[oldSz + i * outCh + c] = buf[oldSz + i];
        }
    }
};
