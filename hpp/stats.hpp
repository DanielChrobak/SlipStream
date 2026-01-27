#pragma once

#include "common.hpp"
#include "capture.hpp"
#include "encoder.hpp"
#include "webrtc.hpp"
#include "input.hpp"
#include "audio.hpp"

class StatsLogger {
    std::atomic<bool>& running;
    std::atomic<bool>& quitting;
    std::shared_ptr<WebRTCServer> rtc;
    ScreenCapture& capture;
    std::unique_ptr<VideoEncoder>& encoder;
    std::mutex& encMtx;
    InputHandler& input;
    FrameSlot& frameSlot;
    EncoderThreadMetrics& encMetrics;
    std::string username;

    uint64_t uptime = 0, sesFrames = 0, sesBytes = 0, sesDrops = 0;

public:
    StatsLogger(std::atomic<bool>& run, std::atomic<bool>& quit, std::shared_ptr<WebRTCServer> r,
                ScreenCapture& cap, std::unique_ptr<VideoEncoder>& enc, std::mutex& emtx,
                InputHandler& inp, FrameSlot& fs, EncoderThreadMetrics& em, const std::string& user)
        : running(run), quitting(quit), rtc(r), capture(cap), encoder(enc), encMtx(emtx),
          input(inp), frameSlot(fs), encMetrics(em), username(user) {}

    void Run() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        while (running && !quitting) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            uptime++;
            LogStats();
        }
    }

