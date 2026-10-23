// Deterministic single-threaded simulation of either side or both. The same
// server_dispatch / client_dispatch as the real binaries run against sim ports:
// virtual time, per-side clocks with offset/drift, a TCP-like link model and a
// fake codec. Same params + seed => identical rows.
#pragma once
#include <memory>
#include <vector>

#include "client/core.hpp"
#include "server/core.hpp"

namespace hs {

struct LinkCfg {
    int64_t rate_bps = 1'000'000'000;
    int64_t delay_ns = 100 * kUs;
    int64_t jitter_ns = 0;   // uniform +-; arrivals never reorder (TCP)
    uint32_t lowat = 32768;  // the sender is writable while fewer bytes are unsent
    uint32_t chunk = 16384;  // delivery granularity (exercises reassembly)
};

struct SimClockCfg {
    int64_t wall_offset_ns = 0;
    double drift_ppm = 0;
    SyncStatus sync{true, 0, 0};
};

struct CodecModel {
    int64_t enc_ns = 8 * kMs;
    int64_t enc_jitter_ns = 1 * kMs;
    int64_t dec_ns = 4 * kMs;
    int64_t display_ns = 2 * kMs;  // present -> on screen
    int idr_ratio = 6;             // IDR size / average frame size
};

struct SimParams {
    bool with_server = true;  // false: client fed by feed()
    bool with_client = true;  // false: server data goes to a sink
    LinkCfg data, ctrl_up, ctrl_down;
    SimClockCfg server_clock, client_clock;
    CodecModel codec;
    uint64_t seed = 1;
    int64_t wall_epoch_ns = 1'700'000'000LL * kSec;
};

struct SimAction {
    enum Kind : uint8_t {
        DataRate,      // value: bits per second
        DataStall,     // value: ns the data link carries nothing
        CtrlStall,     // value: ns both control links carry nothing
        DecoderStall,  // value: ns the client decoder is blocked
        DecodeError,   // the next decode fails
        ToServer,      // ctrl: sent to the server over the control link
        ToClient,      // ev: dispatched to the client (UI input)
    } kind;
    union {
        int64_t value;
        CtrlMsg ctrl;
        ClientEvent ev;
    };
};
SimAction sim_act(SimAction::Kind k, int64_t value = 0);
SimAction sim_to_server(const CtrlMsg& m);
SimAction sim_to_client(const ClientEvent& ev);

class Sim {
public:
    Sim(const Config& cfg, const SimParams& p);
    ~Sim();
    Sim(const Sim&) = delete;
    Sim& operator=(const Sim&) = delete;

    void at(int64_t t_ns, const SimAction& a);
    // Client-alone: put [h][fake Annex B payload of h.payload_len] on the data link at t.
    void feed(int64_t t_ns, const FrameHeader& h);
    void run_until(int64_t t_ns);
    int64_t now() const;
    Time server_time(int64_t t_ns) const;
    Time client_time(int64_t t_ns) const;

    ServerState& server();
    ClientState& client();

    std::vector<ServerRow> server_rows;
    std::vector<ClientRow> client_rows;
    std::vector<FrameHeader> sink_frames;  // server alone: frames that reached the far end
    std::vector<CtrlMsg> server_sent, client_sent;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

// Fills `out` with a fake access unit: real NAL headers (IDR = VPS/SPS/PPS/IDR,
// P = TRAIL_R) padded with 0xAA. Returns the bytes written (= len, at least 32).
uint32_t fake_access_unit(uint8_t* out, uint32_t len, bool idr);

}  // namespace hs
