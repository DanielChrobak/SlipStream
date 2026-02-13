#pragma once
#include "common.hpp"
#include "encoder.hpp"
#include "input.hpp"

#pragma pack(push,1)
struct PacketHeader { int64_t timestamp; uint32_t encodeTimeUs,frameId; uint16_t chunkIndex,totalChunks; uint8_t frameType; };
struct AudioPacketHeader { uint32_t magic; int64_t timestamp; uint16_t samples,dataLength; };
#pragma pack(pop)

struct WebRTCCallbacks {
    InputHandler* input=nullptr;
    std::function<void(int,uint8_t)> onFpsChange;
    std::function<int()> getHostFps,getMonitor;
    std::function<bool(int)> onMonitorChange;
    std::function<void()> onDisconnect,onConnected;
    std::function<bool(CodecType)> onCodecChange;
    std::function<CodecType()> getCodec;
    std::function<std::string()> getClipboard;
    std::function<bool(const std::string&)> setClipboard;
    std::function<void(bool)> onCursorCapture,onAudioEnable,onMicEnable;
    std::function<void(const uint8_t*,size_t)> onMicData;
};

class WebRTCServer {
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> dcCtrl,dcVid,dcAud,dcIn,dcMic;
    std::atomic<bool> conn{false},needsKey{true},fpsRecv{false},gathered{false},hasDesc{false};
    std::atomic<int> chRdy{0},overflow{0};
    std::atomic<int64_t> lastPing{0};
    std::atomic<uint32_t> frmId{0};
    std::atomic<CodecType> curCodec{CODEC_AV1};
    std::string localDesc;
    std::mutex descMtx,sendMtx;
    std::condition_variable descCv;
    rtc::Configuration cfg;
    WebRTCCallbacks cb;

    static constexpr size_t VID_BUF=262144,AUD_BUF=131072,CHUNK=1400,HDR_SZ=sizeof(PacketHeader);
    static constexpr size_t DATA_CHUNK=CHUNK-HDR_SZ,BUF_LOW=CHUNK*16;
    static constexpr int NUM_CH=5;

    bool ChOpen(const std::shared_ptr<rtc::DataChannel>& ch) const { return ch&&ch->isOpen(); }
    bool AllOpen() const { return ChOpen(dcCtrl)&&ChOpen(dcVid)&&ChOpen(dcAud)&&ChOpen(dcIn)&&ChOpen(dcMic); }

    bool SendCtrl(const void* d,size_t len) {
        if(!ChOpen(dcCtrl)) return false;
        try { dcCtrl->send((const std::byte*)d,len); return true; } catch(...) { return false; }
    }

    void SendHostInfo() {
        uint8_t buf[6]; *(uint32_t*)buf=MSG_HOST_INFO;
        *(uint16_t*)(buf+4)=(uint16_t)(cb.getHostFps?cb.getHostFps():60);
        SendCtrl(buf,sizeof(buf));
    }

    void SendMonitorList() {
        std::vector<uint8_t> buf;
        std::lock_guard<std::mutex> lk(g_monitorsMutex);
        buf.resize(6+g_monitors.size()*74);
        size_t o=0;
        *(uint32_t*)&buf[o]=MSG_MONITOR_LIST; o+=4;
        buf[o++]=(uint8_t)g_monitors.size();
        buf[o++]=(uint8_t)(cb.getMonitor?cb.getMonitor():0);
        for(const auto& m:g_monitors) {
            buf[o++]=(uint8_t)m.index;
            *(uint16_t*)&buf[o]=(uint16_t)m.width; *(uint16_t*)&buf[o+2]=(uint16_t)m.height;
            *(uint16_t*)&buf[o+4]=(uint16_t)m.refreshRate; o+=6;
            buf[o++]=m.isPrimary?1:0;
            size_t nl=std::min(m.name.size(),(size_t)63);
            buf[o++]=(uint8_t)nl; memcpy(&buf[o],m.name.c_str(),nl); o+=nl;
        }
        buf.resize(o);
        SendCtrl(buf.data(),buf.size());
    }

