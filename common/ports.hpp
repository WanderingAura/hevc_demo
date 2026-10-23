// Ports: the only way the pure cores touch the outside world. Each has a real
// implementation (Asio / libav* / SDL / clock_gettime) and a sim one (sim/).
// Completions come back as events; a port never re-enters dispatch from inside
// one of its own calls.
#pragma once
#include "common/log.hpp"
#include "common/types.hpp"
#include "common/wire.hpp"

namespace hs {

struct Clock {
    virtual ~Clock() = default;
    virtual Time now() = 0;
    virtual SyncStatus sync() = 0;
};

// One timer per id; arming again replaces the previous deadline. Fires a Timer event.
struct Timers {
    virtual ~Timers() = default;
    virtual void arm(uint8_t id, int64_t at_mono_ns) = 0;
};

// Control channel, either direction. Dropped silently when not connected.
struct MsgTx {
    virtual ~MsgTx() = default;
    virtual void send(const CtrlMsg& m) = 0;
};

// Server data channel. Completion: WriteDone, then Writable once the socket
// has room again (TCP_NOTSENT_LOWAT). Errors arrive as DataDisconnected.
struct ByteTx {
    virtual ~ByteTx() = default;
    virtual void write(Bytes frame) = 0;
};

// Client data channel. Bytes arrive as DataBytes events; close() drops the
// connection (protocol error) and the port reconnects.
struct ByteRx {
    virtual ~ByteRx() = default;
    virtual void close() = 0;
};

struct EncodeParams {
    int width, height, fps;
    int bitrate_kbps, gop, vbv_ms;
    char preset[16];
};

struct EncodeJob {
    uint32_t seq;
    uint32_t slot;
    uint32_t capture_index;  // burned into the picture
    int64_t capture_wall;
    bool force_idr;
    bool reopen;  // apply params before this frame (forces an IDR)
    EncodeParams params;
};

// Fires Encoded{seq, slot, len, key, ...} with the Annex B payload in `out`.
struct Encoder {
    virtual ~Encoder() = default;
    virtual void submit(const EncodeJob& job, MutBytes out) = 0;
};

struct YuvView {
    uint8_t* plane[3];
    int stride[3];
    int width, height;
};

// The camera input.
struct FrameSource {
    virtual ~FrameSource() = default;
    virtual void fill(const YuvView& dst, uint32_t index, int64_t capture_wall) = 0;
};

struct DecodeJob {
    uint32_t seq;
    uint32_t slot;
    bool flush_before;
};

// Fires Decoded{seq, ok, ...}. present() hands the last decoded picture to the
// display, which later fires Displayed{seq}.
struct Decoder {
    virtual ~Decoder() = default;
    virtual void submit(const DecodeJob& job, Bytes au) = 0;
    virtual void present(uint32_t seq) = 0;
};

struct LogSink {
    virtual ~LogSink() = default;
    virtual void server_row(const ServerRow&) {}
    virtual void client_row(const ClientRow&) {}
    virtual void client_status(const ClientStatus&) {}
};

}  // namespace hs
