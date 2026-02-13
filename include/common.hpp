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

#define LOG(f,...) printf("[LOG] " f "\n",##__VA_ARGS__)
#define ERR(f,...) fprintf(stderr,"[ERR] " f "\n",##__VA_ARGS__)
#define VIDEO_DROP(f,...) fprintf(stderr,"[VIDEO DROP] " f "\n",##__VA_ARGS__)
#define AUDIO_DROP(f,...) fprintf(stderr,"[AUDIO DROP] " f "\n",##__VA_ARGS__)
#define INPUT_DROP(f,...) fprintf(stderr,"[INPUT DROP] " f "\n",##__VA_ARGS__)
#define NETWORK_DROP(f,...) fprintf(stderr,"[NETWORK DROP] " f "\n",##__VA_ARGS__)
#define MIC_DROP(f,...) fprintf(stderr,"[MIC DROP] " f "\n",##__VA_ARGS__)

enum MsgType : uint32_t {
    MSG_PING=0x504E4750,MSG_FPS_SET=0x46505343,MSG_HOST_INFO=0x484F5354,MSG_FPS_ACK=0x46505341,
    MSG_REQUEST_KEY=0x4B455952,MSG_MONITOR_LIST=0x4D4F4E4C,MSG_MONITOR_SET=0x4D4F4E53,MSG_AUDIO_DATA=0x41554449,
    MSG_MOUSE_MOVE=0x4D4F5645,MSG_MOUSE_BTN=0x4D42544E,MSG_MOUSE_WHEEL=0x4D57484C,MSG_KEY=0x4B455920,
    MSG_CODEC_SET=0x434F4443,MSG_CODEC_ACK=0x434F4441,MSG_MOUSE_MOVE_REL=0x4D4F5652,MSG_CLIPBOARD_DATA=0x434C4950,
    MSG_CLIPBOARD_GET=0x434C4754,MSG_KICKED=0x4B49434B,MSG_CURSOR_CAPTURE=0x43555243,MSG_CURSOR_SHAPE=0x43555253,
    MSG_AUDIO_ENABLE=0x41554445,MSG_MIC_DATA=0x4D494344,MSG_MIC_ENABLE=0x4D494345
};

enum CodecType : uint8_t { CODEC_AV1=0,CODEC_H265=1,CODEC_H264=2 };

enum CursorType : uint8_t {
    CURSOR_DEFAULT=0,CURSOR_TEXT,CURSOR_POINTER,CURSOR_WAIT,CURSOR_PROGRESS,CURSOR_CROSSHAIR,CURSOR_MOVE,
    CURSOR_EW_RESIZE,CURSOR_NS_RESIZE,CURSOR_NWSE_RESIZE,CURSOR_NESW_RESIZE,CURSOR_NOT_ALLOWED,CURSOR_HELP,
    CURSOR_NONE,CURSOR_CUSTOM=255
};

#pragma pack(push,1)
struct MicPacketHeader { uint32_t magic; int64_t timestamp; uint16_t samples,dataLength; };
#pragma pack(pop)

inline int64_t GetTimestamp() {
    FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER u={{ft.dwLowDateTime,ft.dwHighDateTime}};
    return (int64_t)((u.QuadPart-116444736000000000ULL)/10);
}

template<typename...T> void SafeRelease(T*&...p) { ((p?(p->Release(),p=nullptr):nullptr),...); }

struct MTLock {
    ID3D11Multithread* m;
    MTLock(ID3D11Multithread* mt):m(mt) { if(m) m->Enter(); }
    ~MTLock() { if(m) m->Leave(); }
    MTLock(const MTLock&)=delete;
    MTLock& operator=(const MTLock&)=delete;
};

struct MonitorInfo { HMONITOR hMon; int index,width,height,refreshRate; bool isPrimary; std::string name; };
extern std::vector<MonitorInfo> g_monitors;
extern std::mutex g_monitorsMutex;
void RefreshMonitorList();

constexpr int PBKDF2_ITER=600000,PBKDF2_KLEN=32,SALT_LEN=16;

