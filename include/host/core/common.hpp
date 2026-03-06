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
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <jwt-cpp/traits/nlohmann-json/defaults.h>
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
#include <filesystem>
#include <utility>
#include "host/core/logging.hpp"
#include "host/core/protocol.hpp"
#include "host/core/utils.hpp"
#include "host/core/audio_resampler.hpp"
#include "host/core/d3d_sync.hpp"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libswscale/swscale.h>
}

constexpr const char* SLIPSTREAM_VERSION = "1.0.0";
using json = nlohmann::json;
namespace WGC = winrt::Windows::Graphics::Capture;
namespace WGD = winrt::Windows::Graphics::DirectX;

struct MonitorInfo {
    HMONITOR hMon;
    int index, width, height, refreshRate;
    bool isPrimary;
    std::string name;
};

extern std::vector<MonitorInfo> g_monitors;
extern std::mutex g_monitorsMutex;

const std::string& GetSlipStreamDataDir();
std::string GetSlipStreamDataFilePath(const char* fileName);
std::vector<std::string> GetLocalIPv4Addresses();
std::string BytesToHex(const unsigned char* d, size_t n);
std::string GenerateSalt(size_t n = 16);
std::string HashPassword(const std::string& pw, const std::string& salt);
bool VerifyPassword(const std::string& pw, const std::string& salt, const std::string& stored);

class JWTAuth {
    std::string sec;
    void LoadOrGen();
public:
    JWTAuth();
    std::string CreateToken(const std::string& u);
    bool ValidateToken(const std::string& t, std::string& u);
};

class RateLimiter {
    struct RL { int att = 0; std::chrono::steady_clock::time_point first, lockout; };
    std::unordered_map<std::string, RL> lim;
    std::mutex mtx;
    static constexpr int MAX = 5;
public:
    bool IsAllowed(const std::string& ip);
    void RecordAttempt(const std::string& ip, bool ok);
    int RemainingAttempts(const std::string& ip);
    int LockoutSeconds(const std::string& ip);
};

bool EnsureSSLCert();
std::string GetSSLCertFilePath();
std::string GetSSLKeyFilePath();
