#pragma once

#include "host/core/utils.hpp"

#include <d3d11_4.h>
#include <windows.h>

struct MTLock {
    ID3D11Multithread* m;
    MTLock(ID3D11Multithread* mt) : m(mt) { if (m) m->Enter(); }
    ~MTLock() { if (m) m->Leave(); }
    MTLock(const MTLock&) = delete;
    MTLock& operator=(const MTLock&) = delete;
};

class D3D11FenceSync {
    ID3D11Device5* d5 = nullptr;
    ID3D11DeviceContext4* c4 = nullptr;
    ID3D11Fence* f = nullptr;
    HANDLE evt = nullptr;
    uint64_t val = 0, lastSig = 0;
    bool active = false;
public:
    D3D11FenceSync() = default;
    D3D11FenceSync(const D3D11FenceSync&) = delete;
    D3D11FenceSync& operator=(const D3D11FenceSync&) = delete;
    ~D3D11FenceSync() { if (evt) CloseHandle(evt); SafeRelease(f, c4, d5); }

    bool Init(ID3D11Device* d, ID3D11DeviceContext* c, D3D11_FENCE_FLAG flags = D3D11_FENCE_FLAG_NONE) {
        if (FAILED(d->QueryInterface(IID_PPV_ARGS(&d5)))) return true;
        if (FAILED(c->QueryInterface(IID_PPV_ARGS(&c4)))) { SafeRelease(d5); return true; }
        if (FAILED(d5->CreateFence(0, flags, IID_PPV_ARGS(&f)))) { SafeRelease(d5, c4); return true; }
        evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!evt) { SafeRelease(f, c4, d5); return false; }
        active = true;
        return true;
    }

    uint64_t Signal() {
        if (active && c4 && f && SUCCEEDED(c4->Signal(f, ++val))) { lastSig = val; return val; }
        return 0;
    }

    bool Wait(uint64_t v, ID3D11DeviceContext* ctx = nullptr, ID3D11Multithread* mt = nullptr, DWORD ms = 16) {
        if (active && f && evt) {
            if (f->GetCompletedValue() >= v) return true;
            if (FAILED(f->SetEventOnCompletion(v, evt))) return false;
            return WaitForSingleObject(evt, ms) == WAIT_OBJECT_0 || f->GetCompletedValue() >= v;
        }
        if (ctx) { if (mt) { MTLock lk(mt); ctx->Flush(); } else ctx->Flush(); }
        return true;
    }

    [[nodiscard]] bool Complete(uint64_t v) const { return !active || !f || f->GetCompletedValue() >= v; }
    [[nodiscard]] bool IsLastComplete() const { return !active || !f || f->GetCompletedValue() >= lastSig; }
};
