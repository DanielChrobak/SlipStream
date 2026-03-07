#include "host/host_app.hpp"

#include "host/core/app_support.hpp"
#include "host/media/audio.hpp"
#include "host/media/capture.hpp"
#include "host/core/common.hpp"
#include "host/media/encoder.hpp"
#include "host/io/input.hpp"
#include "host/io/tray.hpp"
#include "host/net/port_mapper.hpp"
#include "host/net/webrtc.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <thread>
#include <utility>
#include <winsock2.h>
#include <ws2tcpip.h>

namespace host {
namespace {

constexpr int kHttpsPort = 443;
constexpr int64_t kFallbackFramePeriodUs = 16667;

class WinsockSession {
public:
    bool Start() {
        WSADATA wsaData{};
        started_ = WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
        return started_;
    }

    ~WinsockSession() {
        if (started_) {
            WSACleanup();
        }
    }

private:
    bool started_ = false;
};

class WiggleManager {
    std::atomic<bool>& running_;
    InputHandler& input_;
    std::atomic<int64_t> lastRequestTimestampMs_{0};
    std::thread worker_;
    void Run() {
        while (running_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            auto requestTimestampMs = lastRequestTimestampMs_.load(std::memory_order_acquire);
            if (requestTimestampMs <= 0) continue;
            auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (now - requestTimestampMs >= 100) { lastRequestTimestampMs_.store(0, std::memory_order_release); input_.WiggleCenter(); }
        }
    }
public:
    WiggleManager(std::atomic<bool>& r, InputHandler& i) : running_(r), input_(i), worker_([this] { Run(); }) {}
    ~WiggleManager() { if (worker_.joinable()) worker_.join(); }
    void Request() { lastRequestTimestampMs_.store(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_release); }
};

class OfferProcessingGate {
public:
    [[nodiscard]] uint64_t TryAcquire(int64_t staleAfterMs, bool& recoveredStaleOwner) {
        recoveredStaleOwner = false;
        const int64_t nowMs = NowMs();
        const uint64_t token = nextToken_.fetch_add(1, std::memory_order_acq_rel);

        uint64_t expected = 0;
        if (ownerToken_.compare_exchange_strong(expected, token, std::memory_order_acq_rel, std::memory_order_acquire)) {
            startedAtMs_.store(nowMs, std::memory_order_release);
            return token;
        }

        const int64_t startedAt = startedAtMs_.load(std::memory_order_acquire);
        if (startedAt > 0 && nowMs - startedAt >= staleAfterMs) {
            expected = ownerToken_.load(std::memory_order_acquire);
            if (expected != 0 && ownerToken_.compare_exchange_strong(expected, token, std::memory_order_acq_rel, std::memory_order_acquire)) {
                startedAtMs_.store(nowMs, std::memory_order_release);
                recoveredStaleOwner = true;
                return token;
            }
        }

        return 0;
    }

    void Release(uint64_t token) {
        if (!token) {
            return;
        }

        uint64_t expected = token;
        if (ownerToken_.compare_exchange_strong(expected, 0, std::memory_order_acq_rel, std::memory_order_acquire)) {
            startedAtMs_.store(0, std::memory_order_release);
        }
    }

private:
    static int64_t NowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    std::atomic<uint64_t> ownerToken_{0};
    std::atomic<uint64_t> nextToken_{1};
    std::atomic<int64_t> startedAtMs_{0};
};

class ScopedOfferGate {
public:
    ScopedOfferGate(OfferProcessingGate& gate, uint64_t token) : gate_(gate), token_(token) {}
    ~ScopedOfferGate() { gate_.Release(token_); }

