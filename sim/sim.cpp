#include "sim/sim.hpp"

#include <deque>
#include <queue>

#include "client/run.hpp"
#include "common/nal.hpp"
#include "server/run.hpp"

namespace hs {

namespace {

enum LinkId : uint8_t { kData, kCtrlUp, kCtrlDown, kLinkCount };

constexpr int64_t kReconnectNs = 100 * kMs;

struct SimTimer {
    uint8_t id;
    uint32_t gen;
};

struct LinkArrive {
    uint8_t link;
    uint32_t epoch;
};

struct SimEv {
    enum Kind : uint8_t { Srv, Cli, SrvTimer, CliTimer, Arrive, Action, Feed } kind;
    union {
        ServerEvent srv;
        ClientEvent cli;
        SimTimer timer;
        LinkArrive arrive;
        uint32_t index;  // Action / Feed
    };
};

struct Item {
    int64_t t;
    uint64_t order;
    SimEv ev;
};

struct Later {
    bool operator()(const Item& a, const Item& b) const { return a.t != b.t ? a.t > b.t : a.order > b.order; }
};

struct Link {
    LinkCfg cfg;
    int64_t wire_free = 0;
    int64_t last_arrive = 0;
    uint32_t epoch = 0;
    std::deque<std::vector<uint8_t>> inflight;
};

// ---- sim ports ----

struct SimClockPort final : Clock {
    Sim::Impl* s;
    bool server;
    Time now() override;
    SyncStatus sync() override;
};

struct SimTimers final : Timers {
    Sim::Impl* s;
    bool server;
    void arm(uint8_t id, int64_t at_mono_ns) override;
};

struct SimDataTx final : ByteTx {
    Sim::Impl* s;
    void write(Bytes frame) override;
};

struct SimDataRx final : ByteRx {
    Sim::Impl* s;
    void close() override;
};

struct SimCtrlTx final : MsgTx {
    Sim::Impl* s;
    bool server;
    void send(const CtrlMsg& m) override;
};

struct FakeEncoder final : Encoder {
    Sim::Impl* s;
    bool first = true;
    int since_key = 0;
    void submit(const EncodeJob& job, MutBytes out) override;
};

struct FakeDecoder final : Decoder {
    Sim::Impl* s;
    bool have_ref = false;
    bool error_next = false;
    int64_t stall_until = 0;
    void submit(const DecodeJob& job, Bytes au) override;
    void present(uint32_t seq) override;
};

struct MemLog final : LogSink {
    Sim* self;
    void server_row(const ServerRow& r) override { self->server_rows.push_back(r); }
    void client_row(const ClientRow& r) override { self->client_rows.push_back(r); }
};

}  // namespace

struct Sim::Impl {
    Sim& self;
    Config cfg;
    SimParams p;
    int64_t now = 0;
    uint64_t order = 0;
    std::priority_queue<Item, std::vector<Item>, Later> pq;
    Rng rng;
    Link links[kLinkCount];
    std::vector<SimAction> actions;
    std::vector<std::vector<uint8_t>> feeds;
    std::vector<uint8_t> sink_buf;

    std::unique_ptr<ServerState> srv;
    std::unique_ptr<ClientState> cli;
    uint32_t srv_gen[kSrvTimerCount] = {};
    uint32_t cli_gen[kCliTimerCount] = {};
    CtrlReader srv_reader{}, cli_reader{};

    SimClockPort srv_clock, cli_clock;
    SimTimers srv_timers, cli_timers;
    SimDataTx data_tx;
    SimDataRx data_rx;
    SimCtrlTx srv_msg, cli_msg;
    FakeEncoder encoder;
    FakeDecoder decoder;
    MemLog log;
    ServerPorts sp{};
    ClientPorts cp{};

    Impl(Sim& owner, const Config& c, const SimParams& params) : self(owner), cfg(c), p(params), rng{params.seed} {
        std::vector<Item> storage;
        storage.reserve(4096);
        pq = decltype(pq)(Later{}, std::move(storage));
        links[kData].cfg = p.data;
        links[kCtrlUp].cfg = p.ctrl_up;
        links[kCtrlDown].cfg = p.ctrl_down;
        srv_clock.s = cli_clock.s = srv_timers.s = cli_timers.s = this;
        srv_msg.s = cli_msg.s = data_tx.s = data_rx.s = encoder.s = decoder.s = this;
        srv_clock.server = srv_timers.server = srv_msg.server = true;
        cli_clock.server = cli_timers.server = cli_msg.server = false;
        log.self = &self;
        sp = ServerPorts{&srv_clock, &srv_timers, &data_tx, &srv_msg, &encoder, &log};
        cp = ClientPorts{&cli_clock, &cli_timers, &data_rx, &cli_msg, &decoder, &log};

        if (p.with_server) {
            srv = std::make_unique<ServerState>();
            server_init(*srv, cfg);
            push(0, srv_ev(srv_event(ServerEvent::DataConnected, Time{})));
            push(0, srv_ev(srv_event(ServerEvent::Start, Time{})));
        }
        if (p.with_client) {
            cli = std::make_unique<ClientState>();
            client_init(*cli, cfg);
            push(0, cli_ev(cli_event(ClientEvent::DataConnected, Time{})));
            push(0, cli_ev(cli_event(ClientEvent::Start, Time{})));
        }
    }

