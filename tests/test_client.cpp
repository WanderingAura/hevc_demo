#include <gtest/gtest.h>

#include "client/core.hpp"
#include "sim/sim.hpp"

using namespace hs;

namespace {

constexpr int64_t kEpoch = 1'700'000'000 * kSec;

Time at_ms(double ms) {
    int64_t ns = int64_t(ms * kMs);
    return Time{ns, kEpoch + ns};
}

std::vector<uint8_t> frame_bytes(uint32_t seq, bool idr, int64_t capture_wall, int64_t send_wall, uint32_t len = 200) {
    std::vector<uint8_t> f(kHeaderLen + len);
    FrameHeader h{};
    h.magic = kFrameMagic;
    h.version = kWireVersion;
    h.header_len = kHeaderLen;
    h.seq = seq;
    h.frame_type = idr;
    h.capture_ts_ns = capture_wall;
    h.send_ts_ns = send_wall;
    h.payload_len = len;
    encode_header(h, f.data());
    fake_access_unit(f.data() + kHeaderLen, len, idr);
    return f;
}

Config small_config() {
    Config c;
    c.transport.max_frame_bytes = 4096;
    return c;
}

const ClientCmd* find(const ClientCmds& cmds, ClientCmd::Kind k) {
    for (const ClientCmd& c : cmds)
        if (c.kind == k) return &c;
    return nullptr;
}

const ClientCmd* find_kf_req(const ClientCmds& cmds) {
    for (const ClientCmd& c : cmds)
        if (c.kind == ClientCmd::SendCtrl && c.ctrl.type == CtrlType::KeyframeReq) return &c;
    return nullptr;
}

struct Harness {
    ClientState s;
    explicit Harness(const Config& c = small_config()) { client_init(s, c); }
    ClientCmds step(const ClientEvent& ev) {
        ClientCmds out;
        client_step(s, ev, out);
        return out;
    }
    // Delivers a frame that is `age_ms` old on arrival at time `ms`.
    ClientCmds frame(double ms, uint32_t seq, bool idr, double age_ms) {
        std::vector<uint8_t> f =
            frame_bytes(seq, idr, at_ms(ms - age_ms).wall_ns, at_ms(ms - age_ms / 2).wall_ns);
        return step(cli_bytes(at_ms(ms), Bytes{f.data(), uint32_t(f.size())}));
    }
    ClientCmds decoded(double ms, uint32_t seq, uint32_t slot, bool ok = true) {
        ClientEvent ev = cli_event(ClientEvent::Decoded, at_ms(ms));
        ev.decoded = DecodedEv{seq, slot, ok, at_ms(ms).wall_ns, at_ms(ms).wall_ns};
        return step(ev);
    }
    std::vector<ClientRow> rows() {
        std::vector<ClientRow> v;
        while (!s.rows.empty()) v.push_back(s.rows.pop());
        return v;
    }
};

}  // namespace

TEST(Reasm, ByteAtATimeAndMultiFrameChunks) {
    FramePool pool;
    Config c = small_config();
    ClientState st;
    client_init(st, c);
    std::vector<uint8_t> a = frame_bytes(1, true, 10, 20), b = frame_bytes(2, false, 30, 40, 100);
    std::vector<uint8_t> both(a);
    both.insert(both.end(), b.begin(), b.end());

    Reasm r{};
    int done = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        FeedOut f = reasm_feed(r, Bytes{a.data() + i, 1}, at_ms(double(i)), st.pool, 4096);
        ASSERT_EQ(f.err, Err::Ok);
        if (f.done) {
            ++done;
            EXPECT_EQ(st.pool.hdr[f.slot].seq, 1u);
            EXPECT_EQ(f.first_wall, at_ms(0).wall_ns);  // first header byte
            EXPECT_EQ(std::memcmp(frame_ptr(st.pool, f.slot), a.data() + kHeaderLen, 200), 0);
        }
    }
    EXPECT_EQ(done, 1);

    Reasm r2{};
    Bytes in{both.data(), uint32_t(both.size())};
    FeedOut f1 = reasm_feed(r2, in, at_ms(0), st.pool, 4096);
    ASSERT_TRUE(f1.done);
    EXPECT_EQ(f1.consumed, a.size());
    FeedOut f2 = reasm_feed(r2, Bytes{in.p + f1.consumed, in.n - f1.consumed}, at_ms(0), st.pool, 4096);
    ASSERT_TRUE(f2.done);
    EXPECT_EQ(st.pool.hdr[f2.slot].seq, 2u);

    std::vector<uint8_t> bad = a;
    bad[0] = 'X';
    Reasm r3{};
    EXPECT_EQ(reasm_feed(r3, Bytes{bad.data(), uint32_t(bad.size())}, at_ms(0), st.pool, 4096).err, Err::BadMagic);
    std::vector<uint8_t> big = frame_bytes(3, false, 0, 0, 5000);
    Reasm r4{};
    EXPECT_EQ(reasm_feed(r4, Bytes{big.data(), kHeaderLen}, at_ms(0), st.pool, 4096).err, Err::BadLength);
}

