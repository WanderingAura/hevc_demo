// Configuration: one struct for both binaries, loaded from INI, overridden by
// `section.key=value` CLI args, and changed live through CONFIG_SET / the UI.
// All three paths go through set_key().
#pragma once
#include <string>
#include <string_view>

#include "common/types.hpp"

namespace hs {

constexpr int kMaxQueueFrames = 8;   // server send-queue pool cap (live max_frames <= this)
constexpr int kMaxDecodeQueue = 8;   // client decode-queue pool cap (live decode_queue <= this)

struct SourceCfg {
    std::string kind = "synthetic";
    int width = 1920;
    int height = 1080;
    int fps = 30;
    int sim_isp_delay_ms = 0;
    int sim_isp_jitter_ms = 0;
    int seed = 1;
};

struct EncoderCfg {
    std::string backend = "libx265";
    std::string preset = "ultrafast";
    int bitrate_kbps = 4000;
    int gop = 60;
    int vbv_ms = 200;
};

struct QueueCfg {
    int max_frames = 3;
    int max_bytes = 2'000'000;
    int max_age_ms = 200;
};

struct ServerCfg {
    QueueCfg queue;
    int stats_interval_ms = 500;
    std::string csv = "server_frames.csv";
};

struct TransportCfg {
    std::string host = "127.0.0.1";
    int data_port = 5600;
    int control_port = 5601;
    int tcp_notsent_lowat = 32768;
    int sndbuf = 131072;
    int max_frame_bytes = 2'000'000;
};

struct ClientCfg {
    int decode_threads = 4;
    int decode_queue = 2;
    int t_stale_ms = 400;
    int t_hard_ms = 500;
    int q_max_ms = 400;
    int keyframe_req_min_interval_ms = 300;
    int keyframe_req_max_interval_ms = 2000;
    int catchup_max_frames = 10;
    int ping_interval_ms = 1000;
    bool vsync = false;
    std::string csv = "client_frames.csv";
};

struct ClockCfg {
    bool require_sync = true;
    int sync_margin_ms = 2;  // -1 = auto (client esterror + server esterror)
    int max_negative_transit_ms = 1;
};

struct Config {
    SourceCfg source;
    EncoderCfg encoder;
    ServerCfg server;
    TransportCfg transport;
    ClientCfg client;
    ClockCfg clock;
};

// Which side may change a key at runtime.
enum class Live : uint8_t { No, Server, Client };

// require == Live::No: any key (startup). Otherwise the key must be live on that side.
Err set_key(Config& c, std::string_view key, std::string_view value, Live require = Live::No);
Live key_live(std::string_view key);  // Live::No also for unknown keys

struct IniError {
    Err err;
    int line;  // 1-based, 0 if none
};
IniError load_ini(Config& c, std::string_view text);

// Cross-field checks (e.g. t_stale <= t_hard, even dimensions).
Err validate(const Config& c);

std::string_view trim(std::string_view s);

}  // namespace hs
