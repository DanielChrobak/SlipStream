#pragma once
#include "common.hpp"
#include <mmdeviceapi.h>
#include <audioclient.h>
extern "C" {
    #include <opus/opus.h>
}

struct AudioPacket { std::vector<uint8_t> data; int64_t ts=0; int samples=0; };

class AudioCapture {
    IMMDeviceEnumerator* enm=nullptr;
    IMMDevice* dev=nullptr;
    IAudioClient* cli=nullptr;
    IAudioCaptureClient* cap=nullptr;
    OpusEncoder* enc=nullptr;
    WAVEFORMATEX* wf=nullptr;

    static constexpr int RATE=48000,FRAME_MS=10,FRAME_SZ=RATE*FRAME_MS/1000;
    static constexpr size_t MAX_Q=10;

    int sysRate=48000,ch=2;
    double ratio=1.0,accum=0.0;
    std::vector<float> resBuf,prev,silBuf;
    std::vector<int16_t> encBuf;
    std::vector<uint8_t> outBuf;

    std::atomic<bool> running{false},capturing{false},init{false},streaming{false};
    std::thread thr;
    std::queue<AudioPacket> q;
    std::mutex qMtx;
    std::condition_variable qCv;

    void Loop() {
        SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_TIME_CRITICAL);
        CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        while(running.load(std::memory_order_acquire)) {
            if(!capturing.load(std::memory_order_acquire)||!cap||!init.load(std::memory_order_acquire)) { std::this_thread::sleep_for(10ms); continue; }
            UINT32 pktLen=0;
            if(FAILED(cap->GetNextPacketSize(&pktLen))) { std::this_thread::sleep_for(50ms); continue; }
            while(pktLen>0&&running.load(std::memory_order_acquire)&&capturing.load(std::memory_order_acquire)&&cap) {
                BYTE* data=nullptr; UINT32 frames=0; DWORD flags=0;
                if(FAILED(cap->GetBuffer(&data,&frames,&flags,nullptr,nullptr))) break;
                if(data&&frames>0) {
                    if(flags&AUDCLNT_BUFFERFLAGS_SILENT) {
                        size_t need=frames*ch;
                        if(silBuf.size()<need) silBuf.resize(need,0.0f);
                        else std::fill(silBuf.begin(),silBuf.begin()+need,0.0f);
                        Process((BYTE*)silBuf.data(),frames,GetTimestamp());
                    } else Process(data,frames,GetTimestamp());
                }
                cap->ReleaseBuffer(frames);
                if(!cap) break;
                if(FAILED(cap->GetNextPacketSize(&pktLen))) break;
            }
            std::this_thread::sleep_for(2ms);
        }
        CoUninitialize();
    }

    void Resample(const float* in,size_t frames) {
        if(!frames) return;
        if(sysRate==RATE) {
            resBuf.insert(resBuf.end(),in,in+frames*ch);
            for(int c=0;c<ch;c++) prev[c]=in[(frames-1)*ch+c];
            return;
        }
        while(accum<frames) {
            size_t i0=(size_t)accum,i1=i0+1;
            double f=accum-i0;
            for(int c=0;c<ch;c++) {
                float s0=(i0==0&&accum<1.0)?prev[c]:in[i0*ch+c];
                float s1=(i1<frames)?in[i1*ch+c]:s0;
                resBuf.push_back((float)(s0+(s1-s0)*f));
            }
            accum+=ratio;
        }
        accum-=frames;
        for(int c=0;c<ch;c++) prev[c]=in[(frames-1)*ch+c];
    }

    void Process(BYTE* data,UINT32 frames,int64_t ts) {
        if(!enc) return;
        if(!streaming.load(std::memory_order_acquire)) {
            const float* fd=(const float*)data;
            for(int c=0;c<ch;c++) prev[c]=fd[(frames-1)*ch+c];
            resBuf.clear(); return;
        }
        Resample((float*)data,frames);
        const size_t maxBuf=FRAME_SZ*ch*6;
        if(resBuf.size()>maxBuf) {
            size_t ex=resBuf.size()-FRAME_SZ*ch*2;
            resBuf.erase(resBuf.begin(),resBuf.begin()+ex);
        }
        while(resBuf.size()>=(size_t)(FRAME_SZ*ch)) {
            for(int i=0;i<FRAME_SZ*ch;i++)
                encBuf[i]=(int16_t)(std::clamp(resBuf[i],-1.0f,1.0f)*32767.0f);
            resBuf.erase(resBuf.begin(),resBuf.begin()+FRAME_SZ*ch);
            int bytes=opus_encode(enc,encBuf.data(),FRAME_SZ,outBuf.data(),(opus_int32)outBuf.size());
            if(bytes>0) {
                std::lock_guard<std::mutex> lk(qMtx);
                if(q.size()>=MAX_Q) q.pop();
                q.push({{outBuf.begin(),outBuf.begin()+bytes},ts,FRAME_SZ});
                qCv.notify_one();
            }
        }
    }