    void HandleCtrl(const rtc::binary& m) {
        if(m.size()<4||chRdy<NUM_CH) return;
        uint32_t magic=*(const uint32_t*)m.data();
        switch(magic) {
            case MSG_PING:
                if(m.size()==16) {
                    lastPing=GetTimestamp()/1000; overflow=0;
                    uint8_t r[24]; memcpy(r,m.data(),16); *(uint64_t*)(r+16)=GetTimestamp();
                    SendCtrl(r,sizeof(r));
                } break;
            case MSG_FPS_SET:
                if(m.size()==7) {
                    uint16_t fps=*(const uint16_t*)((const uint8_t*)m.data()+4);
                    uint8_t mode=(uint8_t)m[6];
                    if(fps>=1&&fps<=240&&mode<=2) {
                        int act=(mode==1&&cb.getHostFps)?cb.getHostFps():fps;
                        fpsRecv=true;
                        if(cb.onFpsChange) cb.onFpsChange(act,mode);
                        uint8_t a[7]; *(uint32_t*)a=MSG_FPS_ACK; *(uint16_t*)(a+4)=(uint16_t)act; a[6]=mode;
                        SendCtrl(a,sizeof(a));
                    }
                } break;
            case MSG_CODEC_SET:
                if(m.size()==5&&(uint8_t)m[4]<=2) {
                    CodecType nc=(CodecType)(uint8_t)m[4];
                    if(!cb.onCodecChange||cb.onCodecChange(nc)) { curCodec=nc; needsKey=true; }
                    uint8_t a[5]; *(uint32_t*)a=MSG_CODEC_ACK; a[4]=(uint8_t)curCodec.load();
                    SendCtrl(a,sizeof(a));
                } break;
            case MSG_REQUEST_KEY: needsKey=true; break;
            case MSG_MONITOR_SET:
                if(m.size()==5&&cb.onMonitorChange&&cb.onMonitorChange((int)(uint8_t)m[4])) {
                    needsKey=true; SendMonitorList(); SendHostInfo();
                } break;
            case MSG_CLIPBOARD_DATA:
                if(m.size()>=8&&cb.setClipboard) {
                    uint32_t len=*(const uint32_t*)((const uint8_t*)m.data()+4);
                    if(len>0&&m.size()>=8+len&&len<=1048576)
                        cb.setClipboard(std::string((const char*)m.data()+8,len));
                } break;
            case MSG_CLIPBOARD_GET:
                if(cb.getClipboard) {
                    std::string text=cb.getClipboard();
                    if(!text.empty()&&text.size()<=1048576) {
                        std::vector<uint8_t> buf(8+text.size());
                        *(uint32_t*)buf.data()=MSG_CLIPBOARD_DATA;
                        *(uint32_t*)(buf.data()+4)=(uint32_t)text.size();
                        memcpy(buf.data()+8,text.data(),text.size());
                        SendCtrl(buf.data(),buf.size());
                    }
                } break;
            case MSG_CURSOR_CAPTURE: if(m.size()==5&&cb.onCursorCapture) cb.onCursorCapture((uint8_t)m[4]!=0); break;
            case MSG_AUDIO_ENABLE: if(m.size()==5&&cb.onAudioEnable) cb.onAudioEnable((uint8_t)m[4]!=0); break;
            case MSG_MIC_ENABLE: if(m.size()==5&&cb.onMicEnable) cb.onMicEnable((uint8_t)m[4]!=0); break;
        }
    }

    void HandleInput(const rtc::binary& m) {
        if(m.size()<4||chRdy<NUM_CH||!cb.input) return;
        cb.input->HandleMessage((const uint8_t*)m.data(),m.size());
    }

    void HandleMic(const rtc::binary& m) {
        if(m.size()<sizeof(MicPacketHeader)||chRdy<NUM_CH) return;
        if(*(const uint32_t*)m.data()==MSG_MIC_DATA&&cb.onMicData)
            cb.onMicData((const uint8_t*)m.data(),m.size());
    }

    void OnChOpen() {
        if(++chRdy==NUM_CH) {
            conn=true; needsKey=true; lastPing=GetTimestamp()/1000; overflow=0;
            SendHostInfo(); SendMonitorList();
            if(cb.onConnected) cb.onConnected();
        }
    }

