#include "common/log.hpp"

#include <cinttypes>
#include <cstdio>

namespace hs {

const char* fsm_name(FsmState s) {
    switch (s) {
        case FsmState::WaitIdr: return "WAIT_IDR";
        case FsmState::Normal: return "NORMAL";
        case FsmState::Catchup: return "CATCHUP";
    }
    return "?";
}

const char* fate_name(Fate f) {
    switch (f) {
        case Fate::Pending: return "pending";
        case Fate::Shown: return "shown";
        case Fate::Dropped: return "dropped";
        case Fate::Hidden: return "hidden";
        case Fate::Late: return "late";
        case Fate::Flushed: return "flushed";
        case Fate::Superseded: return "superseded";
        case Fate::DecodeError: return "decode_error";
        case Fate::Count: break;
    }
    return "?";
}

const char* const kServerCsvHeader =
    "seq,gop_id,type,size,flags,capture_ts,enc_in_ts,enc_out_ts,enqueue_ts,send_start_ts,"
    "send_end_ts,dropped,drop_reason";

const char* const kClientCsvHeader =
    "seq,type,size,capture_ts,recv_first_ts,recv_last_ts,transit,age_recv,d_min,queueing,"
    "rtt_min,decode_start,decode_end,display_ts,age_show,sync_ok,est_error_us,anomaly,"
    "fsm_state,fate,shown";

int format_row(const ServerRow& r, char* buf, int cap) {
    return std::snprintf(buf, size_t(cap),
                         "%" PRId64 ",%u,%u,%u,%u,%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64
                         ",%" PRId64 ",%u,%s",
                         r.seq, r.gop_id, r.type, r.size, r.flags, r.capture_ts, r.enc_in_ts, r.enc_out_ts,
                         r.enqueue_ts, r.send_start_ts, r.send_end_ts, r.dropped,
                         drop_reason_name(DropReason(r.drop_reason)));
}

int format_row(const ClientRow& r, char* buf, int cap) {
    return std::snprintf(buf, size_t(cap),
                         "%u,%u,%u,%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64
                         ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64
                         ",%u,%" PRId64 ",%u,%s,%s,%u",
                         r.seq, r.type, r.size, r.capture_ts, r.recv_first_ts, r.recv_last_ts, r.transit,
                         r.age_recv, r.d_min, r.queueing, r.rtt_min, r.decode_start, r.decode_end,
                         r.display_ts, r.age_show, r.sync_ok, r.est_error_us, r.anomaly,
                         fsm_name(FsmState(r.fsm_state)), fate_name(Fate(r.fate)),
                         unsigned(r.fate == uint8_t(Fate::Shown)));
}

}  // namespace hs
