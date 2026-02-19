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
#include <cstdlib>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

constexpr const char* SLIPSTREAM_VERSION = "1.0.0";

using json = nlohmann::json;
using namespace std::chrono;
namespace WGC = winrt::Windows::Graphics::Capture;
namespace WGD = winrt::Windows::Graphics::DirectX;

#define LOG(f,...) printf("[LOG] " f "\n",##__VA_ARGS__)
#define ERR(f,...) fprintf(stderr,"[ERR] " f "\n",##__VA_ARGS__)
#define WARN(f,...) fprintf(stderr,"[WARN] " f "\n",##__VA_ARGS__)
#define DBG(f,...) do { if(g_debugLogging) printf("[DBG] " f "\n",##__VA_ARGS__); } while(0)

inline bool g_debugLogging = false;

const std::string& GetSlipStreamDataDir();
std::string GetSlipStreamDataFilePath(const char* fileName);

enum MsgType : uint32_t {
    MSG_PING=0x504E4750, MSG_FPS_SET=0x46505343, MSG_HOST_INFO=0x484F5354, MSG_FPS_ACK=0x46505341,
    MSG_REQUEST_KEY=0x4B455952, MSG_MONITOR_LIST=0x4D4F4E4C, MSG_MONITOR_SET=0x4D4F4E53,
    MSG_AUDIO_DATA=0x41554449, MSG_MOUSE_MOVE=0x4D4F5645, MSG_MOUSE_BTN=0x4D42544E,
    MSG_MOUSE_WHEEL=0x4D57484C, MSG_KEY=0x4B455920, MSG_CODEC_SET=0x434F4443, MSG_CODEC_ACK=0x434F4441,
    MSG_CODEC_CAPS=0x434F4350, MSG_MOUSE_MOVE_REL=0x4D4F5652, MSG_CLIPBOARD_DATA=0x434C4950,
    MSG_CLIPBOARD_GET=0x434C4754, MSG_KICKED=0x4B49434B, MSG_CURSOR_CAPTURE=0x43555243,
    MSG_CURSOR_SHAPE=0x43555253, MSG_AUDIO_ENABLE=0x41554445, MSG_MIC_DATA=0x4D494344,
    MSG_MIC_ENABLE=0x4D494345, MSG_VERSION=0x56455253
};

enum CodecType : uint8_t { CODEC_AV1=0, CODEC_H265=1, CODEC_H264=2 };

enum CursorType : uint8_t {
    CURSOR_DEFAULT=0, CURSOR_TEXT, CURSOR_POINTER, CURSOR_WAIT, CURSOR_PROGRESS, CURSOR_CROSSHAIR,
    CURSOR_MOVE, CURSOR_EW_RESIZE, CURSOR_NS_RESIZE, CURSOR_NWSE_RESIZE, CURSOR_NESW_RESIZE,
    CURSOR_NOT_ALLOWED, CURSOR_HELP, CURSOR_NONE, CURSOR_CUSTOM=255
};

#pragma pack(push,1)
struct MicPacketHeader { uint32_t magic; int64_t timestamp; uint16_t samples, dataLength; };
#pragma pack(pop)

inline int64_t GetTimestamp() {
    FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER u={{ft.dwLowDateTime,ft.dwHighDateTime}};
    return (int64_t)((u.QuadPart-116444736000000000ULL)/10);
}

template<typename...T> void SafeRelease(T*&...p) { ((p?(p->Release(),p=nullptr):nullptr),...); }
template<typename Q,typename M> void ClearQueue(Q& q,M& mtx) { std::lock_guard<M> lk(mtx); while(!q.empty()) q.pop(); }

template<typename T>
class LinearResampler {
    int srcRate, dstRate, channels;
    double ratio, accum=0.0;
    std::vector<T> prev;
public:
    std::vector<T> buf;
    LinearResampler(int src,int dst,int ch)
        : srcRate(src), dstRate(dst), channels(ch), ratio((double)src/dst), prev(ch,T{}) {
        buf.reserve(480*ch*8);
    }
    void Reset() { accum=0.0; std::fill(prev.begin(),prev.end(),T{}); buf.clear(); }
    void Process(const T* in, size_t frames) {
        if(!frames) return;
        if(srcRate==dstRate) {
            buf.insert(buf.end(), in, in+frames*channels);
            for(int c=0; c<channels; c++) prev[c]=in[(frames-1)*channels+c];
            return;
        }
        while(accum < frames) {
            size_t i0=(size_t)accum, i1=i0+1;
            double f=accum-i0;
            for(int c=0; c<channels; c++) {
                T s0 = (i0==0 && accum<1.0) ? prev[c] : in[i0*channels+c];
                T s1 = (i1<frames) ? in[i1*channels+c] : s0;
                buf.push_back((T)(s0+(s1-s0)*f));
            }
            accum += ratio;
        }
        accum -= frames;
        for(int c=0; c<channels; c++) prev[c]=in[(frames-1)*channels+c];
    }
    void ProcessMono(const T* in, size_t frames, int outCh) {
        if(!frames) return;
        double srcRatio = (double)dstRate/srcRate;
        while(accum < frames) {
            size_t i0=(size_t)accum, i1=i0+1;
            double f=accum-i0;
            T s0 = (i0==0 && accum<1.0) ? prev[0] : in[i0];
            T s1 = (i1<frames) ? in[i1] : s0;
            T s = (T)(s0+(s1-s0)*f);
            for(int c=0; c<outCh; c++) buf.push_back(s);
            accum += 1.0/srcRatio;
        }
        accum -= frames;
        prev[0] = in[frames-1];
    }
};

struct MTLock {
    ID3D11Multithread* m;
    MTLock(ID3D11Multithread* mt) : m(mt) { if(m) m->Enter(); }
    ~MTLock() { if(m) m->Leave(); }
    MTLock(const MTLock&)=delete;
    MTLock& operator=(const MTLock&)=delete;
};

struct MonitorInfo {
    HMONITOR hMon; int index, width, height, refreshRate; bool isPrimary; std::string name;
};
extern std::vector<MonitorInfo> g_monitors;
extern std::mutex g_monitorsMutex;
void RefreshMonitorList();

constexpr int PBKDF2_ITER=600000, PBKDF2_KLEN=32, SALT_LEN=16;

std::string BytesToHex(const unsigned char* d, size_t n);
std::string GenerateSalt(size_t n=SALT_LEN);
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
    struct RL { int att=0; steady_clock::time_point first, lockout; };
    std::unordered_map<std::string, RL> lim;
    std::mutex mtx;
    static constexpr int MAX=5;
public:
    bool IsAllowed(const std::string& ip);
    void RecordAttempt(const std::string& ip, bool ok);
    int RemainingAttempts(const std::string& ip);
    int LockoutSeconds(const std::string& ip);
};

std::string GetSSLCertFilePath();
std::string GetSSLKeyFilePath();
bool SSLCertExists();
bool GenerateSSLCert(int days=3650, int bits=2048);
bool EnsureSSLCert();
