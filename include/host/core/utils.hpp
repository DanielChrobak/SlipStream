#pragma once

#include <cstddef>
#include <cstring>
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

template<typename T>
void WritePod(uint8_t* dst, const T& value) {
    std::memcpy(dst, &value, sizeof(T));
}

template<typename T>
[[nodiscard]] T ReadPod(const uint8_t* src) {
    T value{};
    std::memcpy(&value, src, sizeof(T));
    return value;
}

template<typename T>
[[nodiscard]] bool ReadPod(const uint8_t* data, size_t len, T& out) {
    if (len < sizeof(T)) return false;
    out = ReadPod<T>(data);
    return true;
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
