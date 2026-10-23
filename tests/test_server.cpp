#include <gtest/gtest.h>

#include "server/core.hpp"

using namespace hs;

namespace {

constexpr int64_t kWallEpoch = 1'700'000'000 * kSec;

Time at_ms(double ms) {
    int64_t ns = int64_t(ms * kMs);
    return Time{ns, kWallEpoch + ns};
}

struct Harness {
    ServerState s;
    explicit Harness(const Config& c) { server_init(s, c); }

    ServerCmds step(const ServerEvent& ev) {
        ServerCmds out;
        server_step(s, ev, out);
        return out;
    }
    ServerCmds encoded(double ms, const EncodeJob& job, bool key, uint32_t len = 1000) {
        ServerEvent ev = srv_event(ServerEvent::Encoded, at_ms(ms));
        ev.encoded = EncodedEv{job.seq, job.slot, len, key, true, at_ms(ms).wall_ns, at_ms(ms).wall_ns, 0};
        return step(ev);
    }
    std::vector<ServerRow> rows() {
        std::vector<ServerRow> v;
        while (!s.rows.empty()) v.push_back(s.rows.pop());
        return v;
    }
};

const ServerCmd* find(const ServerCmds& cmds, ServerCmd::Kind k, int nth = 0) {
    for (const ServerCmd& c : cmds)
        if (c.kind == k && nth-- == 0) return &c;
    return nullptr;
}

const ServerCmd* find_timer(const ServerCmds& cmds, uint8_t id) {
    for (const ServerCmd& c : cmds)
        if (c.kind == ServerCmd::ArmTimer && c.arm.id == id) return &c;
    return nullptr;
}

Config base_config() {
    Config c;
    c.source.fps = 30;
    c.transport.max_frame_bytes = 100000;
    return c;
}

}  // namespace

TEST(ServerCore, PacingUsesAbsoluteDeadlines) {
    Harness h(base_config());
    ServerCmds c = h.step(srv_event(ServerEvent::Start, at_ms(0)));
    ASSERT_TRUE(find(c, ServerCmd::StartEncode));
    EXPECT_EQ(find_timer(c, kTimerTick)->arm.at_mono, kSec / 30);
    ASSERT_TRUE(find_timer(c, kTimerStats));

    // Waking 7 ms late does not shift the schedule.
    c = h.step(srv_timer(at_ms(40), kTimerTick));
    EXPECT_EQ(find_timer(c, kTimerTick)->arm.at_mono, 2 * kSec / 30);

    // Waking several frames late skips ahead instead of bursting.
    c = h.step(srv_timer(at_ms(200), kTimerTick));
    EXPECT_EQ(find_timer(c, kTimerTick)->arm.at_mono, 7 * kSec / 30);
}

TEST(ServerCore, IspDelayHoldsCaptureUntilReady) {
    Config cfg = base_config();
    cfg.source.sim_isp_delay_ms = 50;
    Harness h(cfg);
    ServerCmds c = h.step(srv_event(ServerEvent::Start, at_ms(0)));
    EXPECT_FALSE(find(c, ServerCmd::StartEncode));
    EXPECT_EQ(find_timer(c, kTimerIsp)->arm.at_mono, 50 * kMs);

    c = h.step(srv_timer(at_ms(50), kTimerIsp));
    const ServerCmd* enc = find(c, ServerCmd::StartEncode);
    ASSERT_TRUE(enc);
    EXPECT_EQ(enc->encode.capture_wall, at_ms(0).wall_ns);  // stamped at capture, not at encode
}

TEST(ServerCore, EncoderBusyKeepsOnlyNewestCapture) {
    Harness h(base_config());
    ServerCmds c = h.step(srv_event(ServerEvent::Start, at_ms(0)));
    EncodeJob job0 = find(c, ServerCmd::StartEncode)->encode;

    c = h.step(srv_timer(at_ms(33.4), kTimerTick));
    EXPECT_FALSE(find(c, ServerCmd::StartEncode));
    c = h.step(srv_timer(at_ms(66.7), kTimerTick));
    EXPECT_FALSE(find(c, ServerCmd::StartEncode));
    std::vector<ServerRow> rows = h.rows();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].seq, -1);
    EXPECT_EQ(rows[0].drop_reason, uint8_t(DropReason::EncoderBusy));

    c = h.encoded(70, job0, true);
    const ServerCmd* next = find(c, ServerCmd::StartEncode);
    ASSERT_TRUE(next);
    EXPECT_EQ(next->encode.capture_index, 2u);
    EXPECT_EQ(next->encode.seq, 1u);  // seq counts encoded frames: no gap from the busy drop
}

