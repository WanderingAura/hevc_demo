#include "client/decoder_av.hpp"

#include <cstdio>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace hs {

struct AvDec {
    AVCodecContext* ctx;
    AVPacket* pkt;
    AVFrame* scratch;
    AVFrame* picture;  // newest decoded picture
};

AvDec* av_decoder_open(int threads) {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    AVCodecContext* c = avcodec_alloc_context3(codec);
    c->flags |= AV_CODEC_FLAG_LOW_DELAY;
    c->thread_type = FF_THREAD_SLICE;  // never FF_THREAD_FRAME: it adds frames of delay
    c->thread_count = threads;
    if (avcodec_open2(c, codec, nullptr) < 0) {
        std::fprintf(stderr, "decoder: avcodec_open2 failed\n");
        avcodec_free_context(&c);
        return nullptr;
    }
    return new AvDec{c, av_packet_alloc(), av_frame_alloc(), av_frame_alloc()};
}

void av_decoder_close(AvDec* d) {
    if (!d) return;
    avcodec_free_context(&d->ctx);
    av_packet_free(&d->pkt);
    av_frame_free(&d->scratch);
    av_frame_free(&d->picture);
    delete d;
}

void av_decoder_flush(AvDec* d) { avcodec_flush_buffers(d->ctx); }

DecodeOut av_decode(AvDec* d, Bytes au, uint32_t seq) {
    DecodeOut r{};
    // Not refcounted, so libavcodec copies it into a padded buffer.
    d->pkt->data = const_cast<uint8_t*>(au.p);
    d->pkt->size = int(au.n);
    d->pkt->pts = seq;
    int err = avcodec_send_packet(d->ctx, d->pkt);
    d->pkt->data = nullptr;
    d->pkt->size = 0;
    if (err < 0) return r;
    while (avcodec_receive_frame(d->ctx, d->scratch) == 0) {
        av_frame_unref(d->picture);
        av_frame_move_ref(d->picture, d->scratch);
        r.frames++;
    }
    r.ok = r.frames > 0 && !(d->picture->flags & AV_FRAME_FLAG_CORRUPT) && !d->picture->decode_error_flags;
    return r;
}

AVFrame* av_decoder_picture(AvDec* d) { return d->picture; }

FrameMailbox::FrameMailbox() : latest_(av_frame_alloc()) {}
FrameMailbox::~FrameMailbox() { av_frame_free(&latest_); }

void FrameMailbox::put(AVFrame* src, uint32_t seq) {
    std::lock_guard<std::mutex> lk(mu_);
    av_frame_unref(latest_);
    av_frame_move_ref(latest_, src);
    seq_ = seq;
    fresh_ = true;
}

bool FrameMailbox::take(AVFrame* dst, uint32_t& seq) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!fresh_) return false;
    av_frame_unref(dst);
    av_frame_move_ref(dst, latest_);
    seq = seq_;
    fresh_ = false;
    return true;
}

AvDecoder::AvDecoder(int threads, Clock& clock, Post post, FrameMailbox* mailbox)
    : clock_(clock), post_(std::move(post)), mailbox_(mailbox), dec_(av_decoder_open(threads)),
      worker_([this] { run(); }) {}

AvDecoder::~AvDecoder() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        quit_ = true;
    }
    cv_.notify_one();
    worker_.join();
    av_decoder_close(dec_);
}

void AvDecoder::submit(const DecodeJob& job, Bytes au) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        assert(!has_job_);  // the core keeps one decode in flight
        job_ = job;
        au_ = au;
        has_job_ = true;
    }
    cv_.notify_one();
}

// Runs on the io thread between Decoded and the next submit, while the worker is idle.
void AvDecoder::present(uint32_t seq) {
    if (mailbox_) {
        mailbox_->put(av_decoder_picture(dec_), seq);
        return;
    }
    av_frame_unref(av_decoder_picture(dec_));
    ClientEvent ev = cli_event(ClientEvent::Displayed, clock_.now());
    ev.displayed.seq = seq;
    post_(ev);
}

void AvDecoder::run() {
    for (;;) {
        DecodeJob job;
        Bytes au;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return has_job_ || quit_; });
            if (quit_) return;
            job = job_;
            au = au_;
            has_job_ = false;
        }
        if (dec_ && job.flush_before) av_decoder_flush(dec_);
        Time start = clock_.now();
        DecodeOut r = dec_ ? av_decode(dec_, au, job.seq) : DecodeOut{};
        Time end = clock_.now();
        ClientEvent ev = cli_event(ClientEvent::Decoded, end);
        ev.decoded = DecodedEv{job.seq, job.slot, r.ok, start.wall_ns, end.wall_ns};
        post_(ev);
    }
}

}  // namespace hs
