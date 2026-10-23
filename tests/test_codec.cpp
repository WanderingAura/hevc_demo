// Integration test against the real libx265 encoder and libavcodec decoder.
#include <gtest/gtest.h>

#include "client/decoder_av.hpp"
#include "common/nal.hpp"
#include "server/encoder_av.hpp"
#include "server/source.hpp"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/log.h>
}

using namespace hs;

TEST(Codec, OneInOneOutForcedIdrAndDecodeAcrossFlush) {
    av_log_set_level(AV_LOG_ERROR);
    Config cfg;
    cfg.source.width = 320;
    cfg.source.height = 180;
    cfg.encoder.bitrate_kbps = 500;
    cfg.encoder.gop = 8;
    EncodeParams p = encode_params(cfg);
    AvEnc* enc = av_encoder_open(p);
    ASSERT_TRUE(enc);
    SyntheticSource src;

    std::vector<std::vector<uint8_t>> aus;
    std::vector<uint8_t> buf(1 << 20);
    int keys = 0;
    for (uint32_t i = 0; i < 24; ++i) {
        EncodeJob job{};
        job.seq = i;
        job.capture_index = i;
        job.force_idr = i == 6;
        job.params = p;
        src.fill(av_encoder_picture(enc), i, int64_t(i) * 33 * kMs);
        EncodeOut r = av_encode(enc, job, MutBytes{buf.data(), uint32_t(buf.size())});
        ASSERT_TRUE(r.ok) << "frame " << i;
        EXPECT_EQ(r.packets, 1) << "encoder is buffering at frame " << i;
        if (i == 0 || i == 6) {
            EXPECT_TRUE(r.key) << "frame " << i;
        }
        Bytes au{buf.data(), r.len};
        // Every keyframe, forced or periodic, must be a real IDR (19/20) with in-band
        // VPS/SPS/PPS: open-gop would make them CRA, which the client can't join at.
        EXPECT_EQ(is_valid_idr(au), r.key) << "frame " << i;
        if (!r.key) {
            EXPECT_TRUE(nal_types(au) & (uint64_t(1) << kNalTrailR));
        }
        keys += r.key;
        aus.emplace_back(buf.begin(), buf.begin() + r.len);
    }
    av_encoder_close(enc);
    EXPECT_GE(keys, 4);  // 0, forced 6, and periodic ones every 8

    AvDec* dec = av_decoder_open(2);
    ASSERT_TRUE(dec);
    for (uint32_t i = 0; i < 6; ++i) {
        DecodeOut r = av_decode(dec, Bytes{aus[i].data(), uint32_t(aus[i].size())}, i);
        EXPECT_TRUE(r.ok) << "frame " << i;
        EXPECT_EQ(r.frames, 1) << "decoder is buffering at frame " << i;
        EXPECT_EQ(av_decoder_picture(dec)->pts, int64_t(i));
        EXPECT_EQ(av_decoder_picture(dec)->width, 320);
    }
    // Recovery path: flush, then resume at the forced IDR.
    av_decoder_flush(dec);
    for (uint32_t i = 6; i < 12; ++i) {
        DecodeOut r = av_decode(dec, Bytes{aus[i].data(), uint32_t(aus[i].size())}, i);
        EXPECT_TRUE(r.ok) << "frame " << i;
        EXPECT_EQ(av_decoder_picture(dec)->pts, int64_t(i));
    }
    av_decoder_close(dec);
}