TEST(Ages, ComputesAndFlagsAnomalies) {
    Ages a = compute_ages(1000 * kMs, 1010 * kMs, 1015 * kMs, kMs);
    EXPECT_EQ(a.camera_age, 10 * kMs);
    EXPECT_EQ(a.transit, 5 * kMs);
    EXPECT_EQ(a.age_recv, 15 * kMs);
    EXPECT_FALSE(a.anomaly);
    EXPECT_TRUE(compute_ages(0, 10 * kMs, 8 * kMs, kMs).anomaly);   // transit -2 ms: clock trouble
    EXPECT_FALSE(compute_ages(0, 10 * kMs, 9500 * kUs, kMs).anomaly);  // -0.5 ms is within tolerance
}

TEST(WinMin, SlidingTenSeconds) {
    WinMin w{};
    EXPECT_EQ(winmin_get(w, 0), 0);
    winmin_push(w, 0, 50);
    winmin_push(w, 500 * kMs, 30);
    winmin_push(w, 3 * kSec, 40);
    EXPECT_EQ(winmin_get(w, 3 * kSec), 30);
    EXPECT_EQ(winmin_get(w, 9 * kSec), 30);
    EXPECT_EQ(winmin_get(w, 10 * kSec), 40);  // the 0 s bucket has aged out
    winmin_push(w, 13 * kSec, 45);
    EXPECT_EQ(winmin_get(w, 13 * kSec), 45);
}

namespace {
FsmCfg kCfg{400 * kMs, 500 * kMs, 400 * kMs, 3};
FrameFacts facts(uint32_t seq, bool idr, int64_t age_ms, uint32_t qlen = 0) {
    return FrameFacts{seq, idr, idr, true, age_ms * kMs, 0, qlen, 2};
}
}  // namespace

TEST(Fsm, WaitIdrOnlyLeavesOnFreshIdr) {
    Fsm f{};
    f.state = FsmState::WaitIdr;
    EXPECT_EQ(fsm_on_frame(f, facts(1, false, 10), kCfg).verdict, Verdict::Drop);
    EXPECT_EQ(fsm_on_frame(f, facts(2, true, 600), kCfg).verdict, Verdict::Drop);  // too old even as IDR
    EXPECT_EQ(f.state, FsmState::WaitIdr);
    FrameFacts bad = facts(3, true, 10);
    bad.idr_valid = false;  // header says IDR, NAL units disagree
    EXPECT_EQ(fsm_on_frame(f, bad, kCfg).verdict, Verdict::Drop);
    FsmOut o = fsm_on_frame(f, facts(4, true, 10), kCfg);
    EXPECT_EQ(o.verdict, Verdict::DecodeShow);
    EXPECT_FALSE(o.flush);
    EXPECT_EQ(f.state, FsmState::Normal);

    Fsm g{};
    g.state = FsmState::WaitIdr;
    EXPECT_EQ(fsm_on_frame(g, facts(9, true, 450), kCfg).verdict, Verdict::DecodeHide);  // soft-stale IDR
    EXPECT_EQ(g.state, FsmState::Catchup);
}

