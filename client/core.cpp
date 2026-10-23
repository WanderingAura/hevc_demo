#include "client/core.hpp"

#include <climits>

#include "common/nal.hpp"

namespace hs {

namespace {

constexpr uint32_t kPoolSlots = kMaxDecodeQueue + 2;  // queue + decoding + receiving
constexpr uint32_t kRowCapacity = 256;
constexpr uint32_t kPendingDisplay = 8;
constexpr int64_t kMaxSaneAge = 60 * kSec;

bool seq_before(uint32_t a, uint32_t b) { return int32_t(a - b) < 0; }

void arm(ClientCmds& out, uint8_t id, int64_t at) {
    ClientCmd c;
    c.kind = ClientCmd::ArmTimer;
    c.arm = ArmTimerCmd{id, at};
    out.push(c);
}

void send_ctrl(ClientCmds& out, const CtrlMsg& m) {
    ClientCmd c;
    c.kind = ClientCmd::SendCtrl;
    c.ctrl = m;
    out.push(c);
}

void push_row(ClientState& s, ClientRow r, Fate f) {
    r.fate = uint8_t(f);
    s.status.fates[uint32_t(f)]++;
    if (s.rows.full()) s.rows.pop();
    s.rows.push(r);
}

int64_t margin_ns(const ClientState& s) {
    if (s.cfg.clock.sync_margin_ms >= 0) return int64_t(s.cfg.clock.sync_margin_ms) * kMs;
    return s.client_sync.est_error_ns + (s.status.have_stats ? s.status.server.est_error_ns : 0);
}

bool sync_ok(const ClientState& s) {
    return s.client_sync.synced && (!s.status.have_stats || s.status.server.sync_ok);
}

bool row_trusted(const ClientState& s, const ClientRow& r) {
    return !r.anomaly && (!s.cfg.clock.require_sync || r.sync_ok);
}

// Empties the decode queue; the decoder is flushed before the next job.
void flush_decode(ClientState& s) {
    while (!s.dq.empty()) {
        uint16_t slot = s.dq.pop();
        push_row(s, s.pool.row[slot], Fate::Flushed);
        s.pool.free.push(slot);
    }
    s.flush_pending = true;
    if (s.decoding) s.discard_inflight = true;
}

void request_keyframe(ClientState& s, KfReason why, Time t, ClientCmds& out) {
    const int64_t min_iv = int64_t(s.cfg.client.keyframe_req_min_interval_ms) * kMs;
    if (!s.kf.ever || t.mono_ns - s.kf.last_mono >= min_iv) {
        send_ctrl(out, make_keyframe_req(why, s.fsm.last_good_seq));
        s.status.kf_requests[uint32_t(why)]++;
        s.kf.ever = true;
        s.kf.last_mono = t.mono_ns;
        s.kf.interval = min_iv;
        arm(out, kTimerKfRetry, t.mono_ns + min_iv);
    } else {
        arm(out, kTimerKfRetry, s.kf.last_mono + min_iv);  // rate-limited: the retry sends it
    }
}

void on_kf_retry(ClientState& s, Time t, ClientCmds& out) {
    if (s.fsm.state != FsmState::WaitIdr || !s.status.data_connected) return;
    send_ctrl(out, make_keyframe_req(KfReason::Retry, s.fsm.last_good_seq));
    s.status.kf_requests[uint32_t(KfReason::Retry)]++;
    s.kf.last_mono = t.mono_ns;
    const int64_t max_iv = int64_t(s.cfg.client.keyframe_req_max_interval_ms) * kMs;
    s.kf.interval = s.kf.interval * 2 < max_iv ? s.kf.interval * 2 : max_iv;
    arm(out, kTimerKfRetry, t.mono_ns + s.kf.interval);
}

void apply_fsm(ClientState& s, const FsmOut& o, Time t, ClientCmds& out) {
    if (o.flush) flush_decode(s);
    if (o.request_idr) request_keyframe(s, o.reason, t, out);
}

void start_decode(ClientState& s, ClientCmds& out) {
    if (s.decoding || s.dq.empty()) return;
    uint16_t slot = s.dq.pop();
    s.decoding = true;
    s.decoding_slot = slot;
    ClientCmd c;
    c.kind = ClientCmd::StartDecode;
    c.decode = DecodeJob{s.pool.hdr[slot].seq, slot, s.flush_pending};
    s.flush_pending = false;
    out.push(c);
}

void on_frame(ClientState& s, uint16_t slot, int64_t first_wall, Time t, ClientCmds& out) {
    const FrameHeader& h = s.pool.hdr[slot];
    ClientRow r{};
    r.seq = h.seq;
    r.type = h.frame_type;
    r.size = h.payload_len;
    r.capture_ts = h.capture_ts_ns;
    r.recv_first_ts = first_wall;
    r.recv_last_ts = t.wall_ns;

    Ages a = compute_ages(h.capture_ts_ns, h.send_ts_ns, t.wall_ns,
                          int64_t(s.cfg.clock.max_negative_transit_ms) * kMs);
    r.transit = a.transit;
    r.age_recv = a.age_recv;
    r.anomaly = a.anomaly;
    if (a.anomaly) s.status.anomalies++;
    else winmin_push(s.dwin, t.mono_ns, a.transit);
    r.d_min = winmin_get(s.dwin, t.mono_ns);
    r.queueing = a.transit - r.d_min > 0 ? a.transit - r.d_min : 0;
    r.rtt_min = s.status.rtt_min_ns;
    r.sync_ok = sync_ok(s);
    r.est_error_us = s.client_sync.est_error_ns / kUs;
    r.fsm_state = uint8_t(s.fsm.state);

    const bool idr = h.frame_type == 1;
    FrameFacts x{};
    x.seq = h.seq;
    x.idr = idr;
    x.idr_valid = idr && is_valid_idr(Bytes{frame_ptr(s.pool, slot), h.payload_len});
    x.trusted = row_trusted(s, r);
    x.age_ns = a.age_recv + margin_ns(s);
    x.queueing_ns = r.queueing;
    x.queue_len = s.dq.size();
    x.queue_cap = uint32_t(s.cfg.client.decode_queue);
    s.status.ages_trusted = x.trusted;

    const FsmState before = s.fsm.state;
    FsmOut o = fsm_on_frame(s.fsm, x, fsm_cfg(s.cfg));
    if (before == FsmState::WaitIdr && s.fsm.state != FsmState::WaitIdr)
        s.kf.interval = int64_t(s.cfg.client.keyframe_req_min_interval_ms) * kMs;  // recovered: reset backoff
    apply_fsm(s, o, t, out);

    s.pool.row[slot] = r;
    if (o.verdict == Verdict::Drop) {
        push_row(s, r, Fate::Dropped);
        s.pool.free.push(slot);
    } else {
        s.pool.show[slot] = o.verdict == Verdict::DecodeShow;
        s.dq.push(slot);
        start_decode(s, out);
    }
}

void on_bytes(ClientState& s, Bytes in, Time t, ClientCmds& out) {
    while (in.n && !s.reasm.broken) {
        FeedOut f = reasm_feed(s.reasm, in, t, s.pool, s.pool.cap);
        if (f.err != Err::Ok) {
            s.reasm.broken = true;
            ClientCmd c;
            c.kind = ClientCmd::CloseData;
            out.push(c);
            return;
        }
        in.p += f.consumed;
        in.n -= f.consumed;
        if (f.done) {
            s.status.frames_rx++;
            on_frame(s, f.slot, f.first_wall, t, out);
        }
    }
}

void on_decoded(ClientState& s, const DecodedEv& d, Time t, ClientCmds& out) {
    s.decoding = false;
    const uint16_t slot = uint16_t(d.slot);
    ClientRow r = s.pool.row[slot];
    r.decode_start = d.start_wall;
    r.decode_end = d.end_wall;
    const bool show = s.pool.show[slot];
    s.pool.free.push(slot);

    if (s.discard_inflight) {
        s.discard_inflight = false;
        push_row(s, r, Fate::Flushed);
    } else if (!d.ok) {
        push_row(s, r, Fate::DecodeError);
        apply_fsm(s, fsm_on_decode_error(s.fsm), t, out);
    } else if (!show) {
        push_row(s, r, Fate::Hidden);
    } else if (row_trusted(s, r) &&
               t.wall_ns - r.capture_ts + margin_ns(s) > int64_t(s.cfg.client.t_stale_ms) * kMs) {
        push_row(s, r, Fate::Late);  // went stale while queued or decoding
    } else {
        ClientCmd c;
        c.kind = ClientCmd::Present;
        c.present_seq = r.seq;
        out.push(c);
        if (s.pending_display.full()) push_row(s, s.pending_display.pop(), Fate::Superseded);
        s.pending_display.push(r);
    }
    start_decode(s, out);
}

void on_displayed(ClientState& s, uint32_t seq, Time t) {
    while (!s.pending_display.empty() && !seq_before(seq, s.pending_display.front().seq)) {
        ClientRow r = s.pending_display.pop();
        if (r.seq != seq) {
            push_row(s, r, Fate::Superseded);  // a newer frame reached the screen first
            continue;
        }
        r.display_ts = t.wall_ns;
        r.age_show = t.wall_ns - r.capture_ts;
        push_row(s, r, Fate::Shown);
        return;
    }
}

void reset_stream(ClientState& s) {
    reasm_reset(s.reasm, s.pool);
    flush_decode(s);
}

void on_ctrl(ClientState& s, const CtrlMsg& m, Time t) {
    switch (m.type) {
        case CtrlType::Pong: {
            int64_t rtt = (t.mono_ns - m.pong.client_tx_ns) - (m.pong.server_tx_ns - m.pong.server_rx_ns);
            s.status.rtt_last_ns = rtt;
            if (s.status.rtt_min_ns == 0 || rtt < s.status.rtt_min_ns) s.status.rtt_min_ns = rtt;
            break;
        }
        case CtrlType::Stats:
            s.status.server = m.stats;
            s.status.have_stats = true;
            break;
        case CtrlType::ConfigAck:
            s.status.last_ack = m.ack;
            s.status.have_ack = true;
            break;
        case CtrlType::None:
        case CtrlType::Ping:
        case CtrlType::KeyframeReq:
        case CtrlType::ConfigSet: break;
    }
}

bool is_client_key(const char* key) {
    return std::strncmp(key, "client.", 7) == 0 || std::strncmp(key, "clock.", 6) == 0;
}

void on_ui_set(ClientState& s, const UiSetEv& u, ClientCmds& out) {
    if (!is_client_key(u.key)) {
        send_ctrl(out, make_config_set(u.key, u.value));
        return;
    }
    Config c = s.cfg;
    Err e = set_key(c, u.key, u.value, Live::Client);
    if (e == Err::Ok) e = validate(c);
    if (e == Err::Ok) s.cfg = c;
    copy_cstr(s.status.local_key, u.key);
    s.status.local_status = uint8_t(e);
}

}  // namespace

// ---- pure building blocks ----

uint8_t* frame_ptr(FramePool& p, uint32_t slot) { return p.bytes.data() + size_t(slot) * p.cap; }

FeedOut reasm_feed(Reasm& r, Bytes in, Time t, FramePool& pool, uint32_t max_payload) {
    FeedOut o{};
    while (o.consumed < in.n) {
        const uint32_t left = in.n - o.consumed;
        if (!r.in_body) {
            if (r.hdr_have == 0) r.first_wall = t.wall_ns;
            uint32_t take = kHeaderLen - r.hdr_have < left ? kHeaderLen - r.hdr_have : left;
            std::memcpy(r.hdr + r.hdr_have, in.p + o.consumed, take);
            r.hdr_have += take;
            o.consumed += take;
            if (r.hdr_have < kHeaderLen) continue;
            Result<FrameHeader> h = decode_header(r.hdr, max_payload);
            if (h.err != Err::Ok) {
                o.err = h.err;
                return o;
            }
            assert(!pool.free.empty());
            r.h = h.value;
            r.slot = pool.free.pop();
            r.in_body = true;
            r.body_have = 0;
        } else {
            uint32_t need = r.h.payload_len - r.body_have;
            uint32_t take = need < left ? need : left;
            std::memcpy(frame_ptr(pool, r.slot) + r.body_have, in.p + o.consumed, take);
            r.body_have += take;
            o.consumed += take;
            if (r.body_have == r.h.payload_len) {
                pool.hdr[r.slot] = r.h;
                o.done = true;
                o.slot = r.slot;
                o.first_wall = r.first_wall;
                r.in_body = false;
                r.hdr_have = 0;
                return o;
            }
        }
    }
    return o;
}

void reasm_reset(Reasm& r, FramePool& pool) {
    if (r.in_body) pool.free.push(r.slot);
    r = Reasm{};
}

Ages compute_ages(int64_t capture, int64_t send, int64_t recv_last, int64_t max_negative_transit) {
    Ages a;
    a.camera_age = send - capture;
    a.transit = recv_last - send;
    a.age_recv = recv_last - capture;
    a.anomaly = a.transit < -max_negative_transit || a.camera_age < 0 || a.age_recv > kMaxSaneAge;
    return a;
}

void winmin_push(WinMin& w, int64_t mono, int64_t v) {
    const int64_t id = mono / kSec;
    const int i = int(id % kWinBuckets);
    if (!w.used[i] || w.id[i] != id) {
        w.used[i] = true;
        w.id[i] = id;
        w.min[i] = v;
    } else if (v < w.min[i]) {
        w.min[i] = v;
    }
}

int64_t winmin_get(const WinMin& w, int64_t mono) {
    const int64_t id = mono / kSec;
    int64_t best = INT64_MAX;
    for (int i = 0; i < kWinBuckets; ++i)
        if (w.used[i] && id - w.id[i] < kWinBuckets && w.min[i] < best) best = w.min[i];
    return best == INT64_MAX ? 0 : best;
}

FsmCfg fsm_cfg(const Config& c) {
    return FsmCfg{int64_t(c.client.t_stale_ms) * kMs, int64_t(c.client.t_hard_ms) * kMs,
                  int64_t(c.client.q_max_ms) * kMs, uint32_t(c.client.catchup_max_frames)};
}

static FsmOut fsm_wait(Fsm& f, KfReason why) {
    f.state = FsmState::WaitIdr;
    f.catchup_frames = 0;
    return FsmOut{Verdict::Drop, true, true, why};
}

FsmOut fsm_on_frame(Fsm& f, const FrameFacts& x, const FsmCfg& c) {
    const bool gap = f.have_last && x.seq != f.last_seq + 1;
    f.have_last = true;
    f.last_seq = x.seq;
    const bool hard = x.trusted && x.age_ns > c.t_hard;
    const bool soft = (x.trusted && x.age_ns > c.t_stale) || x.queueing_ns > c.q_max;
    const FsmOut drop{Verdict::Drop, false, false, KfReason::Retry};

    if (x.idr && !x.idr_valid) return f.state == FsmState::WaitIdr ? drop : fsm_wait(f, KfReason::DecodeError);
    if (f.state == FsmState::WaitIdr) {
        if (!x.idr || hard) return drop;  // keep waiting; the retry timer re-asks
    } else {
        if (gap && !x.idr) return fsm_wait(f, KfReason::Gap);  // a reference was lost
        if (hard) return fsm_wait(f, KfReason::Stale);
        if (x.queue_len >= x.queue_cap) return fsm_wait(f, KfReason::QueueFull);
    }
    // A gap followed by an IDR is fine: the server dropped to it on purpose.
    if (soft) {
        if (f.state == FsmState::Catchup) {
            if (++f.catchup_frames > c.catchup_max) return fsm_wait(f, KfReason::Catchup);
        } else if (f.state == FsmState::Normal && x.queue_len > 1) {
            return fsm_wait(f, KfReason::Stale);  // backlog too big to catch up by decoding
        } else {
            f.state = FsmState::Catchup;
            f.catchup_frames = 1;
        }
        f.last_good_seq = x.seq;
        return FsmOut{Verdict::DecodeHide, false, false, KfReason::Retry};
    }
    f.state = FsmState::Normal;
    f.catchup_frames = 0;
    f.last_good_seq = x.seq;
    return FsmOut{Verdict::DecodeShow, false, false, KfReason::Retry};
}

FsmOut fsm_on_decode_error(Fsm& f) { return fsm_wait(f, KfReason::DecodeError); }

FsmOut fsm_reset(Fsm& f, KfReason why) {
    f.have_last = false;
    return fsm_wait(f, why);
}

// ---- events ----

ClientEvent cli_event(ClientEvent::Kind k, Time t) {
    ClientEvent ev{};
    ev.kind = k;
    ev.t = t;
    return ev;
}

ClientEvent cli_timer(Time t, uint8_t id) {
    ClientEvent ev = cli_event(ClientEvent::Timer, t);
    ev.timer.id = id;
    return ev;
}

ClientEvent cli_ctrl(Time t, const CtrlMsg& m) {
    ClientEvent ev = cli_event(ClientEvent::Ctrl, t);
    ev.ctrl = m;
    return ev;
}

ClientEvent cli_bytes(Time t, Bytes b) {
    ClientEvent ev = cli_event(ClientEvent::DataBytes, t);
    ev.bytes = b;
    return ev;
}

ClientEvent cli_ui_set(Time t, const char* key, const char* value) {
    ClientEvent ev = cli_event(ClientEvent::UiSet, t);
    copy_cstr(ev.ui.key, key);
    copy_cstr(ev.ui.value, value);
    return ev;
}

void client_init(ClientState& s, const Config& cfg) {
    s.cfg = cfg;
    FramePool& p = s.pool;
    p.cap = uint32_t(cfg.transport.max_frame_bytes);
    p.bytes.assign(size_t(p.cap) * kPoolSlots, 0);
    p.hdr.assign(kPoolSlots, FrameHeader{});
    p.row.assign(kPoolSlots, ClientRow{});
    p.show.assign(kPoolSlots, 0);
    p.free.init(kPoolSlots);
    for (uint16_t i = 0; i < kPoolSlots; ++i) p.free.push(i);
    s.dq.init(kMaxDecodeQueue);
    s.pending_display.init(kPendingDisplay);
    s.rows.init(kRowCapacity);
    s.fsm.state = FsmState::WaitIdr;
}

void client_step(ClientState& s, const ClientEvent& ev, ClientCmds& out) {
    const Time t = ev.t;
    switch (ev.kind) {
        case ClientEvent::Start: arm(out, kTimerPing, t.mono_ns); break;
        case ClientEvent::Timer:
            s.client_sync = ev.timer.sync;
            if (ev.timer.id == kTimerPing) {
                send_ctrl(out, make_ping(++s.ping_nonce, t.mono_ns));
                arm(out, kTimerPing, t.mono_ns + int64_t(s.cfg.client.ping_interval_ms) * kMs);
            } else if (ev.timer.id == kTimerKfRetry) {
                on_kf_retry(s, t, out);
            }
            break;
        case ClientEvent::DataConnected:
            reset_stream(s);
            s.status.data_connected = true;
            apply_fsm(s, fsm_reset(s.fsm, KfReason::Connect), t, out);
            break;
        case ClientEvent::DataDisconnected:
            reset_stream(s);
            s.status.data_connected = false;
            fsm_reset(s.fsm, KfReason::Connect);
            break;
        case ClientEvent::DataBytes: on_bytes(s, ev.bytes, t, out); break;
        case ClientEvent::Decoded: on_decoded(s, ev.decoded, t, out); break;
        case ClientEvent::Displayed: on_displayed(s, ev.displayed.seq, t); break;
        case ClientEvent::Ctrl: on_ctrl(s, ev.ctrl, t); break;
        case ClientEvent::UiSet: on_ui_set(s, ev.ui, out); break;
        case ClientEvent::UiForceIdr:
            send_ctrl(out, make_keyframe_req(KfReason::Manual, s.fsm.last_good_seq));
            s.status.kf_requests[uint32_t(KfReason::Manual)]++;
            break;
    }
    s.status.fsm = s.fsm.state;
    s.status.last_seq = s.fsm.last_seq;
    s.status.decode_queue_len = s.dq.size();
    s.status.client_sync = s.client_sync;
    s.status.margin_ns = margin_ns(s);
}

}  // namespace hs
