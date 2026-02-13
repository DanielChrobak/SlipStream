#pragma once
#include "common.hpp"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
extern "C" {
    #include <opus/opus.h>
}

class MicPlayback {
    IMMDeviceEnumerator* enm=nullptr;
    IMMDevice* dev=nullptr;
    IAudioClient* cli=nullptr;
    IAudioRenderClient* rnd=nullptr;
    OpusDecoder* dec=nullptr;
    WAVEFORMATEX* wf=nullptr;

    static constexpr int RATE=48000,FRAME_MS=10,FRAME_SZ=RATE*FRAME_MS/1000;
    static constexpr size_t MAX_Q=20;

    int devRate=48000,ch=1;
    double ratio=1.0,accum=0.0;
    std::vector<float> resBuf,prev;
    std::vector<int16_t> decBuf;

    std::atomic<bool> running{false},streaming{false},init{false};
    std::thread thr;
    std::queue<std::vector<uint8_t>> pktQ;
    std::mutex qMtx;
    std::condition_variable qCv;
    std::string targetDev;

    IMMDevice* FindDev(const std::string& name) {
        IMMDeviceCollection* col=nullptr;
        if(FAILED(enm->EnumAudioEndpoints(eRender,DEVICE_STATE_ACTIVE,&col))) return nullptr;
        UINT cnt=0; col->GetCount(&cnt);
        for(UINT i=0;i<cnt;i++) {
            IMMDevice* d=nullptr;
            if(FAILED(col->Item(i,&d))) continue;
            IPropertyStore* ps=nullptr;
            if(SUCCEEDED(d->OpenPropertyStore(STGM_READ,&ps))) {
                PROPVARIANT var; PropVariantInit(&var);
                if(SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName,&var))) {
                    char dn[256]={}; WideCharToMultiByte(CP_UTF8,0,var.pwszVal,-1,dn,sizeof(dn),nullptr,nullptr);
                    PropVariantClear(&var); ps->Release();
                    std::string dl=dn,tl=name;
                    std::transform(dl.begin(),dl.end(),dl.begin(),::tolower);
                    std::transform(tl.begin(),tl.end(),tl.begin(),::tolower);
                    if(dl.find(tl)!=std::string::npos) { col->Release(); LOG("Found VB-Cable: %s",dn); return d; }
                }
                ps->Release();
            }
            d->Release();
        }
        col->Release();
        return nullptr;
    }

    void Resample(const float* in,size_t frames) {
        if(!frames) return;
        if(devRate==RATE) {
            for(size_t i=0;i<frames;i++) for(int c=0;c<ch;c++) resBuf.push_back(in[i]);
            prev[0]=in[frames-1];
            return;
        }
        double srcRatio=(double)devRate/RATE;
        while(accum<frames) {
            size_t i0=(size_t)accum,i1=i0+1;
            double f=accum-i0;
            float s0=(i0==0&&accum<1.0)?prev[0]:in[i0];
            float s1=(i1<frames)?in[i1]:s0;
            float s=(float)(s0+(s1-s0)*f);
            for(int c=0;c<ch;c++) resBuf.push_back(s);
            accum+=1.0/srcRatio;
        }
        accum-=frames;
        prev[0]=in[frames-1];
    }

    void Loop() {
        SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_TIME_CRITICAL);
        CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        std::vector<float> decF(FRAME_SZ);
        while(running.load(std::memory_order_acquire)) {
            if(!streaming.load(std::memory_order_acquire)||!rnd||!init.load(std::memory_order_acquire)) { std::this_thread::sleep_for(10ms); continue; }
            std::vector<uint8_t> pkt;
            { std::unique_lock<std::mutex> lk(qMtx);
              if(!qCv.wait_for(lk,5ms,[this] { return !pktQ.empty()||!running.load(std::memory_order_acquire); })) continue;
              if(!running.load(std::memory_order_acquire)) break;
              if(pktQ.empty()) continue;
              pkt=std::move(pktQ.front()); pktQ.pop(); }
            if(pkt.size()<sizeof(MicPacketHeader)) continue;
            auto* h=(MicPacketHeader*)pkt.data();
            if(h->magic!=MSG_MIC_DATA) continue;
            if(h->dataLength>pkt.size()-sizeof(MicPacketHeader)) continue;
            int ds=opus_decode(dec,pkt.data()+sizeof(MicPacketHeader),h->dataLength,decBuf.data(),FRAME_SZ,0);
            if(ds<=0) continue;
            for(int i=0;i<ds;i++) decF[i]=decBuf[i]/32768.0f;
            Resample(decF.data(),ds);
            while(!resBuf.empty()&&running.load(std::memory_order_acquire)) {
                UINT32 bufFr=0,pad=0;
                cli->GetBufferSize(&bufFr);
                cli->GetCurrentPadding(&pad);
                UINT32 avail=bufFr-pad;
                if(avail==0) { std::this_thread::sleep_for(1ms); continue; }
                UINT32 toW=std::min(avail,(UINT32)(resBuf.size()/ch));
                if(toW==0) break;
                BYTE* buf=nullptr;
                if(SUCCEEDED(rnd->GetBuffer(toW,&buf))) {
                    float* out=(float*)buf;
                    for(UINT32 i=0;i<toW*ch;i++) out[i]=resBuf[i];
                    rnd->ReleaseBuffer(toW,0);
                    resBuf.erase(resBuf.begin(),resBuf.begin()+toW*ch);
                } else break;
            }
            if(resBuf.size()>FRAME_SZ*ch*10)
                resBuf.erase(resBuf.begin(),resBuf.begin()+resBuf.size()-FRAME_SZ*ch*4);
        }
        CoUninitialize();
    }