    ScopedOfferGate(const ScopedOfferGate&) = delete;
    ScopedOfferGate& operator=(const ScopedOfferGate&) = delete;

private:
    OfferProcessingGate& gate_;
    uint64_t token_ = 0;
};

void JoinIfJoinable(std::thread& thread) {
    if (thread.joinable()) {
        thread.join();
    }
}

template <typename LoopFn>
std::thread StartThreadWithPriority(int priority, LoopFn&& loopFn) {
    return std::thread([priority, loop = std::forward<LoopFn>(loopFn)]() mutable {
        SetThreadPriority(GetCurrentThread(), priority);
        loop();
    });
}

void UpdateInputBoundsForMonitor(InputHandler& input, int monitorIndex) {
    std::lock_guard<std::mutex> lock(g_monitorsMutex);
    if (monitorIndex < 0 || monitorIndex >= static_cast<int>(g_monitors.size())) {
        return;
    }
    input.UpdateFromMonitorInfo(g_monitors[monitorIndex]);
}

void RegisterStaticRoutes(httplib::SSLServer& server) {
    server.Get("/", [](auto&, auto& response) {
        const auto content = LoadFile("index.html");
        response.set_content(content.empty() ? "<h1>index.html not found</h1>" : content, "text/html");
    });

    server.Get("/styles.css", [](auto&, auto& response) {
        response.set_content(LoadFile("styles.css"), "text/css");
    });

    server.Get("/SlipStream.ico", [](auto&, auto& response) {
        response.set_content(LoadFile("SlipStream.ico"), "image/x-icon");
    });

    constexpr std::array<const char*, 12> kJsModules = {"constants", "input", "media", "network", "renderer", "state", "ui", "mic", "auth", "protocol", "audio-worklet", "mic-worklet"};
    for (const auto* module : kJsModules) {
        server.Get(std::string("/js/") + module + ".js", [module](auto&, auto& response) {
            response.set_content(LoadFile((std::string("js/") + module + ".js").c_str()), "application/javascript");
        });
    }
}

void RegisterAuthRoutes(httplib::SSLServer& server) {
    server.Post("/api/auth", HandleAuth);

    server.Post("/api/logout", [](auto&, auto& response) {
        response.set_header("Set-Cookie", "session=; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=0");
        response.set_content(R"({"success":true})", "application/json");
    });

    server.Get("/api/session", AuthRequired([](const httplib::Request&, httplib::Response& response, const std::string& user) {
        response.set_content(json{{"valid", true}, {"username", user}}.dump(), "application/json");
    }));
}

void RegisterNetworkRoutes(httplib::SSLServer& server, const std::shared_ptr<PortMapper>& portMapper) {
    server.Get("/api/network", AuthRequired([portMapper](const httplib::Request&, httplib::Response& response, const std::string&) {
        if (!portMapper) {
            response.set_content(json{{"portMapping", json{{"enabled", false}}}}.dump(), "application/json");
            return;
        }

        const PortMappingStatus status = portMapper->GetStatus();
        response.set_content(json{{"portMapping", {
                {"enabled", status.enabled},
                {"running", status.running},
                {"active", status.active},
                {"signalTcpMapped", status.signalTcpMapped},
                {"webrtcUdpAvailable", status.webrtcUdpAvailable},
                {"webrtcTcpAvailable", status.webrtcTcpAvailable},
                {"httpsPort", status.httpsPort},
                {"icePortBegin", status.icePortBegin},
                {"icePortEnd", status.icePortEnd},
                {"iceTcpEnabled", status.iceTcpEnabled},
                {"requestedLeaseSeconds", status.requestedLeaseSeconds},
                {"assignedLeaseSeconds", status.assignedLeaseSeconds},
                {"requestedUdpPorts", status.requestedUdpPorts},
                {"requestedTcpPorts", status.requestedTcpPorts},
                {"mappedUdpPorts", status.mappedUdpPorts},
                {"mappedTcpPorts", status.mappedTcpPorts},
                {"failedUdpPorts", status.failedUdpPorts},
                {"failedTcpPorts", status.failedTcpPorts},
                {"method", status.method},
                {"gatewayAddress", status.gatewayAddress},
                {"localAddress", status.localAddress},
                {"externalAddress", status.externalAddress},
                {"lastError", status.lastError},
            }}}.dump(), "application/json");
    }));
}

void RegisterOfferRoute(httplib::SSLServer& server, const std::shared_ptr<WebRTCServer>& webrtcServer, OfferProcessingGate& offerGate) {
    server.Post("/api/offer", AuthRequired([&](const httplib::Request& request, httplib::Response& response, const std::string&) {
        if (request.body.size() > 65536) {
            JsonError(response, 413, "Payload too large");
            return;
        }

        bool recoveredStaleOwner = false;
        uint64_t offerToken = offerGate.TryAcquire(8000, recoveredStaleOwner);
        if (!offerToken) {
            JsonError(response, 503, "Offer processing busy");
            return;
        }
        ScopedOfferGate offerGuard(offerGate, offerToken);
        if (recoveredStaleOwner) {
            WARN("Offer route: recovered stale offer processing owner");
        }

        try {
            const auto body = json::parse(request.body);
            if (!body.contains("sdp") || !body["sdp"].is_string()) {
                JsonError(response, 400, "Missing SDP");
                return;
            }

            std::string offer = body["sdp"];
            if (offer.empty() || offer.size() > 65536) {
                JsonError(response, 400, "Invalid SDP");
                return;
            }

            webrtcServer->SetRemote(offer, "offer");
            std::string answer = webrtcServer->GetLocal();
            if (answer.empty()) {
                JsonError(response, 500, "Failed to generate answer");
                return;
            }

            if (const size_t setupPos = answer.find("a=setup:actpass"); setupPos != std::string::npos) {
                answer.replace(setupPos, 15, "a=setup:active");
            }

            response.set_content(json{{"sdp", answer}, {"type", "answer"}}.dump(), "application/json");
        } catch (...) {
            JsonError(response, 400, "Invalid offer");
        }
    }));
}

std::thread StartEncoderThread(
    FrameSlot& frameSlot,
    ScreenCapture& capture,
    const std::shared_ptr<WebRTCServer>& webrtcServer,
    std::atomic<bool>& running,
    std::mutex& encoderMutex,
    std::unique_ptr<VideoEncoder>& encoder,
    std::atomic<bool>& encoderReady,
    std::atomic<int64_t>& lastEncodeTs,
    std::atomic<int>& targetFps) {
    return std::thread([&] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

        FrameData currentFrame;
        FrameData pendingFrame;

        bool wasStreaming = false;
        bool hasPendingFrame = false;

        int64_t framePeriodUs = kFallbackFramePeriodUs;
        int64_t nextTs = 0;
        uint64_t lastGeneration = frameSlot.GetGeneration();

        auto freePending = [&] { frameSlot.MarkReleased(pendingFrame.poolIdx); pendingFrame.Release(); hasPendingFrame = false; };
        auto freeCurrent = [&] { frameSlot.MarkReleased(currentFrame.poolIdx); currentFrame.Release(); };
        auto promoteCurrent = [&] { pendingFrame = currentFrame; currentFrame = {}; hasPendingFrame = true; };
        auto loadTargetFps = [&] {
            int fps = targetFps.load(std::memory_order_acquire);
            return fps > 0 ? fps : 60;
        };

        while (running.load(std::memory_order_acquire)) {
            if (!frameSlot.Pop(currentFrame)) {
                if (!running.load(std::memory_order_acquire)) {
                    break;
                }
                continue;
            }

            const int64_t now = GetTimestamp();
            const uint64_t currentGeneration = frameSlot.GetGeneration();

            if (currentGeneration != lastGeneration) {
                if (hasPendingFrame) freePending();
                lastGeneration = currentGeneration;
                nextTs = 0;
            }

            if (currentFrame.generation != currentGeneration) { freeCurrent(); continue; }

            bool isStreaming = false;
            try {
                isStreaming = webrtcServer->IsStreaming() && encoderReady.load(std::memory_order_acquire);
            } catch (const std::exception& e) {
                ERR("EncoderThread: Exception checking streaming state: %s", e.what());
            } catch (...) {
                ERR("EncoderThread: Unknown exception checking streaming state");
            }

            if (isStreaming && !wasStreaming) {
                LOG("EncoderThread: Streaming started (fps=%d)", loadTargetFps());
                std::lock_guard<std::mutex> lock(encoderMutex);
                if (encoder) {
                    encoder->Flush();
                }

                framePeriodUs = 1000000 / loadTargetFps();
                lastEncodeTs.store(0, std::memory_order_release);
                nextTs = 0;
                if (hasPendingFrame) freePending();
            }

            wasStreaming = isStreaming;

            if (!isStreaming || !currentFrame.tex) {
                if (isStreaming && !currentFrame.tex) {
                    WARN("EncoderThread: Frame has null texture (ts=%lld, gen=%llu) - dropping",
                        currentFrame.ts, currentFrame.generation);
                }
                freeCurrent();
                continue;
            }

            framePeriodUs = 1000000 / loadTargetFps();

            bool needsKeyFrame = true;
            try {
                needsKeyFrame = webrtcServer->NeedsKey();
            } catch (const std::exception& e) {
                ERR("EncoderThread: Exception checking NeedsKey: %s", e.what());
            } catch (...) {
                ERR("EncoderThread: Unknown exception checking NeedsKey");
            }

            if (nextTs == 0) {
                nextTs = currentFrame.ts;
            }

            const auto encodeAndSend = [&](FrameData& frame, bool forceKey) {
                if (frame.needsSync && !capture.WaitReady(frame.fence)) {
                    WARN("EncoderThread: GPU fence not ready (ts=%lld, forceKey=%d) - frame dropped",
                        frame.ts, forceKey ? 1 : 0);
                    return false;
                }

                try {
                    std::lock_guard<std::mutex> lock(encoderMutex);
                    if (encoder && webrtcServer->IsStreaming()) {
                        auto* encoded = encoder->Encode(frame.tex, frame.ts, frame.sourceTs, forceKey);
                        if (encoded) {
                            if (webrtcServer->Send(*encoded)) {
                                lastEncodeTs.store(frame.ts, std::memory_order_release);
                            } else {
                                WARN("EncoderThread: WebRTC Send failed (ts=%lld, key=%d, size=%zu)",
                                    encoded->ts, encoded->isKey ? 1 : 0, encoded->data.size());
                            }
                        } else {
                            DBG("EncoderThread: Encode returned null (ts=%lld, forceKey=%d) - frame dropped by encoder",
                                frame.ts, forceKey ? 1 : 0);
                        }
                    } else {
                        DBG("EncoderThread: Skipping encode - encoder=%s streaming=%s",
                            encoder ? "yes" : "no", webrtcServer->IsStreaming() ? "yes" : "no");
                    }
                } catch (const std::exception& e) {
                    ERR("EncoderThread: Exception during encode/send: %s (ts=%lld, forceKey=%d)",
                        e.what(), frame.ts, forceKey ? 1 : 0);
                } catch (...) {
                    ERR("EncoderThread: Unknown exception during encode/send (ts=%lld)", frame.ts);
                }

                try {
                    std::lock_guard<std::mutex> lock(encoderMutex);
                    if (encoder && !encoder->IsEncodeComplete()) {
                        int retry;
                        for (retry = 0; retry < 8 && !encoder->IsEncodeComplete(); ++retry) {
                            std::this_thread::sleep_for(std::chrono::microseconds(500));
                        }
                        if (retry >= 8) {
                            WARN("EncoderThread: Encode completion timeout after %d retries (ts=%lld)", retry, frame.ts);
                        }
                    }
                } catch (const std::exception& e) {
                    ERR("EncoderThread: Exception waiting for encode completion: %s", e.what());
                } catch (...) {
                    ERR("EncoderThread: Unknown exception waiting for encode completion");
                }

                return true;
            };

            if (needsKeyFrame) {
                if (hasPendingFrame) freePending();
                if (encodeAndSend(currentFrame, true)) nextTs = currentFrame.ts + framePeriodUs;
                freeCurrent();
                continue;
            }

            // Backpressure: skip encoding when the network send queue is congested
            static int congestionSkipCount = 0;
            static int64_t congestionStartUs = 0;
            bool congested = false;
            try { congested = webrtcServer->IsCongested(); } catch (...) {}
            if (congested && !needsKeyFrame) {
                if (congestionSkipCount == 0) congestionStartUs = GetTimestamp();
                congestionSkipCount++;
                if (congestionSkipCount == 1 || congestionSkipCount % 10 == 0) {
                    DBG("EncoderThread: Congestion skip #%d (ts=%lld, duration=%lldus)",
                        congestionSkipCount, currentFrame.ts, GetTimestamp() - congestionStartUs);
                }
                DBG("EncoderThread: Network congested, skipping frame (ts=%lld)", currentFrame.ts);
                freeCurrent();
                // Advance nextTs so we don't try to catch up and flood the queue further
                while (nextTs < currentFrame.ts) nextTs += framePeriodUs;
                continue;
            } else if (!congested && congestionSkipCount > 0) {
                LOG("EncoderThread: Congestion cleared after %d skipped frames (%lldus total)",
                    congestionSkipCount, GetTimestamp() - congestionStartUs);
                congestionSkipCount = 0;
            }

            if (currentFrame.ts - nextTs < -framePeriodUs * 3 / 2) {
                DBG("EncoderThread: Frame too old (ts=%lld nextTs=%lld delta=%lld framePeriod=%lld) - dropping",
                    currentFrame.ts, nextTs, currentFrame.ts - nextTs, framePeriodUs);
                freeCurrent();
                continue;
            }

            if (hasPendingFrame) {
                if (pendingFrame.generation != currentGeneration) freePending();
                if (hasPendingFrame) {
                    if (std::abs(currentFrame.ts - nextTs) < std::abs(pendingFrame.ts - nextTs)) {
                        freePending();
                        promoteCurrent();
                    } else { freeCurrent(); }
                } else { promoteCurrent(); }
            } else { promoteCurrent(); }

            if (hasPendingFrame && (pendingFrame.ts >= nextTs || now >= nextTs + framePeriodUs / 2)) {
                if (pendingFrame.generation != frameSlot.GetGeneration()) { freePending(); continue; }

                if (now - pendingFrame.ts > framePeriodUs * 2) {
                    DBG("EncoderThread: Pending frame too stale (age=%lldus, limit=%lldus, ts=%lld) - dropping",
                        now - pendingFrame.ts, framePeriodUs * 2, pendingFrame.ts);
                    freePending();
                    while (nextTs < now - framePeriodUs) nextTs += framePeriodUs;
                    continue;
                }

                encodeAndSend(pendingFrame, false);
                freePending();
                nextTs += framePeriodUs;
                if (nextTs < now - framePeriodUs * 2) {
                    nextTs = now;
                }
            }
        }

        if (hasPendingFrame) freePending();
    });
}

void ConfigureRuntime(int argc, char* argv[]) {
    InitLogging();
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--debug") == 0 || std::strcmp(argv[i], "-d") == 0) {
            g_debugLogging = true;
        }
    }

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
}

