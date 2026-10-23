// Wire formats for the data channel (FrameHeader + Annex B) and the control channel.
// Little-endian, fixed-size PODs, serialised with memcpy.
#pragma once
#include "common/types.hpp"

namespace hs {

constexpr uint32_t kFrameMagic = 0x4D495348;  // "HSIM"
constexpr uint32_t kCtrlMagic = 0x4C544348;   // "HCTL"
constexpr uint16_t kWireVersion = 1;

enum FrameFlags : uint8_t {
    kFlagForcedIdr = 1 << 0,
    kFlagAfterDrop = 1 << 1,
};

struct FrameHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t header_len;
    uint32_t seq;  // per encoded frame: a gap means a lost reference
    uint32_t gop_id;
    uint8_t frame_type;  // 0 = P, 1 = IDR
    uint8_t flags;       // FrameFlags
    uint16_t reserved;
    uint32_t reserved1;  // explicit padding so the u64s stay 8-aligned
    int64_t capture_ts_ns;
    int64_t send_ts_ns;
    uint32_t payload_len;
    uint32_t reserved2;
};
static_assert(sizeof(FrameHeader) == 48, "FrameHeader must be 48 bytes");
constexpr uint32_t kHeaderLen = sizeof(FrameHeader);

void encode_header(const FrameHeader& h, uint8_t* out);
Result<FrameHeader> decode_header(const uint8_t* in, uint32_t max_payload);
void patch_send_ts(uint8_t* frame, int64_t wall_ns);
int64_t read_send_ts(const uint8_t* frame);

// ---- control channel ----

enum class CtrlType : uint16_t {
    None = 0,
    Ping,
    Pong,
    KeyframeReq,
    ConfigSet,
    ConfigAck,
    Stats,
};

enum class KfReason : uint8_t {
    Connect,
    Stale,
    Gap,
    DecodeError,
    QueueFull,
    Catchup,
    Retry,
    Manual,
    Count,
};
const char* kf_reason_name(KfReason r);

enum class DropReason : uint8_t {
    None,
    Overflow,
    TooOld,
    WaitingIdr,
    EncoderBusy,
    EncodeError,
    Count,
};
const char* drop_reason_name(DropReason r);

constexpr uint32_t kKeyLen = 48;
constexpr uint32_t kValLen = 48;

struct CtrlHdr {
    uint32_t magic;
    uint16_t type;
    uint16_t len;  // body bytes
};

struct PingMsg {
    uint32_t nonce;
    uint32_t pad;
    int64_t client_tx_ns;  // client CLOCK_MONOTONIC
};

struct PongMsg {
    uint32_t nonce;
    uint32_t pad;
    int64_t client_tx_ns;  // echoed
    int64_t server_rx_ns;  // server wall
    int64_t server_tx_ns;  // server wall, stamped just before send
};

struct KeyframeReqMsg {
    uint8_t reason;  // KfReason
    uint8_t pad[3];
    uint32_t last_good_seq;
};

struct ConfigSetMsg {
    char key[kKeyLen];
    char value[kValLen];
};

struct ConfigAckMsg {
    char key[kKeyLen];
    char value[kValLen];
    uint8_t status;  // Err
    uint8_t pad[3];
    uint32_t applied_at_seq;
};

struct StatsMsg {
    uint32_t queue_frames;
    uint32_t queue_bytes;
    uint32_t drops[uint32_t(DropReason::Count)];
    uint32_t idr_count;
    uint32_t keyframe_reqs;
    uint32_t next_seq;
    uint32_t pad;
    int64_t enc_last_ns;
    int64_t enc_avg_ns;
    uint8_t sync_ok;
    uint8_t pad2[7];
    int64_t est_error_ns;
    int64_t max_error_ns;
};

struct CtrlMsg {
    CtrlType type;
    union {
        PingMsg ping;
        PongMsg pong;
        KeyframeReqMsg kf;
        ConfigSetMsg set;
        ConfigAckMsg ack;
        StatsMsg stats;
    };
};

constexpr uint32_t kCtrlMaxBody = 128;
constexpr uint32_t kCtrlMaxBytes = sizeof(CtrlHdr) + kCtrlMaxBody;

// Writes header + body to out (at least kCtrlMaxBytes). Returns bytes written.
uint32_t encode_ctrl(const CtrlMsg& m, uint8_t* out);

// Incremental parser for the control byte stream.
struct CtrlReader {
    uint8_t buf[kCtrlMaxBytes];
    uint32_t have;
};

struct CtrlFeed {
    uint32_t consumed;
    bool done;  // msg is valid
    Err err;
    CtrlMsg msg;
};

CtrlFeed ctrl_feed(CtrlReader& r, Bytes in);

CtrlMsg make_ping(uint32_t nonce, int64_t client_tx_ns);
CtrlMsg make_keyframe_req(KfReason reason, uint32_t last_good_seq);
CtrlMsg make_config_set(const char* key, const char* value);

}  // namespace hs