public:
    MicPlayback(const std::string& dn="CABLE Input"):targetDev(dn) {
        CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        auto chk=[](HRESULT hr,const char* m) { if(FAILED(hr)) throw std::runtime_error(m); };
        chk(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),(void**)&enm),"Enum");
        dev=FindDev(targetDev);
        if(!dev) { LOG("VB-Cable not found, using default"); chk(enm->GetDefaultAudioEndpoint(eRender,eConsole,&dev),"Default"); }
        chk(dev->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,(void**)&cli),"Client");
        chk(cli->GetMixFormat(&wf),"Format");
        devRate=wf->nSamplesPerSec;
        ch=wf->nChannels;
        ratio=(double)devRate/RATE;
        chk(cli->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,300000,0,wf,nullptr),"Init");
        chk(cli->GetService(__uuidof(IAudioRenderClient),(void**)&rnd),"Render");
        int err; dec=opus_decoder_create(RATE,1,&err);
        if(err!=OPUS_OK) throw std::runtime_error("OpusDecoder");
        decBuf.resize(FRAME_SZ);
        resBuf.reserve(FRAME_SZ*ch*8);
        prev.resize(1,0.0f);
        init.store(true,std::memory_order_release);
        LOG("MicPlayback: %dHz -> %dHz, %dch",RATE,devRate,ch);
        CoUninitialize();
    }

    ~MicPlayback() {
        Stop();
        init.store(false,std::memory_order_release);
        if(dec) opus_decoder_destroy(dec);
        if(wf) CoTaskMemFree(wf);
        SafeRelease(rnd,cli,dev,enm);
    }

    void Start() {
        if(running.load(std::memory_order_acquire)||!init.load(std::memory_order_acquire)) return;
        running.store(true,std::memory_order_release);
        accum=0.0; resBuf.clear();
        std::fill(prev.begin(),prev.end(),0.0f);
        if(FAILED(cli->Start())) { running.store(false,std::memory_order_release); return; }
        thr=std::thread(&MicPlayback::Loop,this);
        LOG("MicPlayback started");
    }

    void Stop() {
        if(!running.load(std::memory_order_acquire)) return;
        running.store(false,std::memory_order_release);
        streaming.store(false,std::memory_order_release);
        qCv.notify_all();
        if(thr.joinable()) thr.join();
        if(cli) cli->Stop();
        { std::lock_guard<std::mutex> lk(qMtx); while(!pktQ.empty()) pktQ.pop(); }
        LOG("MicPlayback stopped");
    }

    void SetStreaming(bool s) {
        bool was=streaming.exchange(s,std::memory_order_acq_rel);
        if(s&&!was) {
            std::lock_guard<std::mutex> lk(qMtx);
            while(!pktQ.empty()) pktQ.pop();
            resBuf.clear(); accum=0.0;
        }
    }

    void PushPacket(const uint8_t* data,size_t len) {
        if(!streaming.load(std::memory_order_acquire)||len<sizeof(MicPacketHeader)) return;
        std::lock_guard<std::mutex> lk(qMtx);
        if(pktQ.size()>=MAX_Q) pktQ.pop();
        pktQ.emplace(data,data+len);
        qCv.notify_one();
    }

    bool IsInitialized() const { return init.load(std::memory_order_acquire); }
};
