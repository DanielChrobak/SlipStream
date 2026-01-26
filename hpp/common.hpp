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
    MSG_MOUSE_BTN = 0x4D42544E, MSG_MOUSE_WHEEL = 0x4D57484C, MSG_KEY = 0x4B455920
};

inline int64_t GetTimestamp() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER uli = {{ft.dwLowDateTime, ft.dwHighDateTime}};
    return static_cast<int64_t>((uli.QuadPart - 116444736000000000ULL) / 10);
}

template<typename... T>
void SafeRelease(T*&... p) { ((p ? (p->Release(), p = nullptr) : nullptr), ...); }

struct MTLock {
    ID3D11Multithread* mt;
    MTLock(ID3D11Multithread* m) : mt(m) { if (mt) mt->Enter(); }
    ~MTLock() { if (mt) mt->Leave(); }
    MTLock(const MTLock&) = delete;
    MTLock& operator=(const MTLock&) = delete;
};

struct MonitorInfo {
    HMONITOR hMon;
    int index, width, height, refreshRate;
    bool isPrimary;
    std::string name;
};

extern std::vector<MonitorInfo> g_monitors;
extern std::mutex g_monitorsMutex;
void RefreshMonitorList();

constexpr int PBKDF2_ITERATIONS = 600000, PBKDF2_KEY_LENGTH = 32, SALT_LENGTH = 16;

inline std::string BytesToHex(const unsigned char* data, size_t len) {
    std::ostringstream oss;
    for (size_t i = 0; i < len; i++)
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
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
        unsigned int byte;
        std::sscanf(salt.c_str() + i * 2, "%02x", &byte);
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
    if (computed.empty() || computed.size() != storedHash.size()) return false;
    volatile int result = 0;
    for (size_t i = 0; i < computed.size(); i++) result |= computed[i] ^ storedHash[i];
    return result == 0;
}

inline std::string GenerateSessionToken() { return GenerateSalt(32); }

class AuthManager {
    struct Session { std::string username, clientIP; steady_clock::time_point created, lastActivity; };
    struct RateLimit { int attempts = 0; steady_clock::time_point firstAttempt, lockoutUntil; };

    std::unordered_map<std::string, Session> sessions;
    std::unordered_map<std::string, RateLimit> rateLimits;
    std::mutex mtx;

    static constexpr auto SESSION_TIMEOUT = hours(24), ACTIVITY_TIMEOUT = hours(4);
    static constexpr auto ATTEMPT_WINDOW = minutes(15), LOCKOUT_DURATION = minutes(30);
    static constexpr int MAX_ATTEMPTS = 5;

public:
    std::string CreateSession(const std::string& username, const std::string& clientIP = "") {
        std::lock_guard<std::mutex> lock(mtx);
        std::erase_if(sessions, [&](const auto& p) { return p.second.username == username; });
        std::string token = GenerateSessionToken();
        auto now = steady_clock::now();
        sessions[token] = {username, clientIP, now, now};
        LOG("Session created for user: %s", username.c_str());
        return token;
    }

    bool ValidateSession(const std::string& token) {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = sessions.find(token);
        if (it == sessions.end()) return false;
        auto now = steady_clock::now();
        if (now - it->second.created > SESSION_TIMEOUT || now - it->second.lastActivity > ACTIVITY_TIMEOUT) {
            sessions.erase(it);
            return false;
        }
        it->second.lastActivity = now;
        return true;
    }

    void InvalidateSession(const std::string& token) {
        std::lock_guard<std::mutex> lock(mtx);
        sessions.erase(token);
    }

    std::string GetUsername(const std::string& token) {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = sessions.find(token);
        return it != sessions.end() ? it->second.username : "";
    }

    bool IsAllowed(const std::string& ip) {
        std::lock_guard<std::mutex> lock(mtx);
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
            WARN("Rate limit exceeded for: %s (locked for 30 min)", ip.c_str());
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
        std::erase_if(sessions, [&](const auto& p) {
            return now - p.second.created > SESSION_TIMEOUT || now - p.second.lastActivity > ACTIVITY_TIMEOUT;
        });
        std::erase_if(rateLimits, [&](const auto& p) {
            return now - p.second.firstAttempt > ATTEMPT_WINDOW && p.second.lockoutUntil <= now;
        });
    }
};