TEST(Fsm, NormalTransitions) {
    auto normal = [] {
        Fsm f{};
        f.state = FsmState::WaitIdr;
        fsm_on_frame(f, facts(10, true, 10), kCfg);
        return f;
    };
    Fsm f = normal();
    EXPECT_EQ(fsm_on_frame(f, facts(11, false, 20), kCfg).verdict, Verdict::DecodeShow);

    FsmOut o = fsm_on_frame(f, facts(13, false, 20), kCfg);  // 12 lost
    EXPECT_EQ(o.verdict, Verdict::Drop);
    EXPECT_TRUE(o.flush);
    EXPECT_TRUE(o.request_idr);
    EXPECT_EQ(o.reason, KfReason::Gap);
    EXPECT_EQ(f.state, FsmState::WaitIdr);

    f = normal();
    EXPECT_EQ(fsm_on_frame(f, facts(15, true, 20), kCfg).verdict, Verdict::DecodeShow);  // gap + IDR is fine

    f = normal();
    o = fsm_on_frame(f, facts(11, false, 501), kCfg);
    EXPECT_EQ(o.reason, KfReason::Stale);
    EXPECT_EQ(f.state, FsmState::WaitIdr);

    f = normal();
    EXPECT_EQ(fsm_on_frame(f, facts(11, false, 450, 1), kCfg).verdict, Verdict::DecodeHide);
    EXPECT_EQ(f.state, FsmState::Catchup);

    f = normal();
    o = fsm_on_frame(f, facts(11, false, 450, /*qlen=*/2), kCfg);  // queue full
    EXPECT_EQ(o.reason, KfReason::QueueFull);

    f = normal();
    FrameFacts x = facts(11, false, 1000);
    x.trusted = false;  // unsynchronised clocks: ages ignored, queueing still counts
    EXPECT_EQ(fsm_on_frame(f, x, kCfg).verdict, Verdict::DecodeShow);
    x = facts(12, false, 0);
    x.trusted = false;
    x.queueing_ns = 450 * kMs;
    EXPECT_EQ(fsm_on_frame(f, x, kCfg).verdict, Verdict::DecodeHide);
}

TEST(Fsm, CatchupRecoversOrGivesUp) {
    Fsm f{};
    f.state = FsmState::WaitIdr;
    fsm_on_frame(f, facts(1, true, 10), kCfg);
    fsm_on_frame(f, facts(2, false, 450), kCfg);
    EXPECT_EQ(f.state, FsmState::Catchup);
    EXPECT_EQ(fsm_on_frame(f, facts(3, false, 100), kCfg).verdict, Verdict::DecodeShow);  // fresh again
    EXPECT_EQ(f.state, FsmState::Normal);

    for (uint32_t s = 4; s <= 6; ++s) EXPECT_EQ(fsm_on_frame(f, facts(s, false, 450), kCfg).verdict, Verdict::DecodeHide);
    FsmOut o = fsm_on_frame(f, facts(7, false, 450), kCfg);  // still stale after catchup_max (3)
    EXPECT_EQ(o.reason, KfReason::Catchup);
    EXPECT_EQ(f.state, FsmState::WaitIdr);

    EXPECT_EQ(fsm_on_decode_error(f).reason, KfReason::DecodeError);
}

TEST(ClientCore, KeyframeRequestsAreRateLimitedAndBackOff) {
    Harness h;
    ClientCmds c = h.step(cli_event(ClientEvent::DataConnected, at_ms(0)));
    const ClientCmd* req = find_kf_req(c);
    ASSERT_TRUE(req);
    EXPECT_EQ(req->ctrl.kf.reason, uint8_t(KfReason::Connect));
    const ClientCmd* t = find(c, ClientCmd::ArmTimer);
    ASSERT_TRUE(t);
    EXPECT_EQ(t->arm.at_mono, 300 * kMs);

    // Stream starts, then a gap 100 ms later: rate-limited, so the retry timer sends it.
    h.frame(50, 1, true, 10);
    h.decoded(52, 1, h.s.decoding_slot);
    c = h.frame(100, 3, false, 10);
    EXPECT_FALSE(find_kf_req(c));
    EXPECT_EQ(find(c, ClientCmd::ArmTimer)->arm.at_mono, 300 * kMs);

    int64_t expect_iv[] = {600, 1200, 2000, 2000};
    int64_t now = 300;
    for (int64_t iv : expect_iv) {
        c = h.step(cli_timer(at_ms(double(now)), kTimerKfRetry));
        req = find_kf_req(c);
        ASSERT_TRUE(req);
        EXPECT_EQ(req->ctrl.kf.reason, uint8_t(KfReason::Retry));
        EXPECT_EQ(find(c, ClientCmd::ArmTimer)->arm.at_mono, (now + iv) * kMs);
        now += iv;
    }

    h.frame(double(now) + 5, 9, true, 10);  // recovered: backoff resets
    EXPECT_EQ(h.s.kf.interval, 300 * kMs);
    c = h.step(cli_timer(at_ms(double(now) + 10), kTimerKfRetry));
    EXPECT_FALSE(find_kf_req(c));
}

