// libavcodec HEVC Decoder port. av_decode() is the synchronous core; the
// AvDecoder port (client side) runs it on a worker thread.
#pragma once
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "client/core.hpp"

struct AVFrame;

namespace hs {

struct AvDec;  // opaque libavcodec state

AvDec* av_decoder_open(int threads);
void av_decoder_close(AvDec* d);
void av_decoder_flush(AvDec* d);

struct DecodeOut {
    bool ok;
    int frames;  // pictures produced by this access unit (1 with LOW_DELAY)
};

// Decodes one access unit straight from our framing (no parser/demuxer). The
// newest picture is kept in av_decoder_picture() until the next call.
DecodeOut av_decode(AvDec* d, Bytes au, uint32_t seq);
AVFrame* av_decoder_picture(AvDec* d);

// Single-slot "latest frame" handoff to the UI thread: a newer frame replaces
// an older one that was never picked up.
class FrameMailbox {
public:
    FrameMailbox();
    ~FrameMailbox();
    void put(AVFrame* src, uint32_t seq);   // takes src's reference
    bool take(AVFrame* dst, uint32_t& seq);  // moves the newest frame out, if any

private:
    std::mutex mu_;
    AVFrame* latest_;
    uint32_t seq_ = 0;
    bool fresh_ = false;
};

class AvDecoder final : public Decoder {
public:
    using Post = std::function<void(const ClientEvent&)>;  // must be thread-safe
    // mailbox == nullptr: headless, frames count as displayed as soon as presented.
    AvDecoder(int threads, Clock& clock, Post post, FrameMailbox* mailbox);
    ~AvDecoder() override;
    void submit(const DecodeJob& job, Bytes au) override;
    void present(uint32_t seq) override;

private:
    void run();

    Clock& clock_;
    Post post_;
    FrameMailbox* mailbox_;
    AvDec* dec_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool has_job_ = false;
    bool quit_ = false;
    DecodeJob job_{};
    Bytes au_{};
    std::thread worker_;
};

}  // namespace hs