inline std::string BytesToHex(const unsigned char* d,size_t n) {
    std::ostringstream o;
    for(size_t i=0;i<n;i++) o<<std::hex<<std::setw(2)<<std::setfill('0')<<(int)d[i];
    return o.str();
}

inline std::string GenerateSalt(size_t n=SALT_LEN) {
    std::vector<unsigned char> s(n); RAND_bytes(s.data(),(int)n);
    return BytesToHex(s.data(),s.size());
}

inline std::string HashPassword(const std::string& pw,const std::string& salt) {
    std::vector<unsigned char> sb(salt.size()/2);
    for(size_t i=0;i<sb.size();i++) { unsigned int b; std::sscanf(salt.c_str()+i*2,"%02x",&b); sb[i]=(unsigned char)b; }
    unsigned char h[PBKDF2_KLEN];
    PKCS5_PBKDF2_HMAC(pw.c_str(),(int)pw.size(),sb.data(),(int)sb.size(),PBKDF2_ITER,EVP_sha256(),PBKDF2_KLEN,h);
    return BytesToHex(h,PBKDF2_KLEN);
}

inline bool VerifyPassword(const std::string& pw,const std::string& salt,const std::string& stored) {
    if(salt.empty()||stored.empty()) return false;
    std::string c=HashPassword(pw,salt);
    if(c.size()!=stored.size()) return false;
    volatile int r=0;
    for(size_t i=0;i<c.size();i++) r|=c[i]^stored[i];
    return r==0;
}

class JWTAuth {
    std::string sec;
    void LoadOrGen() {
        std::ifstream f("jwt_secret.dat");
        if(f) { std::getline(f,sec); if(sec.size()==64) return; }
        unsigned char b[32]; RAND_bytes(b,sizeof(b));
        sec=BytesToHex(b,sizeof(b));
        std::ofstream("jwt_secret.dat")<<sec;
    }
public:
    JWTAuth() { LoadOrGen(); }
    std::string CreateToken(const std::string& u) {
        return jwt::create().set_issuer("slipstream").set_subject(u)
            .set_issued_at(system_clock::now()).set_expires_at(system_clock::now()+hours(24))
            .sign(jwt::algorithm::hs256{sec});
    }
    bool ValidateToken(const std::string& t,std::string& u) {
        try {
            auto d=jwt::decode(t);
            jwt::verify().allow_algorithm(jwt::algorithm::hs256{sec}).with_issuer("slipstream").verify(d);
            u=d.get_subject(); return true;
        } catch(...) { return false; }
    }
};

class RateLimiter {
    struct RL { int att=0; steady_clock::time_point first,lockout; };
    std::unordered_map<std::string,RL> lim;
    std::mutex mtx;
    static constexpr int MAX=5;
public:
    bool IsAllowed(const std::string& ip) {
        std::lock_guard<std::mutex> lk(mtx);
        auto it=lim.find(ip); if(it==lim.end()) return true;
        auto now=steady_clock::now();
        if(it->second.lockout>steady_clock::time_point{} && now<it->second.lockout) return false;
        if(now-it->second.first>minutes(15)) { lim.erase(it); return true; }
        return it->second.att<MAX;
    }
    void RecordAttempt(const std::string& ip,bool ok) {
        std::lock_guard<std::mutex> lk(mtx);
        if(ok) { lim.erase(ip); return; }
        auto now=steady_clock::now(); auto& r=lim[ip];
        if(r.att==0||now-r.first>minutes(15)) r={1,now,{}};
        else if(++r.att>=MAX) r.lockout=now+minutes(30);
    }
    int RemainingAttempts(const std::string& ip) {
        std::lock_guard<std::mutex> lk(mtx);
        auto it=lim.find(ip);
        return it==lim.end()?MAX:std::max(0,MAX-it->second.att);
    }
    int LockoutSeconds(const std::string& ip) {
        std::lock_guard<std::mutex> lk(mtx);
        auto it=lim.find(ip);
        if(it==lim.end()||it->second.lockout<=steady_clock::now()) return 0;
        return (int)duration_cast<seconds>(it->second.lockout-steady_clock::now()).count();
    }
};

