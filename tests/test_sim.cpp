// Deterministic scenario tests: the real cores and interpreters in virtual time.
#include <gtest/gtest.h>

#include <algorithm>

#include "sim/sim.hpp"

using namespace hs;

namespace {

Config sim_config() {
    Config c;
    c.transport.max_frame_bytes = 500'000;
    return c;
}

SimParams params() {
    SimParams p;
    p.data.rate_bps = 100'000'000;  // 100 Mbit/s
    p.data.delay_ns = 500 * kUs;
    p.ctrl_up = p.ctrl_down = p.data;
    return p;
}

int64_t secs(double s) { return int64_t(s * kSec); }

// Server-side time of a wall timestamp (server clock has no offset in these tests).
double capture_s(const SimParams& p, int64_t wall) { return double(wall - p.wall_epoch_ns) / kSec; }

int count_fate(const std::vector<ClientRow>& rows, Fate f) {
    return int(std::count_if(rows.begin(), rows.end(), [f](const ClientRow& r) { return r.fate == uint8_t(f); }));
}

// The key metric: frames on screen older than t_stale. `slack` covers present -> display.
int stale_shown(const std::vector<ClientRow>& rows, const Config& c, int64_t slack) {
    int n = 0;
    for (const ClientRow& r : rows)
        if (r.fate == uint8_t(Fate::Shown) && r.age_show > int64_t(c.client.t_stale_ms) * kMs + slack) ++n;
    return n;
}

int keyframe_reqs(const std::vector<CtrlMsg>& sent) {
    return int(std::count_if(sent.begin(), sent.end(), [](const CtrlMsg& m) { return m.type == CtrlType::KeyframeReq; }));
}

// First shown frame captured at or after time t (seconds), or -1.
double first_shown_after(const std::vector<ClientRow>& rows, const SimParams& p, double t) {
    for (const ClientRow& r : rows)
        if (r.fate == uint8_t(Fate::Shown) && capture_s(p, r.capture_ts) >= t) return capture_s(p, r.capture_ts);
    return -1;
}

// Seq gaps on the wire must only ever land on an IDR.
void expect_no_p_after_gap(const std::vector<FrameHeader>& frames) {
    for (size_t i = 1; i < frames.size(); ++i)
        if (frames[i].seq != frames[i - 1].seq + 1) {
            EXPECT_EQ(frames[i].frame_type, 1) << "P-frame after a gap at seq " << frames[i].seq;
            EXPECT_TRUE(frames[i].flags & kFlagAfterDrop) << "seq " << frames[i].seq;
        }
}

}  // namespace

// ---- server alone ----

TEST(SimServer, CleanLinkSendsEveryFrame) {
    Config c = sim_config();
    SimParams p = params();
    p.with_client = false;
    Sim sim(c, p);
    sim.run_until(secs(3));
    ASSERT_GE(sim.sink_frames.size(), 88u);
    EXPECT_EQ(sim.sink_frames[0].frame_type, 1);
    EXPECT_EQ(sim.sink_frames[0].seq, 0u);
    expect_no_p_after_gap(sim.sink_frames);
    for (size_t i = 1; i < sim.sink_frames.size(); ++i) EXPECT_EQ(sim.sink_frames[i].seq, sim.sink_frames[i - 1].seq + 1);
    for (const ServerRow& r : sim.server_rows) EXPECT_EQ(r.dropped, 0);
    EXPECT_EQ(sim.sink_frames[60].frame_type, 1);  // periodic IDR, gop 60
}

TEST(SimServer, BandwidthCollapseDropsToIdr) {
    Config c = sim_config();
    SimParams p = params();
    p.with_client = false;
    Sim sim(c, p);
    sim.at(secs(1), sim_act(SimAction::DataRate, 1'000'000));  // 1 Mbit/s, below the 4 Mbit/s bitrate
    sim.at(secs(4), sim_act(SimAction::DataRate, 100'000'000));
    sim.run_until(secs(7));

    int drops = 0;
    for (const ServerRow& r : sim.server_rows) drops += r.dropped;
    EXPECT_GT(drops, 10);
    expect_no_p_after_gap(sim.sink_frames);
    // Queue stays bounded: nothing arrives that sat in the server longer than max_age + one write.
    EXPECT_GT(sim.server().drops[uint32_t(DropReason::TooOld)] + sim.server().drops[uint32_t(DropReason::Overflow)], 0u);
    // After the link returns, the stream flows again.
    EXPECT_GT(std::count_if(sim.sink_frames.begin(), sim.sink_frames.end(),
                            [&](const FrameHeader& h) { return capture_s(p, h.capture_ts_ns) > 5.0; }),
              50);
}

