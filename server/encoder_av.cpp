#include "server/encoder_av.hpp"

#include <cstdio>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

namespace hs {

struct AvEnc {
    AVCodecContext* ctx;
    AVFrame* frame;
    AVPacket* pkt;
};

AvEnc* av_encoder_open(const EncodeParams& p) {
    const AVCodec* codec = avcodec_find_encoder_by_name("libx265");
    if (!codec) {
        std::fprintf(stderr, "encoder: libx265 not available in this FFmpeg build\n");
        return nullptr;
    }
    AVCodecContext* c = avcodec_alloc_context3(codec);
    c->width = p.width;
    c->height = p.height;
    c->pix_fmt = AV_PIX_FMT_YUV420P;
    c->time_base = AVRational{1, p.fps};
    c->framerate = AVRational{p.fps, 1};
    c->gop_size = p.gop;
    c->max_b_frames = 0;
    c->bit_rate = int64_t(p.bitrate_kbps) * 1000;
    c->rc_max_rate = c->bit_rate;                                  // CBR-ish, like a camera
    c->rc_buffer_size = int(int64_t(p.bitrate_kbps) * p.vbv_ms);  // bits
    // No AV_CODEC_FLAG_GLOBAL_HEADER: VPS/SPS/PPS stay in-band.
    av_opt_set(c->priv_data, "preset", p.preset, 0);
    av_opt_set(c->priv_data, "tune", "zerolatency", 0);
    av_opt_set_int(c->priv_data, "forced-idr", 1, 0);  // pict_type I => IDR, not CRA
    std::string x265 = "repeat-headers=1:open-gop=0:frame-threads=1:bframes=0:rc-lookahead=0:scenecut=0:log-level=error:keyint=" +
                       std::to_string(p.gop) + ":min-keyint=" + std::to_string(p.gop);
    av_opt_set(c->priv_data, "x265-params", x265.c_str(), 0);
    if (avcodec_open2(c, codec, nullptr) < 0) {
        std::fprintf(stderr, "encoder: avcodec_open2 failed\n");
        avcodec_free_context(&c);
        return nullptr;
    }
    AvEnc* e = new AvEnc{c, av_frame_alloc(), av_packet_alloc()};
    e->frame->format = AV_PIX_FMT_YUV420P;
    e->frame->width = p.width;
    e->frame->height = p.height;
    av_frame_get_buffer(e->frame, 0);
    return e;
}

void av_encoder_close(AvEnc* e) {
    if (!e) return;
    avcodec_free_context(&e->ctx);
    av_frame_free(&e->frame);
    av_packet_free(&e->pkt);
    delete e;
}

YuvView av_encoder_picture(AvEnc* e) {
    AVFrame* f = e->frame;
    av_frame_make_writable(f);  // no-op unless the encoder still holds a reference
    return YuvView{{f->data[0], f->data[1], f->data[2]}, {f->linesize[0], f->linesize[1], f->linesize[2]}, f->width,
                   f->height};
}

EncodeOut av_encode(AvEnc* e, const EncodeJob& job, MutBytes out) {
    EncodeOut r{};
    AVFrame* f = e->frame;
    f->pts = job.seq;
    f->pict_type = job.force_idr ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    if (avcodec_send_frame(e->ctx, f) < 0) return r;
    r.ok = true;
    while (avcodec_receive_packet(e->ctx, e->pkt) == 0) {
        r.packets++;
        if (r.len + uint32_t(e->pkt->size) > out.n) {
            r.ok = false;  // larger than transport.max_frame_bytes
        } else {
            std::memcpy(out.p + r.len, e->pkt->data, size_t(e->pkt->size));
            r.len += uint32_t(e->pkt->size);
        }
        r.key = r.key || (e->pkt->flags & AV_PKT_FLAG_KEY);
        av_packet_unref(e->pkt);
    }
    // One frame in, one packet out. Anything else means the encoder is buffering.
    assert(r.packets == 1);
    if (r.packets != 1) r.ok = false;
    return r;
}

AvEncoder::AvEncoder(FrameSource& src, Clock& clock, const EncodeParams& initial, Post post)
    : src_(src), clock_(clock), post_(std::move(post)), initial_(initial), worker_([this] { run(); }) {}

AvEncoder::~AvEncoder() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        quit_ = true;
    }
    cv_.notify_one();
    worker_.join();
    av_encoder_close(enc_);
}

void AvEncoder::submit(const EncodeJob& job, MutBytes out) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        assert(!has_job_);  // the core keeps one encode in flight
        job_ = job;
        out_ = out;
        has_job_ = true;
    }
    cv_.notify_one();
}

void AvEncoder::run() {
    enc_ = av_encoder_open(initial_);
    for (;;) {
        EncodeJob job;
        MutBytes out;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return has_job_ || quit_; });
            if (quit_) return;
            job = job_;
            out = out_;
            has_job_ = false;
        }
        if (!enc_ || job.reopen) {
            av_encoder_close(enc_);
            enc_ = av_encoder_open(job.params);
        }
        EncodeOut r{};
        Time t_in = clock_.now();
        if (enc_) {
            src_.fill(av_encoder_picture(enc_), job.capture_index, job.capture_wall);
            t_in = clock_.now();  // rendering counts as ISP time, not encode time
            r = av_encode(enc_, job, out);
        }
        Time t_out = clock_.now();

        ServerEvent ev = srv_event(ServerEvent::Encoded, t_out);
        ev.encoded = EncodedEv{job.seq, job.slot, r.len, r.key, r.ok, t_in.wall_ns, t_out.wall_ns,
                               t_out.mono_ns - t_in.mono_ns};
        post_(ev);
    }
}

}  // namespace hs