bool InitializeHostSecurity(WinsockSession& winsockSession) {
    if (!winsockSession.Start()) {
        ERR("WSAStartup failed");
        return false;
    }

    SetupConfig();
    if (!EnsureSSLCert()) {
        ERR("SSL cert init failed");
        return false;
    }

    return true;
}

void ConfigureHttpServer(
    httplib::SSLServer& server,
    const std::shared_ptr<WebRTCServer>& webrtcServer,
    const std::shared_ptr<PortMapper>& portMapper,
    OfferProcessingGate& offerGate) {
    server.set_post_routing_handler(SetupCORS);
    server.Options(".*", [](auto&, auto& response) { response.status = 204; });

    RegisterStaticRoutes(server);
    RegisterAuthRoutes(server);
    RegisterNetworkRoutes(server, portMapper);
    RegisterOfferRoute(server, webrtcServer, offerGate);
}

void PrintStartupInfo(
    const std::vector<std::string>& localIpAddresses,
    const AppContext& app,
    const ScreenCapture& capture,
    const MicPlayback* micPlayback,
    const PortMapper* portMapper) {
    printf("SlipStream v%s on port %d\n", SLIPSTREAM_VERSION, kHttpsPort);
    printf("  Local: https://localhost:%d\n", kHttpsPort);
    for (const auto& ip : localIpAddresses) {
        printf("  Network: https://%s:%d\n", ip.c_str(), kHttpsPort);
    }
    printf("  User: %s | Display: %dHz\n", app.config.username.c_str(), capture.GetHostFPS());
    printf("  Mic: %s\n", micPlayback && micPlayback->IsInitialized() ? micPlayback->GetDeviceName().c_str() : "N/A");
    if (portMapper) {
        const PortMappingStatus status = portMapper->GetStatus();
        printf("  Port mapping: %s", status.enabled ? "enabled" : "disabled");
        if (status.active) {
            printf(" via %s", status.method.c_str());
            if (!status.externalAddress.empty()) {
                printf(" (%s)", status.externalAddress.c_str());
            }
        } else if (!status.lastError.empty()) {
            printf(" (%s)", status.lastError.c_str());
        }
        printf("\n");
    }
}