    static SimEv srv_ev(const ServerEvent& e) {
        SimEv v;
        v.kind = SimEv::Srv;
        v.srv = e;
        return v;
    }
    static SimEv cli_ev(const ClientEvent& e) {
        SimEv v;
        v.kind = SimEv::Cli;
        v.cli = e;
        return v;
    }

    void push(int64_t t, const SimEv& ev) { pq.push(Item{t < now ? now : t, order++, ev}); }

    Time clock_at(const SimClockCfg& c, int64_t t) const {
        return Time{t, p.wall_epoch_ns + t + c.wall_offset_ns + int64_t(double(t) * c.drift_ppm * 1e-6)};
    }
    Time srv_time(int64_t t) const { return clock_at(p.server_clock, t); }
    Time cli_time(int64_t t) const { return clock_at(p.client_clock, t); }

    // Fluid TCP-ish model: bytes leave at the link rate, arrive after delay +- jitter,
    // never reordered. Returns when the last byte leaves the sender.
    // ponytail: departure times are fixed at write time, so a rate change only affects
    // later writes (a big IDR started at 1 Mbit/s finishes at 1 Mbit/s). Schedule chunk
    // departures one at a time if recovery timing after rate changes needs to be exact.
    int64_t link_write(uint8_t id, Bytes b) {
        Link& L = links[id];
        int64_t t = L.wire_free > now ? L.wire_free : now;
        for (uint32_t off = 0; off < b.n; off += L.cfg.chunk) {
            uint32_t n = b.n - off < L.cfg.chunk ? b.n - off : L.cfg.chunk;
            t += int64_t(n) * 8 * kSec / L.cfg.rate_bps;
            int64_t arrive = t + L.cfg.delay_ns + rng_jitter(rng, L.cfg.jitter_ns);
            if (arrive < L.last_arrive) arrive = L.last_arrive;
            L.last_arrive = arrive;
            L.inflight.emplace_back(b.p + off, b.p + off + n);
            SimEv ev;
            ev.kind = SimEv::Arrive;
            ev.arrive = LinkArrive{id, L.epoch};
            push(arrive, ev);
        }
        L.wire_free = t;
        return t;
    }

    void dispatch_srv(ServerEvent ev) {
        ev.t = srv_time(now);
        server_dispatch(*srv, ev, sp);
    }
    void dispatch_cli(ClientEvent ev) {
        ev.t = cli_time(now);
        client_dispatch(*cli, ev, cp);
    }

    void feed_ctrl(CtrlReader& r, Bytes b, bool to_server) {
        while (b.n) {
            CtrlFeed f = ctrl_feed(r, b);
            assert(f.err == Err::Ok);
            b.p += f.consumed;
            b.n -= f.consumed;
            if (!f.done) continue;
            if (to_server) dispatch_srv(srv_ctrl(Time{}, f.msg));
            else dispatch_cli(cli_ctrl(Time{}, f.msg));
        }
    }

    void sink(Bytes b) {
        sink_buf.insert(sink_buf.end(), b.p, b.p + b.n);
        while (sink_buf.size() >= kHeaderLen) {
            Result<FrameHeader> h = decode_header(sink_buf.data(), uint32_t(cfg.transport.max_frame_bytes));
            assert(h.err == Err::Ok);
            size_t total = kHeaderLen + h.value.payload_len;
            if (sink_buf.size() < total) break;
            self.sink_frames.push_back(h.value);
            sink_buf.erase(sink_buf.begin(), sink_buf.begin() + long(total));
        }
    }

    void on_arrive(const LinkArrive& a) {
        Link& L = links[a.link];
        if (a.epoch != L.epoch) return;  // connection was reset
        std::vector<uint8_t> chunk = std::move(L.inflight.front());
        L.inflight.pop_front();
        Bytes b{chunk.data(), uint32_t(chunk.size())};
        switch (a.link) {
            case kData:
                if (cli) dispatch_cli(cli_bytes(Time{}, b));
                else sink(b);
                break;
            case kCtrlUp:
                if (srv) feed_ctrl(srv_reader, b, true);
                break;
            case kCtrlDown:
                if (cli) feed_ctrl(cli_reader, b, false);
                break;
        }
    }

