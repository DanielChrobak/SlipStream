#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <Windows.Graphics.Capture.Interop.h>
#include <Windows.Graphics.DirectX.Direct3D11.Interop.h>
#include <rtc/rtc.hpp>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <jwt-cpp/jwt.h>
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>
#include <fstream>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <sstream>
#include <iomanip>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

using json = nlohmann::json;
using namespace std::chrono;

namespace WGC = winrt::Windows::Graphics::Capture;
namespace WGD = winrt::Windows::Graphics::DirectX;

#define LOG(fmt, ...)  printf("[LOG] " fmt "\n", ##__VA_ARGS__)
#define WARN(fmt, ...) printf("\033[33m[WARN] " fmt "\033[0m\n", ##__VA_ARGS__)
#define ERR(fmt, ...)  fprintf(stderr, "\033[31m[ERR] " fmt "\033[0m\n", ##__VA_ARGS__)

enum MsgType : uint32_t {
    MSG_PING = 0x504E4750, MSG_FPS_SET = 0x46505343, MSG_HOST_INFO = 0x484F5354,
    MSG_FPS_ACK = 0x46505341, MSG_REQUEST_KEY = 0x4B455952, MSG_MONITOR_LIST = 0x4D4F4E4C,
    MSG_MONITOR_SET = 0x4D4F4E53, MSG_AUDIO_DATA = 0x41554449, MSG_MOUSE_MOVE = 0x4D4F5645,
    MSG_MOUSE_BTN = 0x4D42544E, MSG_MOUSE_WHEEL = 0x4D57484C, MSG_KEY = 0x4B455920,
    MSG_CODEC_SET = 0x434F4443, MSG_CODEC_ACK = 0x434F4441,
    MSG_MOUSE_MOVE_REL = 0x4D4F5652
};

enum CodecType : uint8_t {
    CODEC_H264 = 0,
    CODEC_AV1 = 1
};

inline int64_t GetQPCFrequency() {
    static const int64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    return freq;
}

inline int64_t GetTimestampUs() {
    LARGE_INTEGER n; QueryPerformanceCounter(&n);
    return (n.QuadPart * 1000000) / GetQPCFrequency();
}

inline int64_t GetTimestamp() {
    FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER uli = {{ft.dwLowDateTime, ft.dwHighDateTime}};
    return static_cast<int64_t>((uli.QuadPart - 116444736000000000ULL) / 10);
}

template<typename... T> void SafeRelease(T*&... p) { ((p ? (p->Release(), p = nullptr) : nullptr), ...); }

struct MTLock {
    ID3D11Multithread* mt;
    MTLock(ID3D11Multithread* m) : mt(m) { if (mt) mt->Enter(); }
    ~MTLock() { if (mt) mt->Leave(); }
    MTLock(const MTLock&) = delete;
    MTLock& operator=(const MTLock&) = delete;
};

struct MonitorInfo { HMONITOR hMon; int index, width, height, refreshRate; bool isPrimary; std::string name; };
extern std::vector<MonitorInfo> g_monitors;
extern std::mutex g_monitorsMutex;
void RefreshMonitorList();

constexpr int PBKDF2_ITERATIONS = 600000, PBKDF2_KEY_LENGTH = 32, SALT_LENGTH = 16;

inline std::string BytesToHex(const unsigned char* data, size_t len) {
    std::ostringstream oss;
    for (size_t i = 0; i < len; i++) oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
    return oss.str();
}

inline std::string GenerateSalt(size_t length = SALT_LENGTH) {
    std::vector<unsigned char> salt(length);
    RAND_bytes(salt.data(), static_cast<int>(length));
    return BytesToHex(salt.data(), salt.size());
}

inline std::string HashPassword(const std::string& password, const std::string& salt) {
    std::vector<unsigned char> saltBytes(salt.size() / 2);
    for (size_t i = 0; i < saltBytes.size(); i++) {
        unsigned int byte; std::sscanf(salt.c_str() + i * 2, "%02x", &byte);
        saltBytes[i] = static_cast<unsigned char>(byte);
    }
    unsigned char hash[PBKDF2_KEY_LENGTH];
    PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()), saltBytes.data(),
        static_cast<int>(saltBytes.size()), PBKDF2_ITERATIONS, EVP_sha256(), PBKDF2_KEY_LENGTH, hash);
    return BytesToHex(hash, PBKDF2_KEY_LENGTH);
}

