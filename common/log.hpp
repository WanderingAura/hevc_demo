// Per-frame log rows (one per frame per side, joined offline by seq) and the
// client status snapshot. Formatting is pure; writing them is a LogSink port.
#pragma once
#include "common/types.hpp"
#include "common/wire.hpp"

namespace hs {

enum class FsmState : uint8_t { WaitIdr, Normal, Catchup };
const char* fsm_name(FsmState s);

// What finally happened to a frame on the client.
enum class Fate : uint8_t {
    Pending,
    Shown,
    Dropped,      // FSM verdict: not decoded
    Hidden,       // decoded during catch-up, not shown
    Late,         // decoded, but stale by present time
    Flushed,      // removed from the decode queue by a flush
    Superseded,   // presented, but a newer frame replaced it before display
    DecodeError,
    Count,
};
const char* fate_name(Fate f);

struct ServerRow {
    int64_t seq;  // -1: dropped before encode (never got a seq)
    uint32_t gop_id;
    uint8_t type;
    uint8_t dropped;
    uint8_t drop_reason;  // DropReason
    uint8_t flags;
    uint32_t size;
    int64_t capture_ts;
    int64_t enc_in_ts;
    int64_t enc_out_ts;
    int64_t enqueue_ts;
    int64_t send_start_ts;
    int64_t send_end_ts;
};

struct ClientRow {
    uint32_t seq;
    uint8_t type;
    uint8_t sync_ok;
    uint8_t anomaly;
    uint8_t fsm_state;  // FsmState at arrival
    uint8_t fate;       // Fate
    uint32_t size;
    int64_t capture_ts;
    int64_t recv_first_ts;
    int64_t recv_last_ts;
    int64_t transit;
    int64_t age_recv;
    int64_t d_min;
    int64_t queueing;
    int64_t rtt_min;
    int64_t decode_start;
    int64_t decode_end;
    int64_t display_ts;
    int64_t age_show;
    int64_t est_error_us;
};

// What the client UI shows; refreshed after every dispatch.
struct ClientStatus {
    bool data_connected;
    bool ages_trusted;
    FsmState fsm;
    uint32_t last_seq;
    uint32_t frames_rx;
    uint32_t kf_requests[uint32_t(KfReason::Count)];
    uint32_t fates[uint32_t(Fate::Count)];
    uint32_t anomalies;
    uint32_t decode_queue_len;
    int64_t rtt_last_ns;
    int64_t rtt_min_ns;
    int64_t margin_ns;
    SyncStatus client_sync;
    bool have_stats;
    StatsMsg server;
    bool have_ack;
    ConfigAckMsg last_ack;
    char local_key[kKeyLen];  // last client-side key set from the UI
    uint8_t local_status;     // Err
};

extern const char* const kServerCsvHeader;
extern const char* const kClientCsvHeader;
int format_row(const ServerRow& r, char* buf, int cap);
int format_row(const ClientRow& r, char* buf, int cap);

}  // namespace hs
