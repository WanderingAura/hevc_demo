// libx265 (via libavcodec) Encoder port. av_encode() is the synchronous core,
// AvEncoder runs it on a worker thread and posts Encoded events back.
#pragma once
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "server/core.hpp"

namespace hs {

struct AvEnc;  // opaque libavcodec state

AvEnc* av_encoder_open(const EncodeParams& p);
void av_encoder_close(AvEnc* e);

struct EncodeOut {
    bool ok;
    bool key;
    uint32_t len;
    int packets;  // must be 1: zerolatency, frame-threads=1
};

// The encoder's input picture, writable; fill it, then call av_encode.
YuvView av_encoder_picture(AvEnc* e);
// Encodes the input picture and copies the packet into `out`.
EncodeOut av_encode(AvEnc* e, const EncodeJob& job, MutBytes out);

class AvEncoder final : public Encoder {
public:
    using Post = std::function<void(const ServerEvent&)>;  // must be thread-safe
    // Opens the encoder with `initial` up front, so the first frame isn't slow.
    AvEncoder(FrameSource& src, Clock& clock, const EncodeParams& initial, Post post);
    ~AvEncoder() override;
    void submit(const EncodeJob& job, MutBytes out) override;

private:
    void run();

    FrameSource& src_;
    Clock& clock_;
    Post post_;
    EncodeParams initial_;
    AvEnc* enc_ = nullptr;
    std::mutex mu_;
    std::condition_variable cv_;
    bool has_job_ = false;
    bool quit_ = false;
    EncodeJob job_{};
    MutBytes out_{};
    std::thread worker_;
};

}  // namespace hs