    void OnChClose() {
        conn=false; fpsRecv=false; chRdy=0; overflow=0;
        if(cb.onDisconnect) cb.onDisconnect();
    }

    void SetupCh(std::shared_ptr<rtc::DataChannel>& ch,bool drain,bool msg,std::function<void(const rtc::binary&)> h=nullptr) {
        if(!ch) return;
        ch->setBufferedAmountLowThreshold(BUF_LOW);
        ch->onOpen([this] { OnChOpen(); });
        ch->onClosed([this] { OnChClose(); });
        if(msg&&h) ch->onMessage([this,h](auto d) { if(auto* b=std::get_if<rtc::binary>(&d)) h(*b); });
        if(drain) ch->onBufferedAmountLow([this,&ch] { if(&ch==&dcVid) DrainVid(); else if(&ch==&dcAud) DrainAud(); });
    }

    void DrainVid() {
        if(!ChOpen(dcVid)) return;
        std::lock_guard<std::mutex> lk(sendMtx);
        while(!vidQ.empty()&&dcVid->bufferedAmount()<=VID_BUF) {
            try { dcVid->send((const std::byte*)vidQ.front().data(),vidQ.front().size()); } catch(...) { overflow++; needsKey=true; }
            vidQ.pop();
        }
    }

    void DrainAud() {
        if(!ChOpen(dcAud)) return;
        std::lock_guard<std::mutex> lk(sendMtx);
        while(!audQ.empty()&&dcAud->bufferedAmount()<=AUD_BUF) {
            try { dcAud->send((const std::byte*)audQ.front().data(),audQ.front().size()); } catch(...) {}
            audQ.pop();
        }
    }

    std::queue<std::vector<uint8_t>> vidQ,audQ;

    void Reset() {
        auto close=[](auto& ch) { try { if(ch&&ch->isOpen()) ch->close(); } catch(...) {} ch.reset(); };
        close(dcCtrl); close(dcVid); close(dcAud); close(dcIn); close(dcMic);
        try { if(pc) pc->close(); } catch(...) {}
        conn=false; fpsRecv=false; gathered=false; hasDesc=false;
        chRdy=0; overflow=0; lastPing=0;
        { std::lock_guard<std::mutex> lk(descMtx); localDesc.clear(); }
        { std::lock_guard<std::mutex> lk(sendMtx); while(!vidQ.empty()) vidQ.pop(); while(!audQ.empty()) audQ.pop(); }
    }

    void SetupPC() {
        if(pc&&ChOpen(dcCtrl)) {
            try { uint8_t k[4]; *(uint32_t*)k=MSG_KICKED; dcCtrl->send((const std::byte*)k,4); std::this_thread::sleep_for(50ms); } catch(...) {}
        }
        Reset();
        needsKey=true;

        pc=std::make_shared<rtc::PeerConnection>(cfg);
        pc->onLocalDescription([this](rtc::Description d) {
            std::lock_guard<std::mutex> lk(descMtx);
            localDesc=std::string(d); hasDesc=true; descCv.notify_all();
        });
        pc->onLocalCandidate([this](rtc::Candidate) { descCv.notify_all(); });
        pc->onStateChange([this](auto s) {
            bool now=s==rtc::PeerConnection::State::Connected;
            if(now&&!conn) { needsKey=true; lastPing=GetTimestamp()/1000; }
            if(!now&&conn) { fpsRecv=false; chRdy=0; if(cb.onDisconnect) cb.onDisconnect(); }
            conn=now;
        });
        pc->onGatheringStateChange([this](auto s) {
            if(s==rtc::PeerConnection::GatheringState::Complete) { gathered=true; descCv.notify_all(); }
        });
        pc->onDataChannel([this](auto ch) {
            std::string l=ch->label();
            if(l=="control") { dcCtrl=ch; SetupCh(dcCtrl,false,true,[this](auto& m) { HandleCtrl(m); }); }
            else if(l=="video") { dcVid=ch; SetupCh(dcVid,true,false); }
            else if(l=="audio") { dcAud=ch; SetupCh(dcAud,true,false); }
            else if(l=="input") { dcIn=ch; SetupCh(dcIn,false,true,[this](auto& m) { HandleInput(m); }); }
            else if(l=="mic") { dcMic=ch; SetupCh(dcMic,false,true,[this](auto& m) { HandleMic(m); }); }
        });
    }

