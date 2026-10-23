// Plain value types shared by every module. No I/O, no allocation after setup.
#pragma once
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "wire format assumes a little-endian host");

namespace hs {

constexpr int64_t kUs = 1'000;
constexpr int64_t kMs = 1'000'000;
constexpr int64_t kSec = 1'000'000'000;

// Every event carries both clocks, stamped by the port that produced it.
// wall_ns: CLOCK_REALTIME, for timestamps that cross the network.
// mono_ns: CLOCK_MONOTONIC, for local durations and timers.
struct Time {
    int64_t mono_ns;
    int64_t wall_ns;
};

struct SyncStatus {
    bool synced;
    int64_t est_error_ns;
    int64_t max_error_ns;
};

// Timer event payload for both sides. The interpreter fills `sync` from the
// Clock port, so the cores see clock-sync state without reading a clock.
struct TimerEv {
    uint8_t id;
    SyncStatus sync;
};

struct ArmTimerCmd {
    uint8_t id;
    int64_t at_mono;
};

struct Bytes {
    const uint8_t* p;
    uint32_t n;
};

struct MutBytes {
    uint8_t* p;
    uint32_t n;
};

enum class Err : uint8_t {
    Ok,
    BadMagic,
    BadVersion,
    BadLength,
    UnknownKey,
    NotLive,
    BadValue,
    ParseError,
    Io,
};

const char* err_name(Err e);

template <class T>
struct Result {
    Err err;
    T value;
};

// Fixed-capacity vector for small per-step outputs (commands, drops).
template <class T, uint32_t N>
struct FixedVec {
    T items[N];
    uint32_t n = 0;

    void push(const T& v) {
        assert(n < N && "FixedVec full: raise its capacity");
        if (n < N) items[n++] = v;  // never write out of bounds, even with NDEBUG
    }
    bool full() const { return n == N; }
    const T* begin() const { return items; }
    const T* end() const { return items + n; }
    T& operator[](uint32_t i) { return items[i]; }
    const T& operator[](uint32_t i) const { return items[i]; }
};

// FIFO ring with capacity fixed at init. Never reallocates.
template <class T>
struct Ring {
    std::vector<T> buf;
    uint32_t head = 0;
    uint32_t count = 0;

    void init(uint32_t capacity) {
        buf.assign(capacity, T{});
        head = count = 0;
    }
    uint32_t capacity() const { return uint32_t(buf.size()); }
    uint32_t size() const { return count; }
    bool empty() const { return count == 0; }
    bool full() const { return count == buf.size(); }
    void clear() { head = count = 0; }
    T& front() { return buf[head]; }
    T& at(uint32_t i) { return buf[(head + i) % buf.size()]; }
    const T& at(uint32_t i) const { return buf[(head + i) % buf.size()]; }
    void push(const T& v) {
        assert(!full());
        buf[(head + count) % buf.size()] = v;
        ++count;
    }
    T pop() {
        assert(!empty());
        T v = buf[head];
        head = (head + 1) % buf.size();
        --count;
        return v;
    }
};

// splitmix64: tiny, seedable, identical on every platform.
struct Rng {
    uint64_t s;
};

inline uint64_t rng_next(Rng& r) {
    uint64_t z = (r.s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// Uniform in [-span, +span].
inline int64_t rng_jitter(Rng& r, int64_t span) {
    if (span <= 0) return 0;
    return int64_t(rng_next(r) % uint64_t(2 * span + 1)) - span;
}

template <size_t N>
void copy_cstr(char (&dst)[N], const char* src) {
    size_t i = 0;
    for (; i + 1 < N && src[i]; ++i) dst[i] = src[i];
    for (; i < N; ++i) dst[i] = 0;
}

}  // namespace hs