TEST(ClientCore, ShowsFreshFramesAndDropsLateOnes) {
    Harness h;
    h.step(cli_event(ClientEvent::DataConnected, at_ms(0)));
    ClientCmds c = h.frame(100, 1, true, 30);
    const ClientCmd* d = find(c, ClientCmd::StartDecode);
    ASSERT_TRUE(d);
    EXPECT_TRUE(d->decode.flush_before);  // first decode after (re)connect flushes

    c = h.decoded(105, 1, d->decode.slot);
    const ClientCmd* p = find(c, ClientCmd::Present);
    ASSERT_TRUE(p);
    EXPECT_EQ(p->present_seq, 1u);
    ClientEvent shown = cli_event(ClientEvent::Displayed, at_ms(110));
    shown.displayed.seq = 1;
    h.step(shown);
    std::vector<ClientRow> rows = h.rows();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].fate, uint8_t(Fate::Shown));
    EXPECT_EQ(rows[0].age_show, 40 * kMs);

    // Fresh on arrival (390 ms incl. margin), but decode finishes after t_stale: not shown.
    c = h.frame(200, 2, false, 388);
    d = find(c, ClientCmd::StartDecode);
    ASSERT_TRUE(d);
    c = h.decoded(220, 2, d->decode.slot);
    EXPECT_FALSE(find(c, ClientCmd::Present));
    rows = h.rows();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].fate, uint8_t(Fate::Late));
}

TEST(ClientCore, SupersededPresentsAreLogged) {
    Harness h;
    h.step(cli_event(ClientEvent::DataConnected, at_ms(0)));
    ClientCmds c = h.frame(10, 1, true, 5);
    h.decoded(12, 1, find(c, ClientCmd::StartDecode)->decode.slot);
    c = h.frame(20, 2, false, 5);
    h.decoded(22, 2, find(c, ClientCmd::StartDecode)->decode.slot);
    ClientEvent shown = cli_event(ClientEvent::Displayed, at_ms(25));
    shown.displayed.seq = 2;  // the display only ever picked up frame 2
    h.step(shown);
    std::vector<ClientRow> rows = h.rows();
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].fate, uint8_t(Fate::Superseded));
    EXPECT_EQ(rows[1].fate, uint8_t(Fate::Shown));
}

TEST(ClientCore, UiSetRoutesClientAndServerKeys) {
    Harness h;
    ClientCmds c = h.step(cli_ui_set(at_ms(0), "client.t_stale_ms", "300"));
    EXPECT_FALSE(find(c, ClientCmd::SendCtrl));
    EXPECT_EQ(h.s.cfg.client.t_stale_ms, 300);
    EXPECT_EQ(h.s.status.local_status, uint8_t(Err::Ok));

    h.step(cli_ui_set(at_ms(0), "client.t_stale_ms", "900"));  // above t_hard: rejected
    EXPECT_EQ(h.s.cfg.client.t_stale_ms, 300);
    EXPECT_EQ(h.s.status.local_status, uint8_t(Err::BadValue));

    c = h.step(cli_ui_set(at_ms(0), "encoder.bitrate_kbps", "2000"));
    const ClientCmd* s = find(c, ClientCmd::SendCtrl);
    ASSERT_TRUE(s);
    EXPECT_EQ(s->ctrl.type, CtrlType::ConfigSet);
    EXPECT_STREQ(s->ctrl.set.key, "encoder.bitrate_kbps");
}

TEST(ClientCore, ProtocolErrorClosesConnection) {
    Harness h;
    h.step(cli_event(ClientEvent::DataConnected, at_ms(0)));
    std::vector<uint8_t> f = frame_bytes(1, true, 0, 0);
    f[0] = 'X';
    ClientCmds c = h.step(cli_bytes(at_ms(1), Bytes{f.data(), uint32_t(f.size())}));
    EXPECT_TRUE(find(c, ClientCmd::CloseData));
    c = h.step(cli_bytes(at_ms(2), Bytes{f.data(), uint32_t(f.size())}));
    EXPECT_EQ(c.n, 0u);  // ignored until reconnect
}

TEST(ClientCore, BurstOfFramesInOneRead) {
    // After a network stall the kernel hands over many small frames in one read.
    Harness h;
    h.step(cli_event(ClientEvent::DataConnected, at_ms(0)));
    std::vector<uint8_t> burst;
    for (uint32_t i = 0; i < 30; ++i) {
        std::vector<uint8_t> f = frame_bytes(i, i == 0, at_ms(1).wall_ns, at_ms(1).wall_ns, 100);
        burst.insert(burst.end(), f.begin(), f.end());
    }
    ClientCmds c = h.step(cli_bytes(at_ms(5), Bytes{burst.data(), uint32_t(burst.size())}));
    EXPECT_LT(c.n, 64u);
    EXPECT_EQ(h.s.status.frames_rx, 30u);
    EXPECT_EQ(h.s.fsm.state, FsmState::WaitIdr);  // the decode queue overflowed: flush, wait for an IDR
}