    bool IsStale() {
        if(!conn) return false;
        int64_t now=GetTimestamp()/1000;
        return (lastPing>0&&(now-lastPing)>3000)||overflow>=10;
    }

public:
    WebRTCServer() {
        cfg.iceServers.push_back(rtc::IceServer("stun:stun.l.google.com:19302"));
        cfg.portRangeBegin=50000; cfg.portRangeEnd=50020; cfg.enableIceTcp=false;
        SetupPC();
    }

    void Init(WebRTCCallbacks c) { cb=std::move(c); }

    std::string GetLocal() {
        std::unique_lock<std::mutex> lk(descMtx);
        if(!descCv.wait_for(lk,200ms,[this] { return hasDesc.load(); })) return localDesc;
        descCv.wait_for(lk,150ms,[this] { return gathered.load(); });
        return localDesc;
    }

    void SetRemote(const std::string& sdp,const std::string& type) {
        if(type=="offer") SetupPC();
        pc->setRemoteDescription(rtc::Description(sdp,type));
        if(type=="offer") pc->setLocalDescription();
    }

    bool IsStreaming() const { return conn&&fpsRecv&&chRdy==NUM_CH; }
    bool NeedsKey() { return needsKey.exchange(false); }

    bool SendCursorShape(CursorType ct) {
        if(!IsStreaming()) return false;
        uint8_t buf[5]; *(uint32_t*)buf=MSG_CURSOR_SHAPE; buf[4]=(uint8_t)ct;
        return SendCtrl(buf,sizeof(buf));
    }

    bool Send(const EncodedFrame& f) {
        if(!IsStreaming()) return false;
        if(IsStale()) { Reset(); if(cb.onDisconnect) cb.onDisconnect(); return false; }
        size_t sz=f.data.size(),nch=(sz+DATA_CHUNK-1)/DATA_CHUNK;
        if(!sz||nch>65535) return false;

        uint32_t fid=frmId++;
        PacketHeader h={f.ts,(uint32_t)f.encUs,fid,0,(uint16_t)nch,f.isKey?(uint8_t)1:(uint8_t)0};

        { std::lock_guard<std::mutex> lk(sendMtx);
          while(vidQ.size()>nch*3) { vidQ.pop(); needsKey=true; }
          for(size_t i=0;i<nch;i++) {
              h.chunkIndex=(uint16_t)i;
              size_t off=i*DATA_CHUNK,len=std::min(DATA_CHUNK,sz-off);
              std::vector<uint8_t> pkt(HDR_SZ+len);
              memcpy(pkt.data(),&h,HDR_SZ);
              memcpy(pkt.data()+HDR_SZ,f.data.data()+off,len);
              vidQ.push(std::move(pkt));
          }
        }
        DrainVid();
        return true;
    }

    bool SendAudio(const std::vector<uint8_t>& data,int64_t ts,int samples) {
        if(!IsStreaming()||data.empty()||data.size()>4000) return false;

        std::vector<uint8_t> pkt(sizeof(AudioPacketHeader)+data.size());
        auto* h=(AudioPacketHeader*)pkt.data();
        h->magic=MSG_AUDIO_DATA; h->timestamp=ts; h->samples=(uint16_t)samples; h->dataLength=(uint16_t)data.size();
        memcpy(pkt.data()+sizeof(AudioPacketHeader),data.data(),data.size());

        if(ChOpen(dcAud)&&dcAud->bufferedAmount()<=AUD_BUF/2) {
            try { dcAud->send((const std::byte*)pkt.data(),pkt.size()); return true; } catch(...) {}
        }

        std::lock_guard<std::mutex> lk(sendMtx);
        while(audQ.size()>=8) audQ.pop();
        audQ.push(std::move(pkt));
        DrainAud();
        return true;
    }
};