TEST(SimServer, KeyframeRequestGivesForcedIdrOnNextFrame) {
    Config c = sim_config();
    SimParams p = params();
    p.with_client = false;
    Sim sim(c, p);
    sim.at(secs(1), sim_to_server(make_keyframe_req(KfReason::Manual, 0)));
    sim.run_until(secs(2));
    auto forced = std::find_if(sim.sink_frames.begin(), sim.sink_frames.end(), [&](const FrameHeader& h) {
        return h.seq > 0 && (h.flags & kFlagForcedIdr);
    });
    ASSERT_NE(forced, sim.sink_frames.end());
    EXPECT_EQ(forced->frame_type, 1);
    double t = capture_s(p, forced->capture_ts_ns);
    EXPECT_GE(t, 0.99);
    EXPECT_LT(t, 1.0 + 2.0 / 30);  // within two frame intervals
}

TEST(SimServer, ConfigSetAckNamesTheFrameItAppliesTo) {
    Config c = sim_config();
    SimParams p = params();
    p.with_client = false;
    Sim sim(c, p);
    sim.at(secs(1), sim_to_server(make_config_set("encoder.bitrate_kbps", "1000")));
    sim.run_until(secs(2));
    auto ack = std::find_if(sim.server_sent.begin(), sim.server_sent.end(),
                            [](const CtrlMsg& m) { return m.type == CtrlType::ConfigAck; });
    ASSERT_NE(ack, sim.server_sent.end());
    EXPECT_EQ(ack->ack.status, uint8_t(Err::Ok));
    auto applied = std::find_if(sim.sink_frames.begin(), sim.sink_frames.end(),
                                [&](const FrameHeader& h) { return h.seq == ack->ack.applied_at_seq; });
    ASSERT_NE(applied, sim.sink_frames.end());
    EXPECT_EQ(applied->frame_type, 1);  // the encoder reopened: IDR
    // Frames after the change are smaller (fake encoder sizes follow the bitrate).
    EXPECT_LT((applied + 5)->payload_len, sim.sink_frames[5].payload_len);
}

// ---- client alone ----

namespace {

// Feeds a 30 fps stream to a lone client. age_ms(i) gives each frame's age on the wire.
template <class AgeFn>
void feed_stream(Sim& sim, const SimParams& p, int frames, AgeFn age_ms, int gop = 60) {
    for (int i = 0; i < frames; ++i) {
        int64_t t = secs(0.1) + int64_t(i) * kSec / 30;
        FrameHeader h{};
        h.magic = kFrameMagic;
        h.version = kWireVersion;
        h.header_len = kHeaderLen;
        h.seq = uint32_t(i);
        h.frame_type = i % gop == 0;
        h.capture_ts_ns = p.wall_epoch_ns + t - int64_t(age_ms(i) * kMs);
        h.payload_len = h.frame_type ? 60000 : 12000;
        sim.feed(t, h);
    }
}

}  // namespace

TEST(SimClient, StaleBurstHidesThenRecoversAtIdr) {
    Config c = sim_config();
    SimParams p = params();
    p.with_server = false;
    Sim sim(c, p);
    // Fresh, then 10 frames at 450 ms (soft-stale), then 600 ms (hard), then fresh.
    feed_stream(sim, p, 150, [](int i) { return i >= 30 && i < 40 ? 450.0 : i >= 40 && i < 50 ? 600.0 : 30.0; });
    sim.run_until(secs(6));

    const std::vector<ClientRow>& rows = sim.client_rows;
    auto expected = [](uint32_t seq) {
        if (seq < 30) return Fate::Shown;
        if (seq < 40) return Fate::Hidden;   // soft-stale: decoded to keep references, not shown
        if (seq < 60) return Fate::Dropped;  // hard-stale: wait for the next IDR (60)
        return Fate::Shown;
    };
    ASSERT_EQ(rows.size(), 150u);
    for (const ClientRow& r : rows) EXPECT_EQ(r.fate, uint8_t(expected(r.seq))) << r.seq;
    EXPECT_EQ(stale_shown(rows, c, 0), 0);
    EXPECT_GE(sim.client().status.kf_requests[uint32_t(KfReason::Stale)], 1u);
}