    void stall(uint8_t id, int64_t ns) {
        Link& L = links[id];
        L.wire_free = (L.wire_free > now ? L.wire_free : now) + ns;
    }

    void run_action(const SimAction& a) {
        switch (a.kind) {
            case SimAction::DataRate: links[kData].cfg.rate_bps = a.value; break;
            case SimAction::DataStall: stall(kData, a.value); break;
            case SimAction::CtrlStall:
                stall(kCtrlUp, a.value);
                stall(kCtrlDown, a.value);
                break;
            case SimAction::DecoderStall: decoder.stall_until = now + a.value; break;
            case SimAction::DecodeError: decoder.error_next = true; break;
            case SimAction::ToServer: {
                uint8_t buf[kCtrlMaxBytes];
                link_write(kCtrlUp, Bytes{buf, encode_ctrl(a.ctrl, buf)});
                break;
            }
            case SimAction::ToClient:
                if (cli) dispatch_cli(a.ev);
                break;
        }
    }

    void process(const Item& it) {
        now = it.t;
        const SimEv& e = it.ev;
        switch (e.kind) {
            case SimEv::Srv: dispatch_srv(e.srv); break;
            case SimEv::Cli: dispatch_cli(e.cli); break;
            case SimEv::SrvTimer:
                if (srv && e.timer.gen == srv_gen[e.timer.id]) dispatch_srv(srv_timer(Time{}, e.timer.id));
                break;
            case SimEv::CliTimer:
                if (cli && e.timer.gen == cli_gen[e.timer.id]) dispatch_cli(cli_timer(Time{}, e.timer.id));
                break;
            case SimEv::Arrive: on_arrive(e.arrive); break;
            case SimEv::Action: run_action(actions[e.index]); break;
            case SimEv::Feed: {
                std::vector<uint8_t>& f = feeds[e.index];
                if (read_send_ts(f.data()) == 0) patch_send_ts(f.data(), srv_time(now).wall_ns);
                link_write(kData, Bytes{f.data(), uint32_t(f.size())});
                break;
            }
        }
    }
};

namespace {

Time SimClockPort::now() { return server ? s->srv_time(s->now) : s->cli_time(s->now); }
SyncStatus SimClockPort::sync() { return server ? s->p.server_clock.sync : s->p.client_clock.sync; }

void SimTimers::arm(uint8_t id, int64_t at_mono_ns) {
    SimEv ev;
    ev.kind = server ? SimEv::SrvTimer : SimEv::CliTimer;
    ev.timer = SimTimer{id, server ? ++s->srv_gen[id] : ++s->cli_gen[id]};
    s->push(at_mono_ns, ev);
}

void SimDataTx::write(Bytes frame) {
    int64_t done = s->link_write(kData, frame);
    // The socket accepts bytes until fewer than lowat remain unsent; then the write
    // completes and the socket is writable again.
    const LinkCfg& c = s->links[kData].cfg;
    int64_t when = done - int64_t(c.lowat) * 8 * kSec / c.rate_bps;
    s->push(when, Sim::Impl::srv_ev(srv_event(ServerEvent::WriteDone, Time{})));
    s->push(when, Sim::Impl::srv_ev(srv_event(ServerEvent::Writable, Time{})));
}

void SimDataRx::close() {
    Link& L = s->links[kData];
    L.inflight.clear();
    L.epoch++;
    L.wire_free = L.last_arrive = s->now;
    s->sink_buf.clear();
    s->push(s->now, Sim::Impl::cli_ev(cli_event(ClientEvent::DataDisconnected, Time{})));
    s->push(s->now + kReconnectNs, Sim::Impl::cli_ev(cli_event(ClientEvent::DataConnected, Time{})));
    if (s->srv) {
        s->push(s->now, Sim::Impl::srv_ev(srv_event(ServerEvent::DataDisconnected, Time{})));
        s->push(s->now + kReconnectNs, Sim::Impl::srv_ev(srv_event(ServerEvent::DataConnected, Time{})));
    }
}

void SimCtrlTx::send(const CtrlMsg& m) {
    (server ? s->self.server_sent : s->self.client_sent).push_back(m);
    uint8_t buf[kCtrlMaxBytes];
    s->link_write(server ? kCtrlDown : kCtrlUp, Bytes{buf, encode_ctrl(m, buf)});
}

void FakeEncoder::submit(const EncodeJob& job, MutBytes out) {
    const EncodeParams& p = job.params;
    const bool key = first || job.force_idr || job.reopen || since_key + 1 >= p.gop;
    first = false;
    since_key = key ? 0 : since_key + 1;
    const int64_t avg = int64_t(p.bitrate_kbps) * 1000 / 8 / p.fps;
    int64_t size = key ? avg * s->p.codec.idr_ratio : avg * 9 / 10;
    size += rng_jitter(s->rng, size / 10);
    if (size > int64_t(out.n)) size = out.n;
    uint32_t len = fake_access_unit(out.p, uint32_t(size), key);

    const int64_t dur = s->p.codec.enc_ns + rng_jitter(s->rng, s->p.codec.enc_jitter_ns);
    ServerEvent ev = srv_event(ServerEvent::Encoded, Time{});
    ev.encoded = EncodedEv{job.seq, job.slot, len, key, true, s->srv_time(s->now).wall_ns,
                           s->srv_time(s->now + dur).wall_ns, dur};
    s->push(s->now + dur, Sim::Impl::srv_ev(ev));
}

void FakeDecoder::submit(const DecodeJob& job, Bytes au) {
    if (job.flush_before) have_ref = false;
    const bool idr = is_valid_idr(au);
    bool ok = !error_next && (idr || have_ref);  // a P-frame without references can't decode
    error_next = false;
    have_ref = ok;
    const int64_t start = stall_until > s->now ? stall_until : s->now;
    const int64_t end = start + s->p.codec.dec_ns;
    ClientEvent ev = cli_event(ClientEvent::Decoded, Time{});
    ev.decoded = DecodedEv{job.seq, job.slot, ok, s->cli_time(start).wall_ns, s->cli_time(end).wall_ns};
    s->push(end, Sim::Impl::cli_ev(ev));
}

void FakeDecoder::present(uint32_t seq) {
    ClientEvent ev = cli_event(ClientEvent::Displayed, Time{});
    ev.displayed.seq = seq;
    s->push(s->now + s->p.codec.display_ns, Sim::Impl::cli_ev(ev));
}

}  // namespace

uint32_t fake_access_unit(uint8_t* out, uint32_t len, bool idr) {
    if (len < 64) len = 64;
    uint32_t n = 0;
    auto nal = [&](uint8_t type, uint32_t body) {
        n += write_nal_header(out + n, type);
        std::memset(out + n, 0xAA, body);
        n += body;
    };
    if (idr) {
        nal(kNalVps, 4);
        nal(kNalSps, 4);
        nal(kNalPps, 4);
        nal(kNalIdrWRadl, len - n - 6);
    } else {
        nal(kNalTrailR, len - 6);
    }
    return n;
}

SimAction sim_act(SimAction::Kind k, int64_t value) {
    SimAction a{};
    a.kind = k;
    a.value = value;
    return a;
}

SimAction sim_to_server(const CtrlMsg& m) {
    SimAction a{};
    a.kind = SimAction::ToServer;
    a.ctrl = m;
    return a;
}

SimAction sim_to_client(const ClientEvent& ev) {
    SimAction a{};
    a.kind = SimAction::ToClient;
    a.ev = ev;
    return a;
}

Sim::Sim(const Config& cfg, const SimParams& p) : impl_(std::make_unique<Impl>(*this, cfg, p)) {}
Sim::~Sim() = default;

void Sim::at(int64_t t_ns, const SimAction& a) {
    impl_->actions.push_back(a);
    SimEv ev;
    ev.kind = SimEv::Action;
    ev.index = uint32_t(impl_->actions.size() - 1);
    impl_->push(t_ns, ev);
}

void Sim::feed(int64_t t_ns, const FrameHeader& h) {
    std::vector<uint8_t> f(kHeaderLen + (h.payload_len < 64 ? 64 : h.payload_len));
    FrameHeader hh = h;
    hh.payload_len = uint32_t(f.size() - kHeaderLen);
    encode_header(hh, f.data());
    fake_access_unit(f.data() + kHeaderLen, hh.payload_len, h.frame_type == 1);
    impl_->feeds.push_back(std::move(f));
    SimEv ev;
    ev.kind = SimEv::Feed;
    ev.index = uint32_t(impl_->feeds.size() - 1);
    impl_->push(t_ns, ev);
}

void Sim::run_until(int64_t t_ns) {
    while (!impl_->pq.empty() && impl_->pq.top().t <= t_ns) {
        Item it = impl_->pq.top();
        impl_->pq.pop();
        impl_->process(it);
    }
    impl_->now = t_ns;
}

int64_t Sim::now() const { return impl_->now; }
Time Sim::server_time(int64_t t_ns) const { return impl_->srv_time(t_ns); }
Time Sim::client_time(int64_t t_ns) const { return impl_->cli_time(t_ns); }
ServerState& Sim::server() { return *impl_->srv; }
ClientState& Sim::client() { return *impl_->cli; }

}  // namespace hs
