#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
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

#define LOG(fmt, ...) printf("[LOG] " fmt "\n", ##__VA_ARGS__)
#define ERR(fmt, ...) fprintf(stderr, "[ERR] " fmt "\n", ##__VA_ARGS__)

enum MsgType : uint32_t {
    MSG_PING = 0x504E4750, MSG_FPS_SET = 0x46505343, MSG_HOST_INFO = 0x484F5354,
    MSG_FPS_ACK = 0x46505341, MSG_REQUEST_KEY = 0x4B455952, MSG_MONITOR_LIST = 0x4D4F4E4C,
    MSG_MONITOR_SET = 0x4D4F4E53, MSG_AUDIO_DATA = 0x41554449, MSG_MOUSE_MOVE = 0x4D4F5645,
    MSG_MOUSE_BTN = 0x4D42544E, MSG_MOUSE_WHEEL = 0x4D57484C, MSG_KEY = 0x4B455920,
    MSG_CODEC_SET = 0x434F4443, MSG_CODEC_ACK = 0x434F4441, MSG_MOUSE_MOVE_REL = 0x4D4F5652
};
enum CodecType : uint8_t { CODEC_H264 = 0, CODEC_AV1 = 1 };

inline int64_t GetTimestamp() {
    FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER uli = {{ft.dwLowDateTime, ft.dwHighDateTime}};
    return (int64_t)((uli.QuadPart - 116444736000000000ULL) / 10);
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

constexpr int PBKDF2_ITERATIONS = 600000, PBKDF2_KEY_LEN = 32, SALT_LEN = 16;

inline std::string BytesToHex(const unsigned char* data, size_t len) {
    std::ostringstream oss;
    for (size_t i = 0; i < len; i++) oss << std::hex << std::setw(2) << std::setfill('0') << (int)data[i];
    return oss.str();
}

inline std::string GenerateSalt(size_t len = SALT_LEN) {
    std::vector<unsigned char> salt(len);
    RAND_bytes(salt.data(), (int)len);
    return BytesToHex(salt.data(), salt.size());
}

inline std::string HashPassword(const std::string& pw, const std::string& salt) {
    std::vector<unsigned char> sb(salt.size() / 2);
    for (size_t i = 0; i < sb.size(); i++) { unsigned int b; std::sscanf(salt.c_str() + i * 2, "%02x", &b); sb[i] = (unsigned char)b; }
    unsigned char hash[PBKDF2_KEY_LEN];
    PKCS5_PBKDF2_HMAC(pw.c_str(), (int)pw.size(), sb.data(), (int)sb.size(), PBKDF2_ITERATIONS, EVP_sha256(), PBKDF2_KEY_LEN, hash);
    return BytesToHex(hash, PBKDF2_KEY_LEN);
}

inline bool VerifyPassword(const std::string& pw, const std::string& salt, const std::string& stored) {
    if (salt.empty() || stored.empty()) return false;
    std::string computed = HashPassword(pw, salt);
    if (computed.size() != stored.size()) return false;
    volatile int r = 0;
    for (size_t i = 0; i < computed.size(); i++) r |= computed[i] ^ stored[i];
    return r == 0;
}

class JWTAuth {
    std::string secret;
public:
    JWTAuth() { unsigned char b[32]; RAND_bytes(b, sizeof(b)); secret = BytesToHex(b, sizeof(b)); }
    std::string CreateToken(const std::string& user) {
        return jwt::create().set_issuer("slipstream").set_subject(user)
            .set_issued_at(system_clock::now()).set_expires_at(system_clock::now() + hours(24))
            .sign(jwt::algorithm::hs256{secret});
    }
    bool ValidateToken(const std::string& token, std::string& user) {
        try {
            auto d = jwt::decode(token);
            jwt::verify().allow_algorithm(jwt::algorithm::hs256{secret}).with_issuer("slipstream").verify(d);
            user = d.get_subject(); return true;
        } catch (...) { return false; }
    }
};

class RateLimiter {
    struct RL { int attempts = 0; steady_clock::time_point first, lockout; };
    std::unordered_map<std::string, RL> limits;
    std::mutex mtx;
    static constexpr int MAX = 5;
public:
    bool IsAllowed(const std::string& ip) {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = limits.find(ip); if (it == limits.end()) return true;
        auto now = steady_clock::now();
        if (it->second.lockout > steady_clock::time_point{} && now < it->second.lockout) return false;
        if (now - it->second.first > minutes(15)) { limits.erase(it); return true; }
        return it->second.attempts < MAX;
    }
    void RecordAttempt(const std::string& ip, bool ok) {
        std::lock_guard<std::mutex> lk(mtx);
        if (ok) { limits.erase(ip); return; }
        auto now = steady_clock::now(); auto& r = limits[ip];
        if (r.attempts == 0 || now - r.first > minutes(15)) r = {1, now, {}};
        else if (++r.attempts >= MAX) r.lockout = now + minutes(30);
    }
    int RemainingAttempts(const std::string& ip) {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = limits.find(ip);
        return it == limits.end() ? MAX : std::max(0, MAX - it->second.attempts);
    }
    int LockoutSeconds(const std::string& ip) {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = limits.find(ip);
        if (it == limits.end() || it->second.lockout <= steady_clock::now()) return 0;
        return (int)duration_cast<seconds>(it->second.lockout - steady_clock::now()).count();
    }
};