inline bool VerifyPassword(const std::string& password, const std::string& salt, const std::string& storedHash) {
    if (salt.empty() || storedHash.empty()) return false;
    std::string computed = HashPassword(password, salt);
    if (computed.size() != storedHash.size()) return false;
    volatile int result = 0;
    for (size_t i = 0; i < computed.size(); i++) result |= computed[i] ^ storedHash[i];
    return result == 0;
}

class JWTAuth {
    std::string secret;
    static constexpr auto TOKEN_EXPIRY = hours(24);

    std::string GenerateSecret() {
        unsigned char bytes[32];
        RAND_bytes(bytes, sizeof(bytes));
        return BytesToHex(bytes, sizeof(bytes));
    }

public:
    JWTAuth() : secret(GenerateSecret()) {
        LOG("JWT secret generated");
    }

    std::string CreateToken(const std::string& username) {
        auto token = jwt::create()
            .set_issuer("slipstream")
            .set_subject(username)
            .set_issued_at(system_clock::now())
            .set_expires_at(system_clock::now() + TOKEN_EXPIRY)
            .sign(jwt::algorithm::hs256{secret});
        return token;
    }

    bool ValidateToken(const std::string& token, std::string& username) {
        try {
            auto decoded = jwt::decode(token);
            auto verifier = jwt::verify()
                .allow_algorithm(jwt::algorithm::hs256{secret})
                .with_issuer("slipstream");
            verifier.verify(decoded);
            username = decoded.get_subject();
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    std::string GetUsername(const std::string& token) {
        std::string username;
        ValidateToken(token, username);
        return username;
    }
};

class RateLimiter {
    struct RateLimit { int attempts = 0; steady_clock::time_point firstAttempt, lockoutUntil; };
    std::unordered_map<std::string, RateLimit> rateLimits;
    std::unordered_set<std::string> blacklist;
    std::mutex mtx;

    static constexpr auto ATTEMPT_WINDOW = minutes(15), LOCKOUT_DURATION = minutes(30);
    static constexpr int MAX_ATTEMPTS = 5;

public:
    bool IsAllowed(const std::string& ip) {
        std::lock_guard<std::mutex> lock(mtx);
        if (blacklist.count(ip)) return false;
        auto it = rateLimits.find(ip);
        if (it == rateLimits.end()) return true;
        auto now = steady_clock::now();
        if (it->second.lockoutUntil > steady_clock::time_point{} && now < it->second.lockoutUntil) return false;
        if (now - it->second.firstAttempt > ATTEMPT_WINDOW) { rateLimits.erase(it); return true; }
        return it->second.attempts < MAX_ATTEMPTS;
    }

    void RecordAttempt(const std::string& ip, bool success) {
        std::lock_guard<std::mutex> lock(mtx);
        if (success) { rateLimits.erase(ip); return; }
        auto now = steady_clock::now();
        auto& r = rateLimits[ip];
        if (r.attempts == 0 || now - r.firstAttempt > ATTEMPT_WINDOW) r = {1, now, {}};
        else if (++r.attempts >= MAX_ATTEMPTS) {
            r.lockoutUntil = now + LOCKOUT_DURATION;
            WARN("Rate limit: %s locked for 30 min", ip.c_str());
        }
    }

    int RemainingAttempts(const std::string& ip) {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = rateLimits.find(ip);
        return it == rateLimits.end() ? MAX_ATTEMPTS : std::max(0, MAX_ATTEMPTS - it->second.attempts);
    }

    int LockoutSecondsRemaining(const std::string& ip) {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = rateLimits.find(ip);
        if (it == rateLimits.end() || it->second.lockoutUntil <= steady_clock::now()) return 0;
        return static_cast<int>(duration_cast<seconds>(it->second.lockoutUntil - steady_clock::now()).count());
    }

    void Cleanup() {
        std::lock_guard<std::mutex> lock(mtx);
        auto now = steady_clock::now();
        std::erase_if(rateLimits, [&](const auto& p) {
            return now - p.second.firstAttempt > ATTEMPT_WINDOW && p.second.lockoutUntil <= now;
        });
    }
};

struct EncoderGPUMetrics {
    std::atomic<uint64_t> count{0}, noWait{0}, timeouts{0};
    std::atomic<int64_t> minUs{INT64_MAX}, maxUs{0}, sumUs{0};

    void Record(int64_t us, bool waited) {
        if (waited) { count++; sumUs += us; for (int64_t m = minUs.load(); us < m && !minUs.compare_exchange_weak(m, us);); for (int64_t m = maxUs.load(); us > m && !maxUs.compare_exchange_weak(m, us);); }
        else noWait++;
    }
    void RecordTimeout() { timeouts++; }

    struct Snapshot { int64_t minUs, maxUs, avgUs; uint64_t count, noWait, timeouts; };
    Snapshot GetAndReset() {
        Snapshot s{minUs.exchange(INT64_MAX), maxUs.exchange(0), 0, count.exchange(0), noWait.exchange(0), timeouts.exchange(0)};
        int64_t sum = sumUs.exchange(0);
        s.avgUs = s.count > 0 ? sum / static_cast<int64_t>(s.count) : 0;
        if (s.minUs == INT64_MAX) s.minUs = 0;
        return s;
    }
};

struct GPUWaitMetrics {
    std::atomic<uint64_t> count{0}, noWaitCount{0}, timeouts{0};
    std::atomic<int64_t> minUs{INT64_MAX}, maxUs{0}, sumUs{0};

    void Record(int64_t us) {
        count++; sumUs += us;
        for (int64_t m = minUs.load(); us < m && !minUs.compare_exchange_weak(m, us););
        for (int64_t m = maxUs.load(); us > m && !maxUs.compare_exchange_weak(m, us););
    }
    void RecordNoWait() { noWaitCount++; }
    void RecordTimeout() { timeouts++; }

    struct Snapshot { int64_t minUs, maxUs, avgUs; uint64_t count, noWaitCount, timeouts; };
    Snapshot GetAndReset() {
        Snapshot s{minUs.exchange(INT64_MAX), maxUs.exchange(0), 0, count.exchange(0), noWaitCount.exchange(0), timeouts.exchange(0)};
        int64_t sum = sumUs.exchange(0);
        s.avgUs = s.count > 0 ? sum / static_cast<int64_t>(s.count) : 0;
        if (s.minUs == INT64_MAX) s.minUs = 0;
        return s;
    }
};

struct EncoderThreadMetrics {
    std::atomic<uint64_t> handoffCount{0}, encodeCount{0}, deadlineMisses{0}, stateDropCount{0};
    std::atomic<int64_t> handoffMinUs{INT64_MAX}, handoffMaxUs{0}, handoffSumUs{0};
    std::atomic<int64_t> encodeMinUs{INT64_MAX}, encodeMaxUs{0}, encodeSumUs{0};
    std::atomic<int64_t> worstLatenessUs{0};

    void RecordHandoff(int64_t us) {
        handoffCount++; handoffSumUs += us;
        for (int64_t m = handoffMinUs.load(); us < m && !handoffMinUs.compare_exchange_weak(m, us););
        for (int64_t m = handoffMaxUs.load(); us > m && !handoffMaxUs.compare_exchange_weak(m, us););
    }
    void RecordEncode(int64_t us) {
        encodeCount++; encodeSumUs += us;
        for (int64_t m = encodeMinUs.load(); us < m && !encodeMinUs.compare_exchange_weak(m, us););
        for (int64_t m = encodeMaxUs.load(); us > m && !encodeMaxUs.compare_exchange_weak(m, us););
    }
    void RecordDeadlineMiss(int64_t latenessUs) {
        deadlineMisses++;
        for (int64_t m = worstLatenessUs.load(); latenessUs > m && !worstLatenessUs.compare_exchange_weak(m, latenessUs););
    }

    struct Snapshot {
        int64_t handoffMinUs, handoffMaxUs, handoffAvgUs;
        int64_t encodeMinUs, encodeMaxUs, encodeAvgUs;
        uint64_t handoffCount, encodeCount, deadlineMisses, stateDrops;
        int64_t worstLatenessUs;
    };
    Snapshot GetAndReset() {
        Snapshot s;
        s.handoffMinUs = handoffMinUs.exchange(INT64_MAX); if (s.handoffMinUs == INT64_MAX) s.handoffMinUs = 0;
        s.handoffMaxUs = handoffMaxUs.exchange(0);
        s.handoffCount = handoffCount.exchange(0);
        int64_t hSum = handoffSumUs.exchange(0);
        s.handoffAvgUs = s.handoffCount > 0 ? hSum / static_cast<int64_t>(s.handoffCount) : 0;

        s.encodeMinUs = encodeMinUs.exchange(INT64_MAX); if (s.encodeMinUs == INT64_MAX) s.encodeMinUs = 0;
        s.encodeMaxUs = encodeMaxUs.exchange(0);
        s.encodeCount = encodeCount.exchange(0);
        int64_t eSum = encodeSumUs.exchange(0);
        s.encodeAvgUs = s.encodeCount > 0 ? eSum / static_cast<int64_t>(s.encodeCount) : 0;

        s.deadlineMisses = deadlineMisses.exchange(0);
        s.stateDrops = stateDropCount.exchange(0);
        s.worstLatenessUs = worstLatenessUs.exchange(0);
        return s;
    }
};

struct PacedSendMetrics {
    std::atomic<uint64_t> drainEvents{0}, maxQueueDepth{0}, queueDepthSum{0}, queueDepthSamples{0};
    std::atomic<uint64_t> maxBurst{0}, burstSum{0}, burstCount{0};
    std::atomic<int64_t> maxFrameSendTimeUs{0}, frameSendTimeSum{0}, frameSendCount{0};

    void RecordDrain(size_t queueDepth, size_t burst) {
        drainEvents++;
        queueDepthSum += queueDepth; queueDepthSamples++;
        for (uint64_t m = maxQueueDepth.load(); queueDepth > m && !maxQueueDepth.compare_exchange_weak(m, queueDepth););
        burstSum += burst; burstCount++;
        for (uint64_t m = maxBurst.load(); burst > m && !maxBurst.compare_exchange_weak(m, burst););
    }
    void RecordFrameSend(int64_t us) {
        frameSendTimeSum += us; frameSendCount++;
        for (int64_t m = maxFrameSendTimeUs.load(); us > m && !maxFrameSendTimeUs.compare_exchange_weak(m, us););
    }

    struct Snapshot {
        uint64_t drainEvents, avgQueueDepth, maxQueueDepth, avgBurst, maxBurst;
        int64_t avgFrameSendTimeUs, maxFrameSendTimeUs;
    };
    Snapshot GetAndReset() {
        Snapshot s;
        s.drainEvents = drainEvents.exchange(0);
        s.maxQueueDepth = maxQueueDepth.exchange(0);
        uint64_t qdSum = queueDepthSum.exchange(0), qdSamp = queueDepthSamples.exchange(0);
        s.avgQueueDepth = qdSamp > 0 ? qdSum / qdSamp : 0;
        s.maxBurst = maxBurst.exchange(0);
        uint64_t bSum = burstSum.exchange(0), bCnt = burstCount.exchange(0);
        s.avgBurst = bCnt > 0 ? bSum / bCnt : 0;
        s.maxFrameSendTimeUs = maxFrameSendTimeUs.exchange(0);
        int64_t ftSum = frameSendTimeSum.exchange(0), ftCnt = frameSendCount.exchange(0);
        s.avgFrameSendTimeUs = ftCnt > 0 ? ftSum / ftCnt : 0;
        return s;
    }
};
