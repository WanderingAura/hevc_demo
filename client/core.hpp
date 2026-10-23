// Client core: reassembly, frame ages, the staleness FSM, the decode queue and
// keyframe-request rate limiting. Pure: client_step(state, event) -> commands.
#pragma once
#include "common/config.hpp"
#include "common/log.hpp"
#include "common/ports.hpp"
#include "common/wire.hpp"

namespace hs {

enum CliTimer : uint8_t { kTimerPing, kTimerKfRetry, kCliTimerCount };

struct DecodedEv {
    uint32_t seq;
    uint32_t slot;
    bool ok;
    int64_t start_wall;
    int64_t end_wall;
};

struct DisplayedEv {
    uint32_t seq;
};

struct UiSetEv {
    char key[kKeyLen];
    char value[kValLen];
};

struct ClientEvent {
    enum Kind : uint8_t {
        Start,
        Timer,
        DataConnected,
        DataDisconnected,
        DataBytes,
        Decoded,
        Displayed,
        Ctrl,
        UiSet,
        UiForceIdr,
    } kind;
    Time t;
    union {
        TimerEv timer;
        Bytes bytes;  // valid only during dispatch
        DecodedEv decoded;
        DisplayedEv displayed;
        CtrlMsg ctrl;
        UiSetEv ui;
    };
};

ClientEvent cli_event(ClientEvent::Kind k, Time t);
ClientEvent cli_timer(Time t, uint8_t id);
ClientEvent cli_ctrl(Time t, const CtrlMsg& m);
ClientEvent cli_bytes(Time t, Bytes b);
ClientEvent cli_ui_set(Time t, const char* key, const char* value);

struct ClientCmd {
    enum Kind : uint8_t { ArmTimer, StartDecode, Present, SendCtrl, CloseData } kind;
    union {
        ArmTimerCmd arm;
        DecodeJob decode;
        uint32_t present_seq;
        CtrlMsg ctrl;
    };
};
// One DataBytes event can complete many small frames, hence the headroom.
using ClientCmds = FixedVec<ClientCmd, 64>;

// ---- pure building blocks (exposed for tests) ----

// Received frames: payload bytes per slot plus parallel metadata.
struct FramePool {
    uint32_t cap = 0;  // payload bytes per slot
    std::vector<uint8_t> bytes;
    std::vector<FrameHeader> hdr;
    std::vector<ClientRow> row;  // row being filled in
    std::vector<uint8_t> show;   // FSM verdict was DecodeShow
    Ring<uint16_t> free;
};
uint8_t* frame_ptr(FramePool& p, uint32_t slot);

struct Reasm {
    uint8_t hdr[kHeaderLen];
    uint32_t hdr_have;
    bool in_body;
    uint32_t body_have;
    uint16_t slot;
    FrameHeader h;
    int64_t first_wall;
    bool broken;  // protocol error: ignore bytes until reconnect
};

struct FeedOut {
    uint32_t consumed;
    bool done;  // a frame completed into `slot`
    Err err;
    uint16_t slot;
    int64_t first_wall;  // when its first header byte arrived
};
FeedOut reasm_feed(Reasm& r, Bytes in, Time t, FramePool& pool, uint32_t max_payload);
void reasm_reset(Reasm& r, FramePool& pool);

struct Ages {
    int64_t camera_age;  // send - capture: camera side, same clock
    int64_t transit;     // recv_last - send: network, includes sync error
    int64_t age_recv;    // recv_last - capture
    bool anomaly;        // negative transit or absurd age: clock trouble, don't act on it
};
Ages compute_ages(int64_t capture, int64_t send, int64_t recv_last, int64_t max_negative_transit);

// Sliding-window minimum of D over 10 one-second buckets.
// ponytail: 1 s granularity; use a monotonic deque if finer windows matter.
constexpr int kWinBuckets = 10;
struct WinMin {
    int64_t id[kWinBuckets];
    int64_t min[kWinBuckets];
    bool used[kWinBuckets];
};
void winmin_push(WinMin& w, int64_t mono, int64_t v);
int64_t winmin_get(const WinMin& w, int64_t mono);  // 0 if empty

struct Fsm {
    FsmState state;
    bool have_last;
    uint32_t last_seq;
    uint32_t last_good_seq;
    uint32_t catchup_frames;
};

struct FrameFacts {
    uint32_t seq;
    bool idr;
    bool idr_valid;  // header says IDR and the NAL units agree
    bool trusted;    // ages usable (sync ok, no anomaly)
    int64_t age_ns;  // age at receive + sync margin
    int64_t queueing_ns;
    uint32_t queue_len;
    uint32_t queue_cap;
};

struct FsmCfg {
    int64_t t_stale, t_hard, q_max;
    uint32_t catchup_max;
};
FsmCfg fsm_cfg(const Config& c);

enum class Verdict : uint8_t { Drop, DecodeShow, DecodeHide };

struct FsmOut {
    Verdict verdict;
    bool flush;
    bool request_idr;
    KfReason reason;
};

FsmOut fsm_on_frame(Fsm& f, const FrameFacts& x, const FsmCfg& c);
FsmOut fsm_on_decode_error(Fsm& f);
FsmOut fsm_reset(Fsm& f, KfReason why);  // connect / disconnect

struct KfLimiter {
    bool ever;
    int64_t last_mono;
    int64_t interval;  // current retry interval (doubles up to the max)
};

// ---- state ----

struct ClientState {
    Config cfg;
    FramePool pool;
    Reasm reasm{};
    Ring<uint16_t> dq;  // decode queue
    bool decoding = false;
    uint16_t decoding_slot = 0;
    bool flush_pending = false;     // next StartDecode flushes the decoder first
    bool discard_inflight = false;  // the frame being decoded predates a flush
    Fsm fsm{};
    KfLimiter kf{};
    WinMin dwin{};
    Ring<ClientRow> pending_display;  // presented, not yet displayed
    SyncStatus client_sync{true, 0, 0};
    uint32_t ping_nonce = 0;
    ClientStatus status{};
    Ring<ClientRow> rows;
};

void client_init(ClientState& s, const Config& cfg);
void client_step(ClientState& s, const ClientEvent& ev, ClientCmds& out);

}  // namespace hs
