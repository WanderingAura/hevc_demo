#include "common/real.hpp"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/timex.h>
#include <time.h>

#include <fstream>
#include <sstream>

namespace hs {

static int64_t read_clock(clockid_t id) {
    timespec ts;
    clock_gettime(id, &ts);
    return int64_t(ts.tv_sec) * kSec + ts.tv_nsec;
}

Time RealClock::now() { return Time{read_clock(CLOCK_MONOTONIC), read_clock(CLOCK_REALTIME)}; }

SyncStatus RealClock::sync() {
    timex tx{};
    int state = ntp_adjtime(&tx);  // modes = 0: read only, no privileges needed
    SyncStatus s;
    s.synced = state != TIME_ERROR && !(tx.status & STA_UNSYNC);
    s.est_error_ns = int64_t(tx.esterror) * kUs;
    s.max_error_ns = int64_t(tx.maxerror) * kUs;
    return s;
}

AsioTimers::AsioTimers(asio::io_context& io, int count, Fire fire) : fire_(std::move(fire)) {
    for (int i = 0; i < count; ++i) timers_.push_back(std::make_unique<asio::steady_timer>(io));
}

void AsioTimers::arm(uint8_t id, int64_t at_mono_ns) {
    // libstdc++'s steady_clock is CLOCK_MONOTONIC, the same clock as Time::mono_ns.
    asio::steady_timer& t = *timers_[id];
    t.expires_at(asio::steady_timer::time_point(std::chrono::nanoseconds(at_mono_ns)));
    t.async_wait([this, id](const asio::error_code& ec) {
        if (!ec) fire_(id);
    });
}

CsvLog::CsvLog(const std::string& path, const char* header) {
    if (path.empty()) return;
    f_ = std::fopen(path.c_str(), "w");
    if (!f_) {
        std::fprintf(stderr, "csv: cannot open %s\n", path.c_str());
        return;
    }
    std::setvbuf(f_, nullptr, _IOLBF, 1 << 16);
    std::fprintf(f_, "%s\n", header);
}

CsvLog::~CsvLog() {
    if (f_) std::fclose(f_);
}

void CsvLog::server_row(const ServerRow& r) {
    if (!f_) return;
    char buf[512];
    format_row(r, buf, sizeof buf);
    std::fprintf(f_, "%s\n", buf);
}

void CsvLog::client_row(const ClientRow& r) {
    if (!f_) return;
    char buf[512];
    format_row(r, buf, sizeof buf);
    std::fprintf(f_, "%s\n", buf);
}

CtrlConn::CtrlConn(OnMsg on_msg, OnClose on_close) : on_msg_(std::move(on_msg)), on_close_(std::move(on_close)) {
    outbox_.reserve(1 << 16);
    inflight_.reserve(1 << 16);
}

void CtrlConn::attach(asio::ip::tcp::socket s) {
    close();
    asio::error_code ec;
    s.set_option(asio::ip::tcp::no_delay(true), ec);
    sock_ = std::make_unique<asio::ip::tcp::socket>(std::move(s));
    ++gen_;
    reader_ = CtrlReader{};
    read();
}

void CtrlConn::close() {
    if (!sock_) return;
    asio::error_code ec;
    sock_->close(ec);
    sock_.reset();
    ++gen_;
    writing_ = false;
    outbox_.clear();
    on_close_();
}

void CtrlConn::read() {
    uint32_t g = gen_;
    sock_->async_read_some(asio::buffer(rx_), [this, g](const asio::error_code& ec, size_t n) {
        if (g != gen_) return;
        if (ec) return close();
        Bytes in{rx_, uint32_t(n)};
        while (in.n) {
            CtrlFeed f = ctrl_feed(reader_, in);
            if (f.err != Err::Ok) return close();
            in.p += f.consumed;
            in.n -= f.consumed;
            if (f.done) on_msg_(f.msg);
            if (g != gen_) return;  // the handler closed us
        }
        read();
    });
}

void CtrlConn::send(const CtrlMsg& m) {
    if (!sock_ || outbox_.size() > (1 << 16)) return;  // peer not reading: drop, TCP would only delay it
    uint8_t buf[kCtrlMaxBytes];
    uint32_t n = encode_ctrl(m, buf);
    outbox_.insert(outbox_.end(), buf, buf + n);
    flush();
}

void CtrlConn::flush() {
    if (writing_ || outbox_.empty() || !sock_) return;
    inflight_.swap(outbox_);
    outbox_.clear();
    writing_ = true;
    uint32_t g = gen_;
    asio::async_write(*sock_, asio::buffer(inflight_), [this, g](const asio::error_code& ec, size_t) {
        if (g != gen_) return;
        writing_ = false;
        if (ec) return close();
        flush();
    });
}

void tune_socket(asio::ip::tcp::socket& s, const TransportCfg& t) {
    asio::error_code ec;
    s.set_option(asio::ip::tcp::no_delay(true), ec);
    if (t.sndbuf > 0) s.set_option(asio::socket_base::send_buffer_size(t.sndbuf), ec);
    if (t.tcp_notsent_lowat > 0) {
        int v = t.tcp_notsent_lowat;
        setsockopt(s.native_handle(), IPPROTO_TCP, TCP_NOTSENT_LOWAT, &v, sizeof v);
    }
}

bool load_config(Config& c, int argc, char** argv) {
    for (int pass = 0; pass < 2; ++pass) {  // file first, then overrides, whatever the order
        for (int i = 1; i < argc; ++i) {
            std::string_view a = argv[i];
            if (a.rfind("--", 0) == 0) continue;
            size_t eq = a.find('=');
            if (pass == 0 && eq == std::string_view::npos) {
                std::ifstream f(argv[i]);
                if (!f) {
                    std::fprintf(stderr, "config: cannot read %s\n", argv[i]);
                    return false;
                }
                std::stringstream ss;
                ss << f.rdbuf();
                IniError e = load_ini(c, ss.str());
                if (e.err != Err::Ok) {
                    std::fprintf(stderr, "config: %s:%d: %s\n", argv[i], e.line, err_name(e.err));
                    return false;
                }
            } else if (pass == 1 && eq != std::string_view::npos) {
                Err e = set_key(c, a.substr(0, eq), a.substr(eq + 1));
                if (e != Err::Ok) {
                    std::fprintf(stderr, "config: %s: %s\n", argv[i], err_name(e));
                    return false;
                }
            }
        }
    }
    if (validate(c) != Err::Ok) {
        std::fprintf(stderr, "config: invalid combination (check even width/height, t_stale <= t_hard, "
                             "keyframe intervals)\n");
        return false;
    }
    return true;
}

std::string flag(int argc, char** argv, const char* name) {
    size_t len = std::strlen(name);
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], name, len) != 0) continue;
        if (argv[i][len] == '=') return argv[i] + len + 1;
        if (argv[i][len] == 0) return "1";
    }
    return "";
}

}  // namespace hs