TEST(ServerCore, NewClientWaitsForIdrThenSends) {
    Harness h(base_config());
    h.step(srv_event(ServerEvent::DataConnected, at_ms(0)));
    ServerCmds c = h.step(srv_event(ServerEvent::Start, at_ms(0)));
    EncodeJob j = find(c, ServerCmd::StartEncode)->encode;
    EXPECT_TRUE(j.force_idr);

    c = h.encoded(5, j, /*key=*/false);  // encoder ignored the request: P can't start a stream, ask again
    EXPECT_FALSE(find(c, ServerCmd::Write));
    EXPECT_EQ(h.rows().back().drop_reason, uint8_t(DropReason::WaitingIdr));

    c = h.step(srv_timer(at_ms(33.4), kTimerTick));
    j = find(c, ServerCmd::StartEncode)->encode;
    EXPECT_TRUE(j.force_idr);
    c = h.encoded(40, j, true);
    const ServerCmd* w = find(c, ServerCmd::Write);
    ASSERT_TRUE(w);
    Result<FrameHeader> hdr = decode_header(slot_ptr(h.s.pool, w->write.slot), 100000);
    ASSERT_EQ(hdr.err, Err::Ok);
    EXPECT_EQ(hdr.value.frame_type, 1);
    EXPECT_EQ(hdr.value.flags, kFlagForcedIdr | kFlagAfterDrop);
    EXPECT_EQ(hdr.value.seq, 1u);

    // While writing, the next frame queues; WriteDone logs; Writable sends it.
    c = h.step(srv_timer(at_ms(66.7), kTimerTick));
    j = find(c, ServerCmd::StartEncode)->encode;
    c = h.encoded(70, j, false);
    EXPECT_FALSE(find(c, ServerCmd::Write));
    EXPECT_EQ(h.s.queue.size(), 1u);
    h.step(srv_event(ServerEvent::WriteDone, at_ms(71)));
    ServerRow sent = h.rows().back();
    EXPECT_EQ(sent.seq, 1);
    EXPECT_EQ(sent.dropped, 0);
    c = h.step(srv_event(ServerEvent::Writable, at_ms(72)));
    EXPECT_TRUE(find(c, ServerCmd::Write));
}

// Connect, encode an IDR and start writing it: leaves the harness mid-write.
static void start_streaming(Harness& h) {
    h.step(srv_event(ServerEvent::DataConnected, at_ms(0)));
    ServerCmds c = h.step(srv_event(ServerEvent::Start, at_ms(0)));
    h.encoded(1, find(c, ServerCmd::StartEncode)->encode, true);
    ASSERT_EQ(h.s.tx, TxState::Writing);
    h.rows();
}

static EncodeJob tick(Harness& h, double ms) {
    ServerCmds c = h.step(srv_timer(at_ms(ms), kTimerTick));
    const ServerCmd* e = find(c, ServerCmd::StartEncode);
    EXPECT_TRUE(e);
    return e ? e->encode : EncodeJob{};
}

TEST(ServerCore, OverflowDropsEverythingUntilIdr) {
    Config cfg = base_config();
    cfg.server.queue.max_frames = 2;
    Harness h(cfg);
    start_streaming(h);
    h.encoded(34, tick(h, 33.4), false);
    h.encoded(67, tick(h, 66.7), false);
    EXPECT_EQ(h.s.queue.size(), 2u);

    h.encoded(101, tick(h, 100), false);  // third P overflows a 2-frame queue
    std::vector<ServerRow> rows = h.rows();
    ASSERT_EQ(rows.size(), 3u);
    for (const ServerRow& r : rows) EXPECT_EQ(r.drop_reason, uint8_t(DropReason::Overflow));
    EXPECT_TRUE(h.s.waiting_idr);

    EncodeJob j = tick(h, 133.4);
    EXPECT_TRUE(j.force_idr);
    h.encoded(134, j, false);  // P in flight before the IDR arrives: dropped
    EXPECT_EQ(h.rows().back().drop_reason, uint8_t(DropReason::WaitingIdr));
    j = tick(h, 166.7);
    h.encoded(167, j, true);
    EXPECT_EQ(h.s.queue.size(), 1u);
    EXPECT_FALSE(h.s.waiting_idr);
}

TEST(ServerCore, TooOldDropsToNextIdrInQueue) {
    Config cfg = base_config();
    cfg.server.queue.max_frames = 5;
    cfg.server.queue.max_age_ms = 100;
    Harness h(cfg);
    start_streaming(h);
    h.encoded(34, tick(h, 33.4), false);          // P  capture 33
    h.encoded(67, tick(h, 66.7), false);          // P  capture 67
    h.encoded(101, tick(h, 100), true);           // IDR capture 100
    h.encoded(134, tick(h, 133.4), false);        // P  capture 133
    h.step(srv_event(ServerEvent::WriteDone, at_ms(180)));
    h.rows();

    // At 180 ms the captures from 33 and 67 are older than 100 ms; the IDR (80 ms) is not.
    ServerCmds c = h.step(srv_event(ServerEvent::Writable, at_ms(180)));
    std::vector<ServerRow> rows = h.rows();
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].drop_reason, uint8_t(DropReason::TooOld));
    EXPECT_EQ(rows[1].drop_reason, uint8_t(DropReason::WaitingIdr));
    const ServerCmd* w = find(c, ServerCmd::Write);
    ASSERT_TRUE(w);
    EXPECT_EQ(decode_header(slot_ptr(h.s.pool, w->write.slot), 100000).value.frame_type, 1);
    EXPECT_FALSE(h.s.waiting_idr);
}