TEST(SimClient, DecoderStallNeverShowsStaleFrames) {
    Config c = sim_config();
    SimParams p = params();
    p.with_server = false;
    Sim sim(c, p);
    feed_stream(sim, p, 200, [](int) { return 30.0; });
    sim.at(secs(2), sim_act(SimAction::DecoderStall, 500 * kMs));
    sim.run_until(secs(8));

    EXPECT_EQ(stale_shown(sim.client_rows, c, p.codec.display_ns), 0);
    EXPECT_GT(count_fate(sim.client_rows, Fate::Flushed) + count_fate(sim.client_rows, Fate::Late), 0);
    EXPECT_GE(keyframe_reqs(sim.client_sent), 2);  // connect + queue full
    // Without a server answering, recovery comes from the next periodic IDR (seq 120, t = 4.1 s).
    double back = first_shown_after(sim.client_rows, p, 2.5);
    EXPECT_GT(back, 0);
    EXPECT_LT(back, 4.2);
}

TEST(SimClient, GapWithIdrIsAcceptedGapWithPIsNot) {
    Config c = sim_config();
    SimParams p = params();
    p.with_server = false;
    Sim sim(c, p);
    // seq 0..9, skip to IDR 20 (server dropped to it), then 21..29, skip to P 35.
    int seqs[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 35, 36};
    for (int i = 0; i < int(sizeof seqs / sizeof seqs[0]); ++i) {
        int64_t t = secs(0.1) + int64_t(i) * kSec / 30;
        FrameHeader h{};
        h.magic = kFrameMagic;
        h.version = kWireVersion;
        h.header_len = kHeaderLen;
        h.seq = uint32_t(seqs[i]);
        h.frame_type = seqs[i] == 0 || seqs[i] == 20;
        h.capture_ts_ns = p.wall_epoch_ns + t - 20 * kMs;
        h.payload_len = 5000;
        sim.feed(t, h);
    }
    sim.run_until(secs(2));
    for (const ClientRow& r : sim.client_rows)
        EXPECT_EQ(r.fate, uint8_t(r.seq < 30 ? Fate::Shown : Fate::Dropped)) << r.seq;
    EXPECT_EQ(sim.client().status.kf_requests[uint32_t(KfReason::Gap)], 1u);
}

// ---- both sides (doc §10 scenarios that apply over TCP) ----

TEST(SimBoth, CleanLan) {
    Config c = sim_config();
    SimParams p = params();
    Sim sim(c, p);
    sim.run_until(secs(5));
    for (const ServerRow& r : sim.server_rows) EXPECT_EQ(r.dropped, 0);
    EXPECT_EQ(keyframe_reqs(sim.client_sent), 1);  // only the one on connect
    EXPECT_GE(count_fate(sim.client_rows, Fate::Shown), 145);
    EXPECT_EQ(count_fate(sim.client_rows, Fate::Dropped) + count_fate(sim.client_rows, Fate::Hidden), 0);
    // Age on screen ~ encode + network + decode + display.
    for (const ClientRow& r : sim.client_rows) EXPECT_LT(r.age_show, 30 * kMs) << r.seq;
    EXPECT_GT(sim.client().status.rtt_min_ns, 0);
    EXPECT_TRUE(sim.client().status.have_stats);
}

TEST(SimBoth, DelayAndJitterRaisesAgeWithoutFalseCatchup) {
    Config c = sim_config();
    SimParams p = params();
    p.data.delay_ns = 20 * kMs;
    p.data.jitter_ns = 5 * kMs;
    p.ctrl_up = p.ctrl_down = p.data;
    Sim sim(c, p);
    sim.run_until(secs(5));
    EXPECT_EQ(count_fate(sim.client_rows, Fate::Hidden) + count_fate(sim.client_rows, Fate::Dropped) +
                  count_fate(sim.client_rows, Fate::Late),
              0);
    int64_t min_age = INT64_MAX;
    for (const ClientRow& r : sim.client_rows)
        if (r.fate == uint8_t(Fate::Shown)) min_age = std::min(min_age, r.age_show);
    EXPECT_GE(min_age, 15 * kMs);  // the added delay shows up in the age
}

