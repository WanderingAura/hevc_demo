#include "common/wire.hpp"

#include <cstddef>

namespace hs {

const char* err_name(Err e) {
    switch (e) {
        case Err::Ok: return "ok";
        case Err::BadMagic: return "bad_magic";
        case Err::BadVersion: return "bad_version";
        case Err::BadLength: return "bad_length";
        case Err::UnknownKey: return "unknown_key";
        case Err::NotLive: return "not_live";
        case Err::BadValue: return "bad_value";
        case Err::ParseError: return "parse_error";
        case Err::Io: return "io";
    }
    return "?";
}

const char* kf_reason_name(KfReason r) {
    switch (r) {
        case KfReason::Connect: return "connect";
        case KfReason::Stale: return "stale";
        case KfReason::Gap: return "gap";
        case KfReason::DecodeError: return "decode_error";
        case KfReason::QueueFull: return "queue_full";
        case KfReason::Catchup: return "catchup";
        case KfReason::Retry: return "retry";
        case KfReason::Manual: return "manual";
        case KfReason::Count: break;
    }
    return "?";
}

const char* drop_reason_name(DropReason r) {
    switch (r) {
        case DropReason::None: return "";
        case DropReason::Overflow: return "overflow";
        case DropReason::TooOld: return "too_old";
        case DropReason::WaitingIdr: return "waiting_idr";
        case DropReason::EncoderBusy: return "encoder_busy";
        case DropReason::EncodeError: return "encode_error";
        case DropReason::Count: break;
    }
    return "?";
}

void encode_header(const FrameHeader& h, uint8_t* out) { std::memcpy(out, &h, sizeof h); }

Result<FrameHeader> decode_header(const uint8_t* in, uint32_t max_payload) {
    FrameHeader h;
    std::memcpy(&h, in, sizeof h);
    if (h.magic != kFrameMagic) return {Err::BadMagic, h};
    if (h.version != kWireVersion) return {Err::BadVersion, h};
    if (h.header_len != kHeaderLen || h.payload_len == 0 || h.payload_len > max_payload)
        return {Err::BadLength, h};
    return {Err::Ok, h};
}

void patch_send_ts(uint8_t* frame, int64_t wall_ns) {
    std::memcpy(frame + offsetof(FrameHeader, send_ts_ns), &wall_ns, sizeof wall_ns);
}

int64_t read_send_ts(const uint8_t* frame) {
    int64_t v;
    std::memcpy(&v, frame + offsetof(FrameHeader, send_ts_ns), sizeof v);
    return v;
}

static uint32_t body_len(CtrlType t) {
    switch (t) {
        case CtrlType::Ping: return sizeof(PingMsg);
        case CtrlType::Pong: return sizeof(PongMsg);
        case CtrlType::KeyframeReq: return sizeof(KeyframeReqMsg);
        case CtrlType::ConfigSet: return sizeof(ConfigSetMsg);
        case CtrlType::ConfigAck: return sizeof(ConfigAckMsg);
        case CtrlType::Stats: return sizeof(StatsMsg);
        case CtrlType::None: break;
    }
    return 0;
}

uint32_t encode_ctrl(const CtrlMsg& m, uint8_t* out) {
    CtrlHdr h{kCtrlMagic, uint16_t(m.type), uint16_t(body_len(m.type))};
    std::memcpy(out, &h, sizeof h);
    std::memcpy(out + sizeof h, &m.ping, h.len);  // all bodies start at the union's address
    return sizeof h + h.len;
}

CtrlFeed ctrl_feed(CtrlReader& r, Bytes in) {
    CtrlFeed f{};
    while (f.consumed < in.n) {
        uint32_t need = sizeof(CtrlHdr);
        if (r.have >= sizeof(CtrlHdr)) {
            CtrlHdr h;
            std::memcpy(&h, r.buf, sizeof h);
            if (h.magic != kCtrlMagic) {
                f.err = Err::BadMagic;
                return f;
            }
            if (h.len == 0 || h.len != body_len(CtrlType(h.type))) {
                f.err = Err::BadLength;
                return f;
            }
            need += h.len;
        }
        uint32_t take = need - r.have;
        if (take > in.n - f.consumed) take = in.n - f.consumed;
        std::memcpy(r.buf + r.have, in.p + f.consumed, take);
        r.have += take;
        f.consumed += take;
        if (r.have == need && need > sizeof(CtrlHdr)) {
            CtrlHdr h;
            std::memcpy(&h, r.buf, sizeof h);
            f.msg.type = CtrlType(h.type);
            std::memcpy(&f.msg.ping, r.buf + sizeof h, h.len);
            r.have = 0;
            f.done = true;
            return f;
        }
    }
    return f;
}

CtrlMsg make_ping(uint32_t nonce, int64_t client_tx_ns) {
    CtrlMsg m{};
    m.type = CtrlType::Ping;
    m.ping.nonce = nonce;
    m.ping.client_tx_ns = client_tx_ns;
    return m;
}

CtrlMsg make_keyframe_req(KfReason reason, uint32_t last_good_seq) {
    CtrlMsg m{};
    m.type = CtrlType::KeyframeReq;
    m.kf.reason = uint8_t(reason);
    m.kf.last_good_seq = last_good_seq;
    return m;
}

CtrlMsg make_config_set(const char* key, const char* value) {
    CtrlMsg m{};
    m.type = CtrlType::ConfigSet;
    copy_cstr(m.set.key, key);
    copy_cstr(m.set.value, value);
    return m;
}

}  // namespace hs