private:
    void LogStats() {
        auto st = rtc->GetStats();
        uint64_t enc = 0, encFailed = 0;
        EncoderGPUMetrics::Snapshot encGpu = {};
        int encW = 0, encH = 0;
        CodecType encCodec = CODEC_H264;

        {
            std::lock_guard<std::mutex> lk(encMtx);
            if (encoder) {
                enc = encoder->GetEncoded();
                encFailed = encoder->GetFailed();
                encGpu = encoder->GetGPUMetrics();
                encW = encoder->GetWidth();
                encH = encoder->GetHeight();
                encCodec = encoder->GetCodec();
            }
        }

        CaptureMetrics::Snapshot cap = capture.GetMetrics();
        GPUWaitMetrics::Snapshot capGpu = capture.GetGPUWaitMetrics();
        EncoderThreadMetrics::Snapshot encTh = encMetrics.GetAndReset();
        PacedSendMetrics::Snapshot paced = rtc->GetPacedMetrics();
        size_t qDepth = rtc->GetSendQueueDepth();
        uint64_t dropped = frameSlot.GetDropped(), texConflicts = capture.GetTexConflicts();
        auto inputStats = input.GetStats();
        uint64_t audioSent = rtc->GetAudioSent();

        sesFrames += enc;
        sesBytes += st.bytes;
        sesDrops += dropped + st.dropped;

        int targetFps = capture.GetCurrentFPS();
        double fpsEff = targetFps > 0 ? (enc * 100.0 / targetFps) : 0;
        double mbps = st.bytes * 8.0 / 1048576.0;

        double capMs = cap.avgUs / 1000.0;
        double handMs = encTh.handoffCount > 0 ? encTh.handoffAvgUs / 1000.0 : 0;
        double encMs = encTh.encodeCount > 0 ? encTh.encodeAvgUs / 1000.0 : 0;
        double gpuMs = (encGpu.count > 0 ? encGpu.avgUs : 0) / 1000.0;
        double netMs = paced.avgFrameSendTimeUs / 1000.0;
        double totalMs = capMs + handMs + encMs + gpuMs + netMs;

        const char* status = st.connected ? (rtc->IsStreaming() ? "\033[32m[LIVE]\033[0m" : "\033[33m[WAIT]\033[0m") : "\033[33m[IDLE]\033[0m";
        const char* codecName = encCodec == CODEC_H264 ? "H.264" : "AV1";

        if (!st.connected) {
            printf("%s Waiting for connection... (%llus)\n", status, uptime);
            return;
        }

        printf("\n\033[1;36m━━━ [%llus] %s ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\033[0m\n", uptime, status);
        printf("\033[1;33m[THROUGHPUT]\033[0m FPS: %llu/%d (%.1f%%) | Bitrate: %.2f Mbps | V:%llu A:%llu | Res: %dx%d | Codec: %s\n",
               enc, targetFps, fpsEff, mbps, st.sent, audioSent, encW, encH, codecName);
        printf("\033[1;33m[PIPELINE]\033[0m Total: %.2fms | Cap:%.2f + Hand:%.2f + Enc:%.2f + GPU:%.2f + Net:%.2fms\n",
               totalMs, capMs, handMs, encMs, gpuMs, netMs);

        if (cap.captured > 0 || cap.frames > 0) {
            printf("\033[1;33m[CAPTURE]\033[0m Captured: %llu/%llu arrived | Interval: %.2f/%.2f/%.2fms (min/avg/max) | Miss:%llu Skip:%llu\n",
                   cap.captured, cap.frames, cap.minUs / 1000.0, cap.avgUs / 1000.0, cap.maxUs / 1000.0, cap.missed, cap.skipped);
            if (capGpu.count + capGpu.noWaitCount > 0 || capGpu.timeouts > 0)
                printf("         GPU Wait: %.2f/%.2f/%.2fms (min/avg/max) | n:%llu noWait:%llu timeout:%llu\n",
                       capGpu.minUs / 1000.0, capGpu.avgUs / 1000.0, capGpu.maxUs / 1000.0, capGpu.count, capGpu.noWaitCount, capGpu.timeouts);
        }

        if (encTh.handoffCount > 0 || encTh.encodeCount > 0) {
            printf("\033[1;33m[ENCODER]\033[0m Handoff: %.2f/%.2f/%.2fms (n:%llu) | Encode: %.2f/%.2f/%.2fms (n:%llu)\n",
                   encTh.handoffMinUs / 1000.0, encTh.handoffAvgUs / 1000.0, encTh.handoffMaxUs / 1000.0, encTh.handoffCount,
                   encTh.encodeMinUs / 1000.0, encTh.encodeAvgUs / 1000.0, encTh.encodeMaxUs / 1000.0, encTh.encodeCount);
            if (encGpu.count + encGpu.noWait > 0 || encGpu.timeouts > 0)
                printf("         GPU Sync: %.2f/%.2f/%.2fms (n:%llu noWait:%llu timeout:%llu)\n",
                       encGpu.minUs / 1000.0, encGpu.avgUs / 1000.0, encGpu.maxUs / 1000.0, encGpu.count, encGpu.noWait, encGpu.timeouts);
        }

        if (st.sent > 0 || paced.drainEvents > 0) {
            printf("\033[1;33m[NETWORK]\033[0m Queue: avg:%llu max:%llu depth:%zu | Burst: %llu/%llu avg/max | Drains:%llu\n",
                   paced.avgQueueDepth, paced.maxQueueDepth, qDepth, paced.avgBurst, paced.maxBurst, paced.drainEvents);
            if (paced.avgFrameSendTimeUs > 0 || paced.maxFrameSendTimeUs > 0)
                printf("         Send Time: %.2f/%.2fms (avg/max)\n", paced.avgFrameSendTimeUs / 1000.0, paced.maxFrameSendTimeUs / 1000.0);
        }

        uint64_t totalDrops = dropped + st.dropped + encTh.deadlineMisses + encTh.stateDrops + encFailed;
        if (totalDrops > 0 || texConflicts > 0) {
            printf("\033[1;31m[DROPS]\033[0m Slot:%llu Net:%llu Deadline:%llu State:%llu EncFail:%llu TexConflict:%llu\n",
                   dropped, st.dropped, encTh.deadlineMisses, encTh.stateDrops, encFailed, texConflicts);
            if (encTh.worstLatenessUs > 0)
                printf("         Worst deadline miss: %.2fms\n", encTh.worstLatenessUs / 1000.0);
        }

        if (inputStats.moves > 0 || inputStats.clicks > 0 || inputStats.keys > 0 || inputStats.blocked > 0 || inputStats.rateLimited > 0) {
            printf("\033[1;33m[INPUT]\033[0m Mouse: %llu moves, %llu clicks | Keys: %llu", inputStats.moves, inputStats.clicks, inputStats.keys);
            if (inputStats.blocked > 0 || inputStats.rateLimited > 0)
                printf(" | \033[33mBlocked:%llu RateLim:%llu\033[0m", inputStats.blocked, inputStats.rateLimited);
            printf("\n");
        }

        if (uptime % 10 == 0) {
            double avgFps = (double)sesFrames / uptime;
            double avgMbps = (sesBytes * 8.0 / 1048576.0) / uptime;
            printf("\033[1;35m[SESSION]\033[0m Uptime: %llus | Frames: %llu (%.1f avg) | Data: %.2fMB (%.2f Mbps avg) | Drops: %llu\n",
                   uptime, sesFrames, avgFps, sesBytes / 1048576.0, avgMbps, sesDrops);
        }
    }
};