TEST(ServerCore, KeyframeRequestForcesNextEncode) {
    Harness h(base_config());
    start_streaming(h);
    EncodeJob j1 = tick(h, 33.4);
    EXPECT_FALSE(j1.force_idr);
    h.step(srv_ctrl(at_ms(40), make_keyframe_req(KfReason::Stale, 0)));
    h.encoded(41, j1, false);
    EncodeJob j2 = tick(h, 66.7);
    EXPECT_TRUE(j2.force_idr);
    h.encoded(68, j2, true);
    EXPECT_EQ(h.s.pool.flags[j2.slot] & kFlagForcedIdr, kFlagForcedIdr);
    EXPECT_EQ(h.s.keyframe_reqs, 1u);
}

TEST(ServerCore, ConfigSetAcksWhenApplied) {
    Harness h(base_config());
    ServerCmds c = h.step(srv_event(ServerEvent::Start, at_ms(0)));
    EncodeJob j0 = find(c, ServerCmd::StartEncode)->encode;

    c = h.step(srv_ctrl(at_ms(5), make_config_set("encoder.bitrate_kbps", "2000")));
    EXPECT_FALSE(find(c, ServerCmd::SendCtrl));  // encoder keys ack when the reopen happens
    EXPECT_EQ(h.s.cfg.encoder.bitrate_kbps, 2000);

    h.encoded(10, j0, true);
    c = h.step(srv_timer(at_ms(33.4), kTimerTick));
    const ServerCmd* e = find(c, ServerCmd::StartEncode);
    ASSERT_TRUE(e);
    EXPECT_TRUE(e->encode.reopen);
    EXPECT_EQ(e->encode.params.bitrate_kbps, 2000);
    const ServerCmd* ack = find(c, ServerCmd::SendCtrl);
    ASSERT_TRUE(ack);
    EXPECT_EQ(ack->ctrl.type, CtrlType::ConfigAck);
    EXPECT_EQ(ack->ctrl.ack.status, uint8_t(Err::Ok));
    EXPECT_EQ(ack->ctrl.ack.applied_at_seq, e->encode.seq);

    c = h.step(srv_ctrl(at_ms(40), make_config_set("source.width", "640")));
    ack = find(c, ServerCmd::SendCtrl);
    ASSERT_TRUE(ack);
    EXPECT_EQ(ack->ctrl.ack.status, uint8_t(Err::NotLive));
    EXPECT_EQ(h.s.cfg.source.width, 1920);

    c = h.step(srv_ctrl(at_ms(41), make_config_set("server.queue.max_age_ms", "300")));
    ack = find(c, ServerCmd::SendCtrl);
    ASSERT_TRUE(ack);  // non-encoder keys apply and ack immediately
    EXPECT_EQ(h.s.cfg.server.queue.max_age_ms, 300);
}

TEST(ServerCore, PingPongAndStats) {
    Harness h(base_config());
    ServerCmds c = h.step(srv_ctrl(at_ms(7), make_ping(9, 1234)));
    const ServerCmd* p = find(c, ServerCmd::SendCtrl);
    ASSERT_TRUE(p);
    EXPECT_EQ(p->ctrl.type, CtrlType::Pong);
    EXPECT_EQ(p->ctrl.pong.nonce, 9u);
    EXPECT_EQ(p->ctrl.pong.client_tx_ns, 1234);
    EXPECT_EQ(p->ctrl.pong.server_rx_ns, at_ms(7).wall_ns);

    ServerEvent st = srv_timer(at_ms(500), kTimerStats);
    st.timer.sync = SyncStatus{true, 3000, 9000};
    c = h.step(st);
    p = find(c, ServerCmd::SendCtrl);
    ASSERT_TRUE(p);
    EXPECT_EQ(p->ctrl.type, CtrlType::Stats);
    EXPECT_EQ(p->ctrl.stats.sync_ok, 1);
    EXPECT_EQ(p->ctrl.stats.est_error_ns, 3000);
    EXPECT_EQ(find_timer(c, kTimerStats)->arm.at_mono, 1000 * kMs);
}

TEST(ServerCore, StaleCompletionsAfterReconnectAreIgnored) {
    Harness h(base_config());
    start_streaming(h);
    h.step(srv_event(ServerEvent::DataDisconnected, at_ms(5)));
    h.step(srv_event(ServerEvent::DataConnected, at_ms(6)));
    const uint32_t free_before = h.s.pool.free.size();
    h.step(srv_event(ServerEvent::WriteDone, at_ms(7)));  // from the old connection's write
    ServerCmds c = h.step(srv_event(ServerEvent::Writable, at_ms(7)));
    EXPECT_EQ(h.s.pool.free.size(), free_before);  // the slot was not freed twice
    EXPECT_FALSE(find(c, ServerCmd::Write));
    EXPECT_EQ(h.s.tx, TxState::Writable);
}
