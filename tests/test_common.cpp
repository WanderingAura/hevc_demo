#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <sstream>

#include "common/config.hpp"
#include "common/log.hpp"
#include "common/nal.hpp"
#include "common/wire.hpp"

using namespace hs;

TEST(Wire, HeaderRoundTrip) {
    FrameHeader h{};
    h.magic = kFrameMagic;
    h.version = kWireVersion;
    h.header_len = kHeaderLen;
    h.seq = 42;
    h.gop_id = 7;
    h.frame_type = 1;
    h.flags = kFlagForcedIdr;
    h.capture_ts_ns = 123456789;
    h.payload_len = 1000;
    uint8_t buf[kHeaderLen];
    encode_header(h, buf);
    EXPECT_EQ(std::memcmp(buf, "HSIM", 4), 0);
    patch_send_ts(buf, 987654321);
    EXPECT_EQ(read_send_ts(buf), 987654321);

    Result<FrameHeader> r = decode_header(buf, 2000);
    ASSERT_EQ(r.err, Err::Ok);
    EXPECT_EQ(r.value.seq, 42u);
    EXPECT_EQ(r.value.capture_ts_ns, 123456789);
    EXPECT_EQ(r.value.send_ts_ns, 987654321);
    EXPECT_EQ(decode_header(buf, 999).err, Err::BadLength);
    buf[0] = 'X';
    EXPECT_EQ(decode_header(buf, 2000).err, Err::BadMagic);
}

TEST(Wire, CtrlStreamByteAtATime) {
    uint8_t stream[3 * kCtrlMaxBytes];
    uint32_t n = 0;
    n += encode_ctrl(make_ping(5, 111), stream + n);
    n += encode_ctrl(make_keyframe_req(KfReason::Gap, 99), stream + n);
    n += encode_ctrl(make_config_set("encoder.gop", "30"), stream + n);

    CtrlReader r{};
    CtrlMsg got[3];
    int count = 0;
    for (uint32_t i = 0; i < n; ++i) {
        CtrlFeed f = ctrl_feed(r, Bytes{stream + i, 1});
        ASSERT_EQ(f.err, Err::Ok);
        if (f.done) got[count++] = f.msg;
    }
    ASSERT_EQ(count, 3);
    EXPECT_EQ(got[0].type, CtrlType::Ping);
    EXPECT_EQ(got[0].ping.client_tx_ns, 111);
    EXPECT_EQ(got[1].kf.last_good_seq, 99u);
    EXPECT_STREQ(got[2].set.key, "encoder.gop");
    EXPECT_STREQ(got[2].set.value, "30");
}

TEST(Wire, CtrlStreamManyPerChunkAndGarbage) {
    uint8_t stream[2 * kCtrlMaxBytes];
    uint32_t n = encode_ctrl(make_ping(1, 1), stream);
    n += encode_ctrl(make_ping(2, 2), stream + n);
    CtrlReader r{};
    Bytes in{stream, n};
    CtrlFeed f = ctrl_feed(r, in);
    ASSERT_TRUE(f.done);
    EXPECT_EQ(f.msg.ping.nonce, 1u);
    f = ctrl_feed(r, Bytes{stream + f.consumed, n - f.consumed});
    ASSERT_TRUE(f.done);
    EXPECT_EQ(f.msg.ping.nonce, 2u);

    uint8_t junk[16] = {1, 2, 3, 4, 5, 6, 7, 8};
    CtrlReader r2{};
    EXPECT_EQ(ctrl_feed(r2, Bytes{junk, 16}).err, Err::BadMagic);
}

TEST(Config, DefaultsMatchPlan) {
    Config c;
    EXPECT_EQ(c.source.width, 1920);
    EXPECT_EQ(c.source.height, 1080);
    EXPECT_EQ(c.source.fps, 30);
    EXPECT_EQ(c.client.t_stale_ms, 400);
    EXPECT_EQ(c.client.t_hard_ms, 500);
    EXPECT_EQ(validate(c), Err::Ok);
}

