#include "server/core.hpp"

namespace hs {

namespace {

constexpr uint32_t kIspCapacity = 2048;
constexpr uint32_t kRowCapacity = 256;
constexpr uint32_t kPoolSlots = kMaxQueueFrames + 2;  // queue + encoding + writing

void arm(ServerCmds& out, uint8_t id, int64_t at) {
    ServerCmd c;
    c.kind = ServerCmd::ArmTimer;
    c.arm = ArmTimerCmd{id, at};
    out.push(c);
}

void send_ctrl(ServerCmds& out, const CtrlMsg& m) {
    ServerCmd c;
    c.kind = ServerCmd::SendCtrl;
    c.ctrl = m;
    out.push(c);
}

void push_row(ServerState& s, const ServerRow& r) {
    if (s.rows.full()) s.rows.pop();
    s.rows.push(r);
}

ServerRow slot_row(const ServerState& s, uint16_t slot) {
    const PacketPool& p = s.pool;
    ServerRow r{};
    r.seq = p.seq[slot];
    r.gop_id = p.gop[slot];
    r.type = p.key[slot];
    r.flags = p.flags[slot];
    r.size = p.len[slot] > kHeaderLen ? p.len[slot] - kHeaderLen : 0;
    r.capture_ts = p.capture_wall[slot];
    r.enc_in_ts = p.enc_in[slot];
    r.enc_out_ts = p.enc_out[slot];
    r.enqueue_ts = p.enqueue[slot];
    return r;
}

void free_slot(ServerState& s, uint16_t slot) { s.pool.free.push(slot); }

void drop_slot(ServerState& s, uint16_t slot, DropReason why) {
    ServerRow r = slot_row(s, slot);
    r.dropped = 1;
    r.drop_reason = uint8_t(why);
    push_row(s, r);
    s.drops[uint32_t(why)]++;
    free_slot(s, slot);
}

void drop_capture(ServerState& s, const Capture& c) {
    ServerRow r{};
    r.seq = -1;
    r.capture_ts = c.wall;
    r.dropped = 1;
    r.drop_reason = uint8_t(DropReason::EncoderBusy);
    push_row(s, r);
    s.drops[uint32_t(DropReason::EncoderBusy)]++;
}

uint16_t queue_pop(ServerState& s) {
    uint16_t slot = s.queue.pop();
    s.queue_bytes -= s.pool.len[slot];
    return slot;
}

// Nothing more can be sent until an IDR: the next P-frames reference dropped ones.
void begin_wait_idr(ServerState& s) {
    s.waiting_idr = true;
    s.idr_requested = true;
}

void clear_queue_silently(ServerState& s) {
    while (!s.queue.empty()) free_slot(s, queue_pop(s));
}

void start_encode(ServerState& s, const Capture& c, ServerCmds& out) {
    assert(!s.pool.free.empty());
    uint16_t slot = s.pool.free.pop();
    uint32_t seq = s.next_seq++;
    PacketPool& p = s.pool;
    p.seq[slot] = seq;
    p.capture_wall[slot] = c.wall;
    p.capture_mono[slot] = c.mono;
    p.forced[slot] = s.idr_requested;
    p.enc_in[slot] = p.enc_out[slot] = p.enqueue[slot] = 0;

    ServerCmd cmd;
    cmd.kind = ServerCmd::StartEncode;
    cmd.encode = EncodeJob{};
    cmd.encode.seq = seq;
    cmd.encode.slot = slot;
    cmd.encode.capture_index = c.index;
    cmd.encode.capture_wall = c.wall;
    cmd.encode.force_idr = s.idr_requested;
    cmd.encode.reopen = s.reopen_pending;
    cmd.encode.params = encode_params(s.cfg);
    out.push(cmd);

    for (uint32_t i = 0; i < s.pending_acks.n; ++i) {
        CtrlMsg m{};
        m.type = CtrlType::ConfigAck;
        m.ack = s.pending_acks[i];
        m.ack.applied_at_seq = seq;
        send_ctrl(out, m);
    }
    s.pending_acks.n = 0;
    s.idr_requested = false;
    s.reopen_pending = false;
    s.enc_busy = true;
}

void drain_isp(ServerState& s, Time t, ServerCmds& out) {
    while (!s.isp.empty() && s.isp.front().ready_mono <= t.mono_ns) {
        Capture c = s.isp.pop();
        if (!s.enc_busy) {
            start_encode(s, c, out);
        } else {
            // A real VIN drops frames the encoder can't take; keep only the newest.
            if (s.has_pending) drop_capture(s, s.pending);
            s.pending = c;
            s.has_pending = true;
        }
    }
    if (!s.isp.empty()) arm(out, kTimerIsp, s.isp.front().ready_mono);
}

int64_t tick_deadline(const ServerState& s) {
    return s.tick_epoch + s.tick_k * kSec / s.cfg.source.fps;
}

void on_tick(ServerState& s, Time t, ServerCmds& out) {
    int64_t delay = int64_t(s.cfg.source.sim_isp_delay_ms) * kMs +
                    rng_jitter(s.rng, int64_t(s.cfg.source.sim_isp_jitter_ms) * kMs);
    if (delay < 0) delay = 0;
    int64_t ready = t.mono_ns + delay;
    if (ready < s.last_ready) ready = s.last_ready;  // an ISP never reorders frames
    s.last_ready = ready;
    if (s.isp.full()) drop_capture(s, s.isp.pop());
    s.isp.push(Capture{s.capture_index++, t.wall_ns, t.mono_ns, ready});

    s.tick_k++;
    s.next_tick = tick_deadline(s);
    if (s.next_tick <= t.mono_ns) {  // woke more than a frame late: skip, don't burst
        s.tick_k = (t.mono_ns - s.tick_epoch) * s.cfg.source.fps / kSec + 1;
        s.next_tick = tick_deadline(s);
    }
    arm(out, kTimerTick, s.next_tick);
    drain_isp(s, t, out);
}

void try_send(ServerState& s, Time t, ServerCmds& out) {
    const int64_t max_age = int64_t(s.cfg.server.queue.max_age_ms) * kMs;
    while (!s.queue.empty()) {
        uint16_t slot = s.queue.front();
        if (t.mono_ns - s.pool.capture_mono[slot] > max_age) {
            drop_slot(s, queue_pop(s), DropReason::TooOld);
            while (!s.queue.empty() && !s.pool.key[s.queue.front()])
                drop_slot(s, queue_pop(s), DropReason::WaitingIdr);
            if (s.queue.empty()) begin_wait_idr(s);
            continue;
        }
        queue_pop(s);
        s.tx = TxState::Writing;
        s.writing_slot = slot;
        ServerCmd c;
        c.kind = ServerCmd::Write;
        c.write = WriteCmd{slot, s.pool.len[slot]};
        out.push(c);
        return;
    }
}

void enqueue(ServerState& s, uint16_t slot, Time t, ServerCmds& out) {
    PacketPool& p = s.pool;
    if (s.tx == TxState::Disconnected) {  // no client: nothing to send to
        free_slot(s, slot);
        return;
    }
    const bool key = p.key[slot];
    if (s.waiting_idr) {
        if (!key) {
            if (p.forced[slot]) s.idr_requested = true;  // asked for an IDR, got a P: ask again
            drop_slot(s, slot, DropReason::WaitingIdr);
            return;
        }
        s.waiting_idr = false;
        p.flags[slot] |= kFlagAfterDrop;
    }
    const QueueCfg& qc = s.cfg.server.queue;
    if (s.queue.size() + 1 > uint32_t(qc.max_frames) || s.queue_bytes + p.len[slot] > uint32_t(qc.max_bytes)) {
        while (!s.queue.empty()) drop_slot(s, queue_pop(s), DropReason::Overflow);
        if (!key) {
            drop_slot(s, slot, DropReason::Overflow);
            begin_wait_idr(s);
            return;
        }
        p.flags[slot] |= kFlagAfterDrop;  // an IDR always fits an empty queue
    }

    FrameHeader h{};
    h.magic = kFrameMagic;
    h.version = kWireVersion;
    h.header_len = kHeaderLen;
    h.seq = p.seq[slot];
    h.gop_id = p.gop[slot];
    h.frame_type = key;
    h.flags = p.flags[slot];
    h.capture_ts_ns = p.capture_wall[slot];
    h.payload_len = p.len[slot] - kHeaderLen;
    encode_header(h, slot_ptr(p, slot));
    p.enqueue[slot] = t.wall_ns;

    s.queue.push(slot);
    s.queue_bytes += p.len[slot];
    if (s.tx == TxState::Writable) try_send(s, t, out);
}

void on_encoded(ServerState& s, const EncodedEv& e, Time t, ServerCmds& out) {
    s.enc_busy = false;
    uint16_t slot = uint16_t(e.slot);
    PacketPool& p = s.pool;
    p.enc_in[slot] = e.enc_in_wall;
    p.enc_out[slot] = e.enc_out_wall;
    s.enc_last_ns = e.enc_ns;
    s.enc_avg_ns = s.enc_avg_ns ? (s.enc_avg_ns * 7 + e.enc_ns) / 8 : e.enc_ns;

    if (!e.ok) {
        p.len[slot] = kHeaderLen;
        p.key[slot] = 0;
        p.flags[slot] = 0;
        p.gop[slot] = s.gop_id;
        drop_slot(s, slot, DropReason::EncodeError);
        begin_wait_idr(s);  // the encoder's references include the lost frame
    } else {
        if (e.key) {
            s.gop_id++;
            s.idr_count++;
        }
        p.len[slot] = kHeaderLen + e.len;
        p.key[slot] = e.key;
        p.gop[slot] = s.gop_id;
        p.flags[slot] = (p.forced[slot] && e.key) ? kFlagForcedIdr : 0;
        enqueue(s, slot, t, out);
    }
    if (s.has_pending) {
        s.has_pending = false;
        start_encode(s, s.pending, out);
    }
}

void on_write_done(ServerState& s, Time t) {
    uint16_t slot = s.writing_slot;
    ServerRow r = slot_row(s, slot);
    r.send_start_ts = read_send_ts(slot_ptr(s.pool, slot));
    r.send_end_ts = t.wall_ns;
    push_row(s, r);
    free_slot(s, slot);
    s.tx = TxState::WaitWritable;
}

void on_disconnected(ServerState& s) {
    clear_queue_silently(s);
    if (s.tx == TxState::Writing) free_slot(s, s.writing_slot);
    s.tx = TxState::Disconnected;
    s.waiting_idr = false;
}

bool starts_with(const char* s, const char* prefix) { return std::strncmp(s, prefix, std::strlen(prefix)) == 0; }

void on_config_set(ServerState& s, const ConfigSetMsg& m, ServerCmds& out) {
    CtrlMsg reply{};
    reply.type = CtrlType::ConfigAck;
    std::memcpy(reply.ack.key, m.key, kKeyLen);
    std::memcpy(reply.ack.value, m.value, kValLen);
    reply.ack.key[kKeyLen - 1] = reply.ack.value[kValLen - 1] = 0;
    reply.ack.applied_at_seq = s.next_seq;

    Config c = s.cfg;
    Err e = set_key(c, reply.ack.key, reply.ack.value, Live::Server);
    if (e == Err::Ok) e = validate(c);
    reply.ack.status = uint8_t(e);
    if (e != Err::Ok) {
        send_ctrl(out, reply);
        return;
    }
    const bool fps_changed = c.source.fps != s.cfg.source.fps;
    s.cfg = c;
    if (fps_changed) {  // keep the armed deadline, use the new period after it
        s.tick_epoch = s.next_tick;
        s.tick_k = 0;
    }
    const bool reopen = starts_with(reply.ack.key, "encoder.") || fps_changed;
    if (reopen) s.reopen_pending = true;
    if (reopen && !s.pending_acks.full())
        s.pending_acks.push(reply.ack);
    else
        send_ctrl(out, reply);
}

void on_ctrl(ServerState& s, const CtrlMsg& m, Time t, ServerCmds& out) {
    switch (m.type) {
        case CtrlType::Ping: {
            CtrlMsg r{};
            r.type = CtrlType::Pong;
            r.pong.nonce = m.ping.nonce;
            r.pong.client_tx_ns = m.ping.client_tx_ns;
            r.pong.server_rx_ns = t.wall_ns;  // server_tx is stamped by the interpreter
            send_ctrl(out, r);
            break;
        }
        case CtrlType::KeyframeReq:
            s.keyframe_reqs++;
            s.idr_requested = true;
            break;
        case CtrlType::ConfigSet: on_config_set(s, m.set, out); break;
        case CtrlType::None:
        case CtrlType::Pong:
        case CtrlType::ConfigAck:
        case CtrlType::Stats: break;
    }
}

void on_stats(ServerState& s, const TimerEv& te, Time t, ServerCmds& out) {
    CtrlMsg m{};
    m.type = CtrlType::Stats;
    StatsMsg& st = m.stats;
    st.queue_frames = s.queue.size();
    st.queue_bytes = s.queue_bytes;
    for (uint32_t i = 0; i < uint32_t(DropReason::Count); ++i) st.drops[i] = s.drops[i];
    st.idr_count = s.idr_count;
    st.keyframe_reqs = s.keyframe_reqs;
    st.next_seq = s.next_seq;
    st.enc_last_ns = s.enc_last_ns;
    st.enc_avg_ns = s.enc_avg_ns;
    st.sync_ok = te.sync.synced;
    st.est_error_ns = te.sync.est_error_ns;
    st.max_error_ns = te.sync.max_error_ns;
    send_ctrl(out, m);
    arm(out, kTimerStats, t.mono_ns + int64_t(s.cfg.server.stats_interval_ms) * kMs);
}

}  // namespace

ServerEvent srv_event(ServerEvent::Kind k, Time t) {
    ServerEvent ev{};
    ev.kind = k;
    ev.t = t;
    return ev;
}

ServerEvent srv_timer(Time t, uint8_t id) {
    ServerEvent ev = srv_event(ServerEvent::Timer, t);
    ev.timer.id = id;
    return ev;
}

ServerEvent srv_ctrl(Time t, const CtrlMsg& m) {
    ServerEvent ev = srv_event(ServerEvent::Ctrl, t);
    ev.ctrl = m;
    return ev;
}

uint8_t* slot_ptr(PacketPool& p, uint32_t slot) { return p.bytes.data() + size_t(slot) * p.slot_bytes; }

EncodeParams encode_params(const Config& c) {
    EncodeParams e{};
    e.width = c.source.width;
    e.height = c.source.height;
    e.fps = c.source.fps;
    e.bitrate_kbps = c.encoder.bitrate_kbps;
    e.gop = c.encoder.gop;
    e.vbv_ms = c.encoder.vbv_ms;
    copy_cstr(e.preset, c.encoder.preset.c_str());
    return e;
}

void server_init(ServerState& s, const Config& cfg) {
    s.cfg = cfg;
    s.rng = Rng{uint64_t(cfg.source.seed)};
    s.isp.init(kIspCapacity);
    s.rows.init(kRowCapacity);
    s.queue.init(kPoolSlots);

    PacketPool& p = s.pool;
    p.slot_bytes = kHeaderLen + uint32_t(cfg.transport.max_frame_bytes);
    p.bytes.assign(size_t(p.slot_bytes) * kPoolSlots, 0);
    for (auto* v : {&p.len, &p.seq, &p.gop}) v->assign(kPoolSlots, 0);
    for (auto* v : {&p.capture_wall, &p.capture_mono, &p.enc_in, &p.enc_out, &p.enqueue}) v->assign(kPoolSlots, 0);
    for (auto* v : {&p.key, &p.flags, &p.forced}) v->assign(kPoolSlots, 0);
    p.free.init(kPoolSlots);
    for (uint16_t i = 0; i < kPoolSlots; ++i) p.free.push(i);
}

void server_step(ServerState& s, const ServerEvent& ev, ServerCmds& out) {
    switch (ev.kind) {
        case ServerEvent::Start:
            s.tick_epoch = ev.t.mono_ns;
            s.tick_k = 0;
            on_tick(s, ev.t, out);
            arm(out, kTimerStats, ev.t.mono_ns + int64_t(s.cfg.server.stats_interval_ms) * kMs);
            break;
        case ServerEvent::Timer:
            switch (ev.timer.id) {
                case kTimerTick: on_tick(s, ev.t, out); break;
                case kTimerIsp: drain_isp(s, ev.t, out); break;
                case kTimerStats: on_stats(s, ev.timer, ev.t, out); break;
            }
            break;
        case ServerEvent::Encoded: on_encoded(s, ev.encoded, ev.t, out); break;
        // Completions from a connection that has since been replaced are ignored.
        case ServerEvent::WriteDone:
            if (s.tx == TxState::Writing) on_write_done(s, ev.t);
            break;
        case ServerEvent::Writable:
            if (s.tx != TxState::WaitWritable) break;
            s.tx = TxState::Writable;
            try_send(s, ev.t, out);
            break;
        case ServerEvent::DataConnected:
            on_disconnected(s);
            s.tx = TxState::Writable;
            begin_wait_idr(s);  // a new client can only start at an IDR
            break;
        case ServerEvent::DataDisconnected: on_disconnected(s); break;
        case ServerEvent::Ctrl: on_ctrl(s, ev.ctrl, ev.t, out); break;
    }
}

}  // namespace hs