struct WorkerThreads {
    std::thread serverThread;
    std::thread audioThread;
    std::thread cursorThread;
    std::thread encoderThread;
};

bool StartWorkerThreads(
    AppContext& app,
    httplib::SSLServer& server,
    FrameSlot& frameSlot,
    ScreenCapture& capture,
    const std::shared_ptr<WebRTCServer>& webrtcServer,
    InputHandler& input,
    const std::atomic<bool>& cursorCapture,
    std::mutex& encoderMutex,
    std::unique_ptr<VideoEncoder>& encoder,
    std::atomic<bool>& encoderReady,
    std::atomic<int64_t>& lastEncodeTs,
    std::atomic<int>& targetFps,
    std::unique_ptr<AudioCapture>& audioCapture,
    WorkerThreads& threads) {
    const bool bound = server.bind_to_port("0.0.0.0", kHttpsPort);
    if (!bound) {
        ERR("HTTPS server bind failed on port %d", kHttpsPort);
        return false;
    }

    threads = {};
    threads.serverThread = std::thread([&] {
        if (!server.listen_after_bind()) {
            ERR("HTTPS server listener exited unexpectedly");
        }
    });

    threads.audioThread = StartThreadWithPriority(THREAD_PRIORITY_HIGHEST, [audioCapture = audioCapture.get(), webrtcServer, &app] {
        if (audioCapture == nullptr) {
            return;
        }

        AudioPacket packet;
        while (app.running.load(std::memory_order_acquire)) {
            if (!webrtcServer->IsStreaming()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            if (audioCapture->PopPacket(packet, 5)) {
                try {
                    bool sent = webrtcServer->SendAudio(packet.data, packet.ts, packet.samples);
                    if (!sent) {
                        DBG("AudioThread: SendAudio returned false (dataSize=%zu, ts=%lld, samples=%d)",
                            packet.data.size(), packet.ts, packet.samples);
                    }
                } catch (const std::exception& e) {
                    ERR("AudioThread: Exception sending audio: %s", e.what());
                } catch (...) {
                    ERR("AudioThread: Unknown exception sending audio");
                }
            }
        }
    });

    threads.cursorThread = StartThreadWithPriority(THREAD_PRIORITY_BELOW_NORMAL, [&input, &cursorCapture, webrtcServer, &app] {
        while (app.running.load(std::memory_order_acquire)) {
            if (!webrtcServer->IsStreaming() || cursorCapture.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            CursorType cursor{};
            if (input.GetCurrentCursor(cursor)) {
                try {
                    bool sent = webrtcServer->SendCursorShape(cursor);
                    if (!sent) {
                        DBG("CursorThread: SendCursorShape returned false");
                    }
                } catch (const std::exception& e) {
                    ERR("CursorThread: Exception sending cursor: %s", e.what());
                } catch (...) {
                    ERR("CursorThread: Unknown exception sending cursor");
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
    });

    threads.encoderThread = StartEncoderThread(
        frameSlot,
        capture,
        webrtcServer,
        app.running,
        encoderMutex,
        encoder,
        encoderReady,
        lastEncodeTs,
        targetFps);

    return true;
}

void ShutdownHost(
    std::unique_ptr<AudioCapture>& audioCapture,
    std::unique_ptr<MicPlayback>& micPlayback,
    httplib::SSLServer& server,
    FrameSlot& frameSlot,
    const std::shared_ptr<WebRTCServer>& webrtcServer,
    const std::shared_ptr<PortMapper>& portMapper,
    WorkerThreads& threads) {
    if (audioCapture) {
        audioCapture->Stop();
    }
    if (micPlayback) {
        micPlayback->Stop();
    }

    server.stop();
    frameSlot.Wake();

    JoinIfJoinable(threads.encoderThread);
    JoinIfJoinable(threads.audioThread);
    JoinIfJoinable(threads.cursorThread);
    JoinIfJoinable(threads.serverThread);

    if (portMapper) {
        portMapper->Stop();
    }

    if (webrtcServer) {
        webrtcServer->Shutdown();
    }
}

} // namespace

int RunHostApp(int argc, char* argv[]) {
    try {
        auto& app = GetAppContext();
        ConfigureRuntime(argc, argv);

        printf("\n=== SlipStream Server v%s ===\n\n", SLIPSTREAM_VERSION);

        WinsockSession winsockSession;
        if (!InitializeHostSecurity(winsockSession)) {
            return 1;
        }

        SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
        const auto localIpAddresses = GetLocalIPv4Addresses();

        FrameSlot frameSlot;
        auto webrtcServer = std::make_shared<WebRTCServer>();
        auto portMapper = std::make_shared<PortMapper>(
            static_cast<uint16_t>(kHttpsPort),
            webrtcServer->GetIcePortRangeBegin(),
            webrtcServer->GetIcePortRangeEnd(),
            webrtcServer->IsIceTcpEnabled());
        ScreenCapture capture(&frameSlot);

        std::mutex encoderMutex;
        std::unique_ptr<VideoEncoder> encoder;
        std::atomic<bool> encoderReady{false};
        std::atomic<CodecType> currentCodec{CODEC_AV1};
        std::atomic<bool> softwareEncodeRequested{false};
        std::atomic<bool> softwareEncodeEffective{false};
        std::atomic<bool> softwareEncodeForced{false};
        std::mutex encoderInfoMutex;
        std::string activeEncoderName;

        InputHandler input;
        input.Enable();
        UpdateInputBoundsForMonitor(input, capture.GetCurrentMonitorIndex());

        WiggleManager wiggle(app.running, input);

        const uint8_t codecCaps = VideoEncoder::ProbeSupport(capture.GetDev());
        const uint8_t hardwareCodecCaps = VideoEncoder::ProbeHardwareSupport(capture.GetDev());
        LOG("Codec support: AV1=%d H265=%d H264=%d", (codecCaps & 1) ? 1 : 0, (codecCaps & 2) ? 1 : 0, (codecCaps & 4) ? 1 : 0);

        std::unique_ptr<AudioCapture> audioCapture;
        try { audioCapture = std::make_unique<AudioCapture>(); } catch (...) { WARN("AudioCapture init failed"); }
        std::unique_ptr<MicPlayback> micPlayback;
        try { micPlayback = std::make_unique<MicPlayback>("CABLE Input"); } catch (...) { LOG("MicPlayback not available"); }

        auto updateSoftwareEncodeState = [&](CodecType codec, bool requested, const VideoEncoder* activeEncoder) {
            const bool hardwareAvailable = (hardwareCodecCaps & (1 << static_cast<int>(codec))) != 0;
            const bool usingHardware = activeEncoder ? activeEncoder->IsUsingHardware() : hardwareAvailable;
            const bool activeSoftware = !usingHardware;
            const bool forced = !hardwareAvailable;
            softwareEncodeEffective.store(activeSoftware, std::memory_order_release);
            softwareEncodeForced.store(forced, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(encoderInfoMutex);
                activeEncoderName = activeEncoder ? activeEncoder->GetActiveEncoderName() : std::string{};
            }
            LOG("Software encode state: requested=%d activeSoftware=%d forced=%d usingHardware=%d codec=%s encoder=%s",
                requested ? 1 : 0,
                activeSoftware ? 1 : 0,
                forced ? 1 : 0,
                usingHardware ? 1 : 0,
                VideoEncoder::CodecName(codec),
                activeEncoder ? activeEncoder->GetActiveEncoderName().c_str() : "unknown");
        };

        auto createEncoder = [&](int width, int height, int fps, CodecType codec) {
            const bool preferSoftware = softwareEncodeRequested.load(std::memory_order_acquire);
            auto nextEncoder = std::make_unique<VideoEncoder>(
                width,
                height,
                fps,
                capture.GetDev(),
                capture.GetCtx(),
                capture.GetMT(),
                codec,
                preferSoftware);
            currentCodec.store(codec, std::memory_order_release);
            updateSoftwareEncodeState(codec, preferSoftware, nextEncoder.get());
            return nextEncoder;
        };

        auto rebuildEncoder = [&](int width, int height, int fps, CodecType codec) -> bool {
            std::lock_guard<std::mutex> lock(encoderMutex);
            encoderReady.store(false, std::memory_order_release);
            encoder.reset();

            try {
                encoder = createEncoder(width, height, fps, codec);
                encoderReady.store(true, std::memory_order_release);
                return true;
            } catch (const std::exception& ex) {
                ERR("Encoder creation failed: %s", ex.what());
                return false;
            }
        };

        capture.SetResolutionChangeCallback([&](int width, int height, int fps) {
            LOG("Resolution change: %dx%d@%d", width, height, fps);
            rebuildEncoder(width, height, fps, currentCodec.load(std::memory_order_acquire));
        });

        std::atomic<bool> cursorCapture{false};
        std::atomic<int64_t> lastEncodeTs{0};
        std::atomic<int> targetFps{60};

        webrtcServer->Init({
            &input,
            [&](int fps, uint8_t) {
                capture.SetFPS(fps);
                targetFps.store(fps, std::memory_order_release);
                lastEncodeTs.store(0, std::memory_order_release);
                std::lock_guard<std::mutex> lock(encoderMutex);
                if (encoder) {
                    encoder->UpdateFPS(fps);
                } else {
                    try {
                        encoder = createEncoder(capture.GetW(), capture.GetH(), fps,
                            currentCodec.load(std::memory_order_acquire));
                        encoderReady.store(true, std::memory_order_release);
                    } catch (const std::exception& e) {
                        ERR("Failed to create encoder: %s", e.what());
                    } catch (...) {
                        ERR("Failed to create encoder: unknown exception");
                    }
                }
                if (!capture.IsCapturing()) capture.StartCapture();
                frameSlot.Wake();
            },
            [&] { return capture.RefreshHostFPS(); },
            [&] { return capture.GetCurrentMonitorIndex(); },
            [&](int idx) -> bool {
                if (!capture.SwitchMonitor(idx)) return false;
                UpdateInputBoundsForMonitor(input, idx);
                lastEncodeTs.store(0, std::memory_order_release);
                wiggle.Request();
                return true;
            },
            [&] {
                capture.PauseCapture(); frameSlot.Wake();
                lastEncodeTs.store(0, std::memory_order_release);
                if (audioCapture) audioCapture->SetStreaming(false);
            },
            [&] { frameSlot.Wake(); lastEncodeTs.store(0, std::memory_order_release); wiggle.Request(); },
            [&](CodecType codec) -> bool {
                if (codec == currentCodec.load(std::memory_order_acquire)) return true;
                if (!(codecCaps & (1 << static_cast<int>(codec)))) return false;
                if (!rebuildEncoder(capture.GetW(), capture.GetH(), capture.GetCurrentFPS(), codec)) return false;
                lastEncodeTs.store(0, std::memory_order_release);
                return true;
            },
            [&](bool enabled) -> bool {
                CodecType codec = currentCodec.load(std::memory_order_acquire);
                const bool forced = (hardwareCodecCaps & (1 << static_cast<int>(codec))) == 0;
                const bool requested = forced ? true : enabled;

                if (softwareEncodeRequested.load(std::memory_order_acquire) == requested &&
                    softwareEncodeForced.load(std::memory_order_acquire) == forced) {
                    updateSoftwareEncodeState(codec, requested, encoder.get());
                    return true;
                }

                softwareEncodeRequested.store(requested, std::memory_order_release);
                if (!rebuildEncoder(capture.GetW(), capture.GetH(), capture.GetCurrentFPS(), codec)) return false;
                lastEncodeTs.store(0, std::memory_order_release);
                frameSlot.Wake();
                return true;
            },
            [&] { return currentCodec.load(std::memory_order_acquire); },
            [&] { return codecCaps; },
            [&] {
                uint8_t flags = 0;
                if (softwareEncodeEffective.load(std::memory_order_acquire)) flags |= 0x01;
                if (softwareEncodeForced.load(std::memory_order_acquire)) flags |= 0x02;
                return flags;
            },
            [&] {
                std::lock_guard<std::mutex> lock(encoderInfoMutex);
                return activeEncoderName;
            },
            [&] { return input.GetClipboardText(); },
            [&](const std::string& text) { return input.SetClipboardText(text); },
            [&](bool e) { cursorCapture.store(e, std::memory_order_release); capture.SetCursorCapture(e); },
            [&](bool e) { if (audioCapture) audioCapture->SetStreaming(e); },
            [&](bool e) { if (micPlayback) micPlayback->SetStreaming(e); },
            [&](const uint8_t* d, size_t n) { if (micPlayback && micPlayback->IsInitialized()) micPlayback->PushPacket(d, n); },
        });

        const auto certPath = GetSSLCertFilePath();
        const auto keyPath = GetSSLKeyFilePath();
        httplib::SSLServer server(certPath.c_str(), keyPath.c_str());
        if (!server.is_valid()) {
            ERR("HTTPS server init failed");
            return 1;
        }

        OfferProcessingGate offerGate;
        ConfigureHttpServer(server, webrtcServer, portMapper, offerGate);

        WorkerThreads threads{};
        if (!StartWorkerThreads(
                app,
                server,
                frameSlot,
                capture,
                webrtcServer,
                input,
                cursorCapture,
                encoderMutex,
                encoder,
                encoderReady,
                lastEncodeTs,
                targetFps,
                audioCapture,
                threads)) {
            return 1;
        }

        if (portMapper) {
            portMapper->Start();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        PrintStartupInfo(localIpAddresses, app, capture, micPlayback.get(), portMapper.get());

        if (audioCapture) audioCapture->Start();
        if (micPlayback) micPlayback->Start();

        if (!InitAppTray()) {
            WARN("Tray init failed");
        }

        while (app.running.load(std::memory_order_acquire)) {
            PumpAppTrayMessages();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        LOG("Shutting down...");
        ShutdownHost(audioCapture, micPlayback, server, frameSlot, webrtcServer, portMapper, threads);

        CleanupAppTray();
        LOG("Shutdown complete");
    } catch (const std::exception& ex) {
        CleanupAppTray();
        ERR("Fatal: %s", ex.what());
        return 1;
    }

    return 0;
}

} // namespace host
