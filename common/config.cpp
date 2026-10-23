#include "common/config.hpp"

#include <charconv>
#include <climits>

namespace hs {

namespace {

enum class Kind : uint8_t { Int, Bool, Str, Margin };

struct KeyDef {
    const char* name;
    Kind kind;
    Live live;
    int lo, hi;
    const char* choices;  // Str only: space-separated allowed values, nullptr = any
    void* (*field)(Config&);
};

#define KEY(kind, name, live, lo, hi, choices, member) \
    {name, Kind::kind, Live::live, lo, hi, choices, [](Config& c) -> void* { return &c.member; }}

const char* const kPresets =
    "ultrafast superfast veryfast faster fast medium slow slower veryslow placebo";

const KeyDef kKeys[] = {
    KEY(Str, "source.kind", No, 0, 0, "synthetic", source.kind),
    KEY(Int, "source.width", No, 16, 7680, nullptr, source.width),
    KEY(Int, "source.height", No, 16, 4320, nullptr, source.height),
    KEY(Int, "source.fps", Server, 1, 240, nullptr, source.fps),
    KEY(Int, "source.sim_isp_delay_ms", Server, 0, 5000, nullptr, source.sim_isp_delay_ms),
    KEY(Int, "source.sim_isp_jitter_ms", Server, 0, 1000, nullptr, source.sim_isp_jitter_ms),
    KEY(Int, "source.seed", No, 0, INT_MAX, nullptr, source.seed),
    KEY(Str, "encoder.backend", No, 0, 0, "libx265", encoder.backend),
    KEY(Str, "encoder.preset", Server, 0, 0, kPresets, encoder.preset),
    KEY(Int, "encoder.bitrate_kbps", Server, 100, 200000, nullptr, encoder.bitrate_kbps),
    KEY(Int, "encoder.gop", Server, 1, 1000, nullptr, encoder.gop),
    KEY(Int, "encoder.vbv_ms", Server, 10, 10000, nullptr, encoder.vbv_ms),
    KEY(Int, "server.queue.max_frames", Server, 1, kMaxQueueFrames, nullptr, server.queue.max_frames),
    KEY(Int, "server.queue.max_bytes", Server, 10000, 100'000'000, nullptr, server.queue.max_bytes),
    KEY(Int, "server.queue.max_age_ms", Server, 1, 60000, nullptr, server.queue.max_age_ms),
    KEY(Int, "server.stats_interval_ms", No, 50, 60000, nullptr, server.stats_interval_ms),
    KEY(Str, "server.csv", No, 0, 0, nullptr, server.csv),
    KEY(Str, "transport.host", No, 0, 0, nullptr, transport.host),
    KEY(Int, "transport.data_port", No, 1, 65535, nullptr, transport.data_port),
    KEY(Int, "transport.control_port", No, 1, 65535, nullptr, transport.control_port),
    KEY(Int, "transport.tcp_notsent_lowat", No, 0, 16 << 20, nullptr, transport.tcp_notsent_lowat),
    KEY(Int, "transport.sndbuf", No, 0, 64 << 20, nullptr, transport.sndbuf),
    KEY(Int, "transport.max_frame_bytes", No, 1024, 64 << 20, nullptr, transport.max_frame_bytes),
    KEY(Int, "client.decode_threads", No, 0, 64, nullptr, client.decode_threads),
    KEY(Int, "client.decode_queue", Client, 1, kMaxDecodeQueue, nullptr, client.decode_queue),
    KEY(Int, "client.t_stale_ms", Client, 1, 60000, nullptr, client.t_stale_ms),
    KEY(Int, "client.t_hard_ms", Client, 1, 60000, nullptr, client.t_hard_ms),
    KEY(Int, "client.q_max_ms", Client, 1, 60000, nullptr, client.q_max_ms),
    KEY(Int, "client.keyframe_req_min_interval_ms", Client, 10, 60000, nullptr, client.keyframe_req_min_interval_ms),
    KEY(Int, "client.keyframe_req_max_interval_ms", Client, 10, 60000, nullptr, client.keyframe_req_max_interval_ms),
    KEY(Int, "client.catchup_max_frames", Client, 1, 1000, nullptr, client.catchup_max_frames),
    KEY(Int, "client.ping_interval_ms", No, 50, 60000, nullptr, client.ping_interval_ms),
    KEY(Bool, "client.vsync", No, 0, 0, nullptr, client.vsync),
    KEY(Str, "client.csv", No, 0, 0, nullptr, client.csv),
    KEY(Bool, "clock.require_sync", Client, 0, 0, nullptr, clock.require_sync),
    KEY(Margin, "clock.sync_margin_ms", Client, 0, 10000, nullptr, clock.sync_margin_ms),
    KEY(Int, "clock.max_negative_transit_ms", Client, 0, 10000, nullptr, clock.max_negative_transit_ms),
};

#undef KEY

const KeyDef* find_key(std::string_view key) {
    for (const KeyDef& k : kKeys)
        if (key == k.name) return &k;
    return nullptr;
}

bool parse_int(std::string_view v, int& out) {
    auto r = std::from_chars(v.data(), v.data() + v.size(), out);
    return r.ec == std::errc() && r.ptr == v.data() + v.size();
}

bool parse_bool(std::string_view v, bool& out) {
    if (v == "true" || v == "1" || v == "yes" || v == "on") return out = true, true;
    if (v == "false" || v == "0" || v == "no" || v == "off") return out = false, true;
    return false;
}

bool in_choices(const char* choices, std::string_view v) {
    std::string_view rest(choices);
    while (!rest.empty()) {
        size_t sp = rest.find(' ');
        if (rest.substr(0, sp) == v) return true;
        if (sp == std::string_view::npos) break;
        rest.remove_prefix(sp + 1);
    }
    return false;
}

}  // namespace

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

Live key_live(std::string_view key) {
    const KeyDef* k = find_key(key);
    return k ? k->live : Live::No;
}

Err set_key(Config& c, std::string_view key, std::string_view value, Live require) {
    const KeyDef* k = find_key(key);
    if (!k) return Err::UnknownKey;
    if (require != Live::No && k->live != require) return Err::NotLive;
    value = trim(value);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
    void* f = k->field(c);
    switch (k->kind) {
        case Kind::Int:
        case Kind::Margin: {
            int v;
            if (k->kind == Kind::Margin && value == "auto") {
                *static_cast<int*>(f) = -1;
                return Err::Ok;
            }
            if (!parse_int(value, v) || v < k->lo || v > k->hi) return Err::BadValue;
            *static_cast<int*>(f) = v;
            return Err::Ok;
        }
        case Kind::Bool: {
            bool v;
            if (!parse_bool(value, v)) return Err::BadValue;
            *static_cast<bool*>(f) = v;
            return Err::Ok;
        }
        case Kind::Str:
            if (k->choices && !in_choices(k->choices, value)) return Err::BadValue;
            *static_cast<std::string*>(f) = std::string(value);
            return Err::Ok;
    }
    return Err::BadValue;
}

IniError load_ini(Config& c, std::string_view text) {
    std::string section;
    int line_no = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string_view::npos) eol = text.size();
        std::string_view line = text.substr(pos, eol - pos);
        pos = eol + 1;
        ++line_no;
        size_t comment = line.find_first_of(";#");
        if (comment != std::string_view::npos) line = line.substr(0, comment);
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[') {
            if (line.back() != ']') return {Err::ParseError, line_no};
            section = std::string(trim(line.substr(1, line.size() - 2)));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string_view::npos) return {Err::ParseError, line_no};
        std::string key(trim(line.substr(0, eq)));
        if (!section.empty()) key = section + "." + key;
        Err e = set_key(c, key, line.substr(eq + 1));
        if (e != Err::Ok) return {e, line_no};
    }
    return {Err::Ok, 0};
}

Err validate(const Config& c) {
    if (c.source.width % 2 || c.source.height % 2) return Err::BadValue;
    if (c.client.t_stale_ms > c.client.t_hard_ms) return Err::BadValue;
    if (c.client.keyframe_req_min_interval_ms > c.client.keyframe_req_max_interval_ms) return Err::BadValue;
    return Err::Ok;
}

}  // namespace hs
