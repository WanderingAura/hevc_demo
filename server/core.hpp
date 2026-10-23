// Camera simulator core: pacing, simulated ISP delay, encode scheduling, the
// bounded send queue with drop-to-IDR, and control handling.
// Pure: server_step(state, event) -> commands. Time arrives in events.
#pragma once
#include "common/config.hpp"
#include "common/log.hpp"
#include "common/ports.hpp"
#include "common/wire.hpp"

namespace hs {

enum SrvTimer : uint8_t { kTimerTick, kTimerIsp, kTimerStats, kSrvTimerCount };

struct EncodedEv {
    uint32_t seq;
    uint32_t slot;
    uint32_t len;  // payload bytes written into the slot
    bool key;
    bool ok;
    int64_t enc_in_wall;
    int64_t enc_out_wall;
    int64_t enc_ns;  // monotonic duration
};

struct ServerEvent {
    enum Kind : uint8_t { Start, Timer, Encoded, WriteDone, Writable, DataConnected, DataDisconnected, Ctrl } kind;
    Time t;
    union {
        TimerEv timer;
        EncodedEv encoded;
        CtrlMsg ctrl;
    };
};

ServerEvent srv_event(ServerEvent::Kind k, Time t);
ServerEvent srv_timer(Time t, uint8_t id);
ServerEvent srv_ctrl(Time t, const CtrlMsg& m);

struct WriteCmd {
    uint32_t slot;
    uint32_t len;  // header + payload
};

struct ServerCmd {
    enum Kind : uint8_t { ArmTimer, StartEncode, Write, SendCtrl } kind;
    union {
        ArmTimerCmd arm;
        EncodeJob encode;
        WriteCmd write;
        CtrlMsg ctrl;
    };
};
using ServerCmds = FixedVec<ServerCmd, 16>;

struct Capture {
    uint32_t index;
    int64_t wall;
    int64_t mono;
    int64_t ready_mono;  // when the simulated ISP hands it to the encoder
};

// Encoded frames, laid out as [FrameHeader][payload] in one block; per-slot
// metadata in parallel arrays.
struct PacketPool {
    uint32_t slot_bytes = 0;
    std::vector<uint8_t> bytes;
    std::vector<uint32_t> len, seq, gop;
    std::vector<int64_t> capture_wall, capture_mono, enc_in, enc_out, enqueue;
    std::vector<uint8_t> key, flags, forced;
    Ring<uint16_t> free;
};

enum class TxState : uint8_t { Disconnected, Writable, Writing, WaitWritable };

struct ServerState {
    Config cfg;

    // pacing (absolute deadlines: epoch + k * period)
    int64_t tick_epoch = 0;
    int64_t tick_k = 0;
    int64_t next_tick = 0;
    uint32_t capture_index = 0;
    int64_t last_ready = 0;
    Ring<Capture> isp;
    bool has_pending = false;  // newest capture waiting for a busy encoder
    Capture pending{};

    // encoder
    bool enc_busy = false;
    bool idr_requested = false;
    bool reopen_pending = false;
    uint32_t next_seq = 0;
    uint32_t gop_id = 0;
    FixedVec<ConfigAckMsg, 4> pending_acks;  // encoder keys: acked when applied

    // send queue
    PacketPool pool;
    Ring<uint16_t> queue;
    uint32_t queue_bytes = 0;
    bool waiting_idr = false;
    TxState tx = TxState::Disconnected;
    uint16_t writing_slot = 0;

    // stats + logs
    uint32_t drops[uint32_t(DropReason::Count)] = {};
    uint32_t idr_count = 0;
    uint32_t keyframe_reqs = 0;
    int64_t enc_last_ns = 0;
    int64_t enc_avg_ns = 0;
    Ring<ServerRow> rows;
    Rng rng{1};
};

// Allocates every buffer the server will use.
void server_init(ServerState& s, const Config& cfg);
void server_step(ServerState& s, const ServerEvent& ev, ServerCmds& out);

uint8_t* slot_ptr(PacketPool& p, uint32_t slot);
EncodeParams encode_params(const Config& c);

}  // namespace hs