TEST(Config, IniParsing) {
    Config c;
    const char* text =
        "; comment\n"
        "[source]\n"
        "fps = 25   ; trailing comment\n"
        "[server.queue]\n"
        "max_frames=5\n"
        "[client]\r\n"
        "vsync = yes\r\n"
        "csv = \"out.csv\"\n"
        "[clock]\n"
        "sync_margin_ms = auto\n";
    IniError e = load_ini(c, text);
    ASSERT_EQ(e.err, Err::Ok) << e.line;
    EXPECT_EQ(c.source.fps, 25);
    EXPECT_EQ(c.server.queue.max_frames, 5);
    EXPECT_TRUE(c.client.vsync);
    EXPECT_EQ(c.client.csv, "out.csv");
    EXPECT_EQ(c.clock.sync_margin_ms, -1);

    EXPECT_EQ(load_ini(c, "[source]\nbogus = 1\n").err, Err::UnknownKey);
    EXPECT_EQ(load_ini(c, "[source]\nfps = 0\n").line, 2);
    EXPECT_EQ(load_ini(c, "[source\n").err, Err::ParseError);
    EXPECT_EQ(load_ini(c, "fps\n").err, Err::ParseError);
}

TEST(Config, ShippedIniLoads) {
    std::ifstream f(H265_SOURCE_DIR "/config/h265sim.ini");
    ASSERT_TRUE(f.good());
    std::stringstream ss;
    ss << f.rdbuf();
    Config c;
    IniError e = load_ini(c, ss.str());
    ASSERT_EQ(e.err, Err::Ok) << "line " << e.line;
    EXPECT_EQ(validate(c), Err::Ok);
    EXPECT_EQ(c.source.width, 1920);
    EXPECT_EQ(c.client.t_stale_ms, 400);
}

TEST(Config, LiveKeys) {
    Config c;
    EXPECT_EQ(set_key(c, "encoder.bitrate_kbps", "2000", Live::Server), Err::Ok);
    EXPECT_EQ(c.encoder.bitrate_kbps, 2000);
    EXPECT_EQ(set_key(c, "source.width", "1280", Live::Server), Err::NotLive);
    EXPECT_EQ(set_key(c, "client.t_stale_ms", "100", Live::Server), Err::NotLive);
    EXPECT_EQ(set_key(c, "client.t_stale_ms", "100", Live::Client), Err::Ok);
    EXPECT_EQ(set_key(c, "encoder.preset", "fast", Live::Server), Err::Ok);
    EXPECT_EQ(set_key(c, "encoder.preset", "warp", Live::Server), Err::BadValue);
    EXPECT_EQ(set_key(c, "server.queue.max_frames", "9", Live::Server), Err::BadValue);
    EXPECT_EQ(set_key(c, "client.vsync", "maybe"), Err::BadValue);
    EXPECT_EQ(key_live("client.decode_queue"), Live::Client);
    EXPECT_EQ(key_live("nope"), Live::No);

    c.client.t_stale_ms = 600;
    EXPECT_EQ(validate(c), Err::BadValue);
}

TEST(Nal, ScansTypes) {
    uint8_t b[64] = {};
    uint32_t n = 0;
    n += write_nal_header(b + n, kNalVps);
    n += write_nal_header(b + n, kNalSps);
    n += write_nal_header(b + n, kNalPps);
    uint32_t idr_at = n;
    n += write_nal_header(b + n, kNalIdrWRadl);
    b[n++] = 0xAA;
    EXPECT_TRUE(is_valid_idr(Bytes{b, n}));
    EXPECT_FALSE(is_valid_idr(Bytes{b, idr_at}));  // parameter sets only

    uint8_t p[16] = {};
    uint32_t pn = write_nal_header(p, kNalTrailR);
    EXPECT_EQ(nal_types(Bytes{p, pn}), uint64_t(1) << kNalTrailR);
    EXPECT_FALSE(is_valid_idr(Bytes{p, pn}));

    // 3-byte start codes are also recognised.
    uint8_t s3[] = {0, 0, 1, uint8_t(kNalSps << 1), 1};
    EXPECT_EQ(nal_types(Bytes{s3, 5}), uint64_t(1) << kNalSps);
}

TEST(Log, RowsFormat) {
    ServerRow s{};
    s.seq = -1;
    s.dropped = 1;
    s.drop_reason = uint8_t(DropReason::EncoderBusy);
    char buf[512];
    format_row(s, buf, sizeof buf);
    EXPECT_EQ(std::string(buf).rfind("-1,", 0), 0u);
    EXPECT_NE(std::string(buf).find("encoder_busy"), std::string::npos);

    ClientRow c{};
    c.seq = 3;
    c.fate = uint8_t(Fate::Shown);
    format_row(c, buf, sizeof buf);
    std::string line(buf);
    EXPECT_NE(line.find("shown,1"), std::string::npos);
    // Same number of columns as the header.
    auto commas = [](const std::string& x) { return std::count(x.begin(), x.end(), ','); };
    EXPECT_EQ(commas(line), commas(kClientCsvHeader));
    format_row(s, buf, sizeof buf);
    EXPECT_EQ(commas(buf), commas(kServerCsvHeader));
}