TEST(SimBoth, BandwidthCollapseNeverShowsStaleAndRecovers) {
    Config c = sim_config();
    SimParams p = params();
    Sim sim(c, p);
    sim.at(secs(2), sim_act(SimAction::DataRate, 1'000'000));
    sim.at(secs(7), sim_act(SimAction::DataRate, 100'000'000));
    sim.run_until(secs(12));

    EXPECT_EQ(stale_shown(sim.client_rows, c, p.codec.display_ns), 0);
    int drops = 0;
    for (const ServerRow& r : sim.server_rows) drops += r.dropped;
    EXPECT_GT(drops, 0);
    // Shown again within one GOP of the link returning.
    double back = first_shown_after(sim.client_rows, p, 7.0);
    EXPECT_GT(back, 0);
    EXPECT_LT(back, 7.0 + double(c.encoder.gop) / c.source.fps);
    EXPECT_EQ(stale_shown(sim.client_rows, c, int64_t(c.client.t_hard_ms - c.client.t_stale_ms) * kMs), 0);
}

TEST(SimBoth, ClientStallFirstFrameAfterIsFresh) {
    Config c = sim_config();
    SimParams p = params();
    Sim sim(c, p);
    sim.at(secs(2), sim_act(SimAction::DecoderStall, 500 * kMs));
    sim.run_until(secs(5));
    EXPECT_EQ(stale_shown(sim.client_rows, c, p.codec.display_ns), 0);
    double back = first_shown_after(sim.client_rows, p, 2.0);
    EXPECT_GT(back, 0);
    EXPECT_LT(back, 3.0);  // the server answers the keyframe request quickly
}

TEST(SimBoth, ControlStallRecoversFromPeriodicIdrAndRetries) {
    Config c = sim_config();
    SimParams p = params();
    Sim sim(c, p);
    sim.at(secs(1), sim_act(SimAction::CtrlStall, 4 * kSec));
    sim.at(secs(1.5), sim_act(SimAction::DecodeError));  // client loses sync; its request is stuck
    sim.run_until(secs(7));
    EXPECT_EQ(stale_shown(sim.client_rows, c, p.codec.display_ns), 0);
    // Periodic IDRs every 2 s (seq 60 at ~2.0 s) bring the picture back before the control link does.
    double back = first_shown_after(sim.client_rows, p, 1.5);
    EXPECT_GT(back, 0);
    EXPECT_LT(back, 1.5 + double(c.encoder.gop) / c.source.fps);
    EXPECT_GE(sim.client().status.kf_requests[uint32_t(KfReason::DecodeError)], 1u);
}

TEST(SimBoth, ClockAnomaliesAreLoggedNotActedOn) {
    Config c = sim_config();
    SimParams p = params();
    p.client_clock.wall_offset_ns = -5 * kMs;  // client clock 5 ms behind: negative transit
    Sim sim(c, p);
    sim.run_until(secs(2));
    EXPECT_GT(sim.client().status.anomalies, 50u);
    EXPECT_GE(count_fate(sim.client_rows, Fate::Shown), 55);
}

TEST(SimBoth, Deterministic) {
    auto run = [](uint64_t seed) {
        Config c = sim_config();
        c.source.sim_isp_jitter_ms = 3;
        SimParams p = params();
        p.seed = seed;
        p.data.jitter_ns = 2 * kMs;
        Sim sim(c, p);
        sim.at(secs(1), sim_act(SimAction::DataRate, 1'000'000));
        sim.at(secs(3), sim_act(SimAction::DataRate, 100'000'000));
        sim.run_until(secs(5));
        std::string out;
        char buf[512];
        for (const ServerRow& r : sim.server_rows) {
            format_row(r, buf, sizeof buf);
            out += buf;
            out += '\n';
        }
        for (const ClientRow& r : sim.client_rows) {
            format_row(r, buf, sizeof buf);
            out += buf;
            out += '\n';
        }
        return out;
    };
    std::string a = run(7), b = run(7), d = run(8);
    EXPECT_EQ(a, b);
    EXPECT_NE(a, d);
}