public:
    AudioCapture() {
        CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        auto chk=[](HRESULT hr,const char* m) { if(FAILED(hr)) throw std::runtime_error(m); };
        chk(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),(void**)&enm),"Enum");
        chk(enm->GetDefaultAudioEndpoint(eRender,eConsole,&dev),"Endpoint");
        chk(dev->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,(void**)&cli),"Client");
        chk(cli->GetMixFormat(&wf),"Format");
        sysRate=wf->nSamplesPerSec;
        ch=std::min((int)wf->nChannels,2);
        ratio=(double)sysRate/RATE;
        chk(cli->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_LOOPBACK,30000,0,wf,nullptr),"Init");
        chk(cli->GetService(__uuidof(IAudioCaptureClient),(void**)&cap),"Capture");
        int err; enc=opus_encoder_create(RATE,ch,OPUS_APPLICATION_RESTRICTED_LOWDELAY,&err);
        if(err!=OPUS_OK) throw std::runtime_error("Opus");
        opus_encoder_ctl(enc,OPUS_SET_BITRATE(96000));
        opus_encoder_ctl(enc,OPUS_SET_COMPLEXITY(3));
        opus_encoder_ctl(enc,OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
        opus_encoder_ctl(enc,OPUS_SET_PACKET_LOSS_PERC(0));
        opus_encoder_ctl(enc,OPUS_SET_INBAND_FEC(0));
        opus_encoder_ctl(enc,OPUS_SET_DTX(0));
        encBuf.resize(FRAME_SZ*ch);
        outBuf.resize(4000);
        resBuf.reserve(FRAME_SZ*ch*8);
        prev.resize(ch,0.0f);
        silBuf.reserve(FRAME_SZ*ch*4);
        init.store(true,std::memory_order_release);
        LOG("Audio: %dHz -> %dHz, %dch",sysRate,RATE,ch);
        CoUninitialize();
    }

    ~AudioCapture() {
        Stop();
        init.store(false,std::memory_order_release);
        if(enc) opus_encoder_destroy(enc);
        if(wf) CoTaskMemFree(wf);
        SafeRelease(cap,cli,dev,enm);
    }

    void Start() {
        if(running.load(std::memory_order_acquire)||!init.load(std::memory_order_acquire)) return;
        running.store(true,std::memory_order_release);
        capturing.store(true,std::memory_order_release);
        accum=0.0; resBuf.clear();
        std::fill(prev.begin(),prev.end(),0.0f);
        if(FAILED(cli->Start())) { running.store(false,std::memory_order_release); capturing.store(false,std::memory_order_release); return; }
        thr=std::thread(&AudioCapture::Loop,this);
    }

    void Stop() {
        if(!running.load(std::memory_order_acquire)) return;
        running.store(false,std::memory_order_release);
        capturing.store(false,std::memory_order_release);
        streaming.store(false,std::memory_order_release);
        qCv.notify_all();
        if(thr.joinable()) thr.join();
        if(cli) cli->Stop();
    }

    void SetStreaming(bool s) {
        bool was=streaming.exchange(s,std::memory_order_acq_rel);
        if(s&&!was) {
            std::lock_guard<std::mutex> lk(qMtx);
            while(!q.empty()) q.pop();
            resBuf.clear(); accum=0.0;
        }
    }

    bool PopPacket(AudioPacket& out,int ms=5) {
        std::unique_lock<std::mutex> lk(qMtx);
        if(!qCv.wait_for(lk,std::chrono::milliseconds(ms),[this] { return !q.empty()||!running.load(std::memory_order_acquire); })) return false;
        if(q.empty()) return false;
        out=std::move(q.front()); q.pop();
        return true;
    }
};
