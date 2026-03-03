#pragma once

#include <cstdint>
#include <mutex>
#include <queue>
#include <utility>
#include <windows.h>

template<typename... T>
void SafeRelease(T*&... p) {
    ((p ? (p->Release(), p = nullptr) : nullptr), ...);
}

template<typename Q, typename M>
void ClearQueue(Q& q, M& mtx) {
    std::lock_guard<M> lk(mtx);
    while (!q.empty()) q.pop();
}

template<typename Q, typename V>
void PushBoundedQueue(Q& q, size_t maxSize, V&& value) {
    if (q.size() >= maxSize) q.pop();
    q.push(std::forward<V>(value));
}

inline int64_t GetTimestamp() {
    static const int64_t frequency = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return static_cast<int64_t>(f.QuadPart > 0 ? f.QuadPart : 1);
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<int64_t>((counter.QuadPart * 1000000LL) / frequency);
}