constexpr const char* SSL_CERT_FILE="server.crt";
constexpr const char* SSL_KEY_FILE="server.key";

inline bool SSLCertExists() { std::ifstream c(SSL_CERT_FILE),k(SSL_KEY_FILE); return c.good()&&k.good(); }

inline bool GenerateSSLCert(int days=3650,int bits=2048) {
    LOG("Generating self-signed SSL certificate...");
    EVP_PKEY* pkey=nullptr; X509* x509=nullptr; EVP_PKEY_CTX* ctx=nullptr;
    bool ok=false;

    ctx=EVP_PKEY_CTX_new_id(EVP_PKEY_RSA,nullptr);
    if(!ctx||EVP_PKEY_keygen_init(ctx)<=0||EVP_PKEY_CTX_set_rsa_keygen_bits(ctx,bits)<=0||EVP_PKEY_keygen(ctx,&pkey)<=0) goto end;

    x509=X509_new(); if(!x509) goto end;
    X509_set_version(x509,2);

    { unsigned char sb[16]; RAND_bytes(sb,sizeof(sb));
      BIGNUM* bn=BN_bin2bn(sb,sizeof(sb),nullptr);
      if(bn) { ASN1_INTEGER* s=ASN1_INTEGER_new(); BN_to_ASN1_INTEGER(bn,s); X509_set_serialNumber(x509,s); ASN1_INTEGER_free(s); BN_free(bn); } }

    X509_gmtime_adj(X509_getm_notBefore(x509),0);
    X509_gmtime_adj(X509_getm_notAfter(x509),days*24*3600);
    X509_set_pubkey(x509,pkey);

    { X509_NAME* n=X509_get_subject_name(x509);
      X509_NAME_add_entry_by_txt(n,"C",MBSTRING_ASC,(unsigned char*)"US",-1,-1,0);
      X509_NAME_add_entry_by_txt(n,"O",MBSTRING_ASC,(unsigned char*)"SlipStream",-1,-1,0);
      X509_NAME_add_entry_by_txt(n,"CN",MBSTRING_ASC,(unsigned char*)"localhost",-1,-1,0);
      X509_set_issuer_name(x509,n); }

    { X509V3_CTX v3; X509V3_set_ctx_nodb(&v3); X509V3_set_ctx(&v3,x509,x509,nullptr,nullptr,0);
      auto add=[&](int nid,const char* val) { if(auto e=X509V3_EXT_conf_nid(nullptr,&v3,nid,val)) { X509_add_ext(x509,e,-1); X509_EXTENSION_free(e); } };
      add(NID_basic_constraints,"CA:FALSE");
      add(NID_key_usage,"digitalSignature,keyEncipherment");
      add(NID_ext_key_usage,"serverAuth");
      add(NID_subject_alt_name,"DNS:localhost,IP:127.0.0.1,IP:0.0.0.0"); }

    if(!X509_sign(x509,pkey,EVP_sha256())) goto end;

    { BIO* kb=BIO_new_file(SSL_KEY_FILE,"wb");
      if(!kb||!PEM_write_bio_PrivateKey(kb,pkey,nullptr,nullptr,0,nullptr,nullptr)) { if(kb) BIO_free(kb); goto end; }
      BIO_free(kb); }
    { BIO* cb=BIO_new_file(SSL_CERT_FILE,"wb");
      if(!cb||!PEM_write_bio_X509(cb,x509)) { if(cb) BIO_free(cb); goto end; }
      BIO_free(cb); }

    LOG("SSL certificate generated: %s, %s",SSL_CERT_FILE,SSL_KEY_FILE);
    ok=true;

end:
    if(x509) X509_free(x509); if(pkey) EVP_PKEY_free(pkey); if(ctx) EVP_PKEY_CTX_free(ctx);
    return ok;
}

inline bool EnsureSSLCert() { return SSLCertExists()?(LOG("Using existing SSL certificates"),true):GenerateSSLCert(); }
